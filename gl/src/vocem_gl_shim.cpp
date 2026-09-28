// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Vocem Overlay - OpenGL shim.
//
// OpenGL has no layer mechanism, so reaching GL games with no setup means being
// preloaded into every process of the session. This file is the only thing
// preloaded: a few kilobytes that interpose the present, context-teardown and
// lookup functions and nothing else. On the first real GL frame it loads the
// overlay library and hands the frame over, so only processes that draw pay
// for anything beyond one small mapping.

// Invariants: no C++ runtime (a function-local static with a dynamic
// initialiser emits __cxa_guard, pulling libstdc++ into every process), so
// plain globals and atomic builtins only; NEEDED is libc.so.6 alone at both
// widths (tests/shim_artifact.cmake); exactly the 12 hooks below are exported.

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "real_dlsym.h"

namespace {

using PFN_dlsym = void* (*)(void*, const char*);
PFN_dlsym g_real_dlsym = nullptr;

// The real dlsym, reached through dlvsym so it bypasses our own interposed
// dlsym. The version is tried from real_dlsym.h's list, never assumed.
void* real_dlsym(void* handle, const char* name) {
    PFN_dlsym real = __atomic_load_n(&g_real_dlsym, __ATOMIC_ACQUIRE);
    if (!real) {
        static const char* const versions[] = VOCEM_DLSYM_VERSIONS;
        for (unsigned i = 0; i < sizeof(versions) / sizeof(versions[0]) && !real; ++i) {
            real = reinterpret_cast<PFN_dlsym>(dlvsym(RTLD_NEXT, "dlsym", versions[i]));
        }
        // Without it no lookup can be answered. Say so once per process (a
        // separate flag, since the null itself is never cached): the
        // application will report its own symbol failing, pointing elsewhere.
        static int said = 0;
        if (!real && __atomic_exchange_n(&said, 1, __ATOMIC_ACQ_REL) == 0) {
            fprintf(stderr,
                    "[vocem] could not find the real dlsym: every symbol lookup in this "
                    "process will fail. Start it with LD_PRELOAD cleared, and please "
                    "report this.\n");
        }
        __atomic_store_n(&g_real_dlsym, real, __ATOMIC_RELEASE);
    }
    return real ? real(handle, name) : nullptr;
}

using PFN_present_glx = void (*)(void*, unsigned long);
using PFN_present_egl = void (*)(void*, void*);
using PFN_context_gone = void (*)(void*, void*);

// Loaded lazily, once, on the first presented frame.
PFN_present_glx g_present_glx = nullptr;
PFN_present_egl g_present_egl = nullptr;
PFN_context_gone g_context_gone_glx = nullptr;
PFN_context_gone g_context_gone_egl = nullptr;
int g_load_attempted = 0;

// -1 unknown, 0 enabled, 1 disabled.
int g_disabled = -1;

bool disabled() {
    int value = __atomic_load_n(&g_disabled, __ATOMIC_ACQUIRE);
    if (value < 0) {
        const char* env = getenv("VOCEM_DISABLE");
        value = (env && env[0] == '1') ? 1 : 0;
        __atomic_store_n(&g_disabled, value, __ATOMIC_RELEASE);
    }
    return value == 1;
}

// A line on stderr without stdio: write(2) of the pieces, no buffer, no
// allocation. Used once per process at most, on the present path, for the one
// failure this file can have that looks exactly like success.
void say(const char* a, const char* b = "", const char* c = "", const char* d = "") {
    const char* parts[] = {"[vocem/gl-shim] ", a, b, c, d, "\n"};
    for (const char* part : parts) {
        size_t length = strlen(part);
        while (length > 0) {
            const ssize_t written = write(2, part, length);
            if (written <= 0) {
                return;
            }
            part += written;
            length -= static_cast<size_t>(written);
        }
    }
}

// One attempt at the overlay library. Under VOCEM_DEBUG a refusal is said in
// ld.so's words: the application record is written by the library that did
// not load, so nothing else would say it (tests/gl_old_libstdcxx.cpp).
void* try_load(const char* path, bool debug) {
    void* handle = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!handle && debug) {
        const char* why = dlerror();
        say("could not load ", path, ": ", why ? why : "no reason given");
    }
    return handle;
}

void load_overlay() {
    if (__atomic_load_n(&g_load_attempted, __ATOMIC_ACQUIRE)) {
        return;
    }
    __atomic_store_n(&g_load_attempted, 1, __ATOMIC_RELEASE);
    const char* debug_env = getenv("VOCEM_DEBUG");
    const bool debug = debug_env && debug_env[0] == '1';

    // VOCEM_GL_LIBRARY names an absolute path (a build tree) and, when set, is
    // the only path tried: falling back to the installed library would silently
    // pair this tree's shim with the package's overlay.
    const char* override_path = getenv("VOCEM_GL_LIBRARY");
    const bool overridden = override_path && override_path[0];

    // RTLD_LOCAL: the overlay's symbols must not leak into the application's global
    // namespace, where they could shadow something it defines itself.
    void* handle = try_load(overridden ? override_path : "libvocem_gl.so", debug);

    // Inside a container, by its path on the host. The Steam Linux Runtime has
    // its own /usr, where the soname does not resolve; pressure-vessel copies
    // this shim in (as every LD_PRELOAD module) but knows nothing of the
    // library it opens, and mounts the host at /run/host. VOCEM_LIBDIR rather
    // than $LIB: it is where this build's package puts the file.
    //
    // These dlopens are on the present path, after a real frame, never in a
    // resolution path: dlopen walks the filesystem, and resolution runs inside
    // sandboxed processes where a file syscall is SIGSYS.
    if (!handle && !overridden) {
        handle = try_load("/run/host" VOCEM_LIBDIR "/libvocem_gl.so", debug);
    }
    // And by its own path, unprefixed: inside a Flatpak the overlay is mounted
    // from the VulkanLayer extension, which has no add-ld-path and no /run/host;
    // that build compiles VOCEM_LIBDIR to the mount point.
    if (!handle && !overridden) {
        handle = try_load(VOCEM_LIBDIR "/libvocem_gl.so", debug);
    }
    if (!handle) {
        // Not installed, or the wrong architecture, or refused: stay out of the
        // way -- and, when asked, say that this process has no overlay, so the
        // lines above are read as the verdict and not as noise.
        if (debug) {
            say("no overlay in this process: the overlay library did not load");
        }
        return;
    }
    // Atomic stores: a second presenting thread reads these once it sees
    // g_load_attempted set.
    __atomic_store_n(&g_present_glx,
                     reinterpret_cast<PFN_present_glx>(real_dlsym(handle, "vocem_gl_present_glx")),
                     __ATOMIC_RELEASE);
    __atomic_store_n(&g_present_egl,
                     reinterpret_cast<PFN_present_egl>(real_dlsym(handle, "vocem_gl_present_egl")),
                     __ATOMIC_RELEASE);
    __atomic_store_n(
        &g_context_gone_glx,
        reinterpret_cast<PFN_context_gone>(real_dlsym(handle, "vocem_gl_context_destroyed")),
        __ATOMIC_RELEASE);
    __atomic_store_n(
        &g_context_gone_egl,
        reinterpret_cast<PFN_context_gone>(real_dlsym(handle, "vocem_gl_egl_context_destroyed")),
        __ATOMIC_RELEASE);
}

// The overlay is told a context is going only if it was ever loaded. A process that
// never presented a frame has nothing to give back, and must not be made to load a
// megabyte of overlay just to be told so.
void context_gone(bool egl, void* display, void* context) {
    PFN_context_gone gone = __atomic_load_n(egl ? &g_context_gone_egl : &g_context_gone_glx,
                                            __ATOMIC_ACQUIRE);
    if (gone) {
        gone(display, context);
    }
}

}  // namespace

// ---------------------------------------------------------------------------
// The three ways a program reaches a present function; all three are needed.
//
//   1. The program's own call, resolved by the dynamic linker: we are ahead of
//      libGL in the global scope. The only interposable point in the chain --
//      glvnd links its libraries -Bsymbolic, so nothing downstream is hookable.
//   2. glXGetProcAddress / eglGetProcAddress: they return glvnd's internal
//      pointer, so without these hooks every GL loader (glad, GLEW, epoxy)
//      bypasses us.
//   3. dlsym: SDL, GLFW and glad dlopen the GL library and dlsym on that
//      handle, which never searches the global scope. Most games arrive here.
//
// One table (g_hooks) serves 2 and 3, so the two cannot drift apart.

// Every present entry point, and the dispatch functions themselves. Declared with
// C linkage here because they are defined that way below, and the table needs their
// addresses before the definitions exist.
extern "C" {

void glXSwapBuffers(void* display, unsigned long drawable);
long long glXSwapBuffersMscOML(void* display, unsigned long drawable, long long target_msc,
                               long long divisor, long long remainder);
unsigned int eglSwapBuffers(void* display, void* surface);
unsigned int eglSwapBuffersWithDamageEXT(void* display, void* surface, int* rects, int n_rects);
unsigned int eglSwapBuffersWithDamageKHR(void* display, void* surface, int* rects, int n_rects);
void* glXGetProcAddress(const unsigned char* name);
void* glXGetProcAddressARB(const unsigned char* name);
void* eglGetProcAddress(const char* name);
void glXDestroyContext(void* display, void* context);
unsigned int eglDestroyContext(void* display, void* context);
unsigned int eglTerminate(void* display);

}  // extern "C"

namespace {

// ---------------------------------------------------------------------------
// Which pointers the shim may remember.
//
// A process can hold more than one implementation of these names: every
// Electron app has ANGLE's bundled libEGL.so and the system's libEGL.so.1
// beneath it. Forwarding ANGLE's question back to ANGLE deadlocks its GPU
// process, and dlsym(RTLD_DEFAULT, ...) returns the shim's own export, a
// forward-to-self loop (tests/shim_two_egls.cpp). So a pointer is remembered
// only when it belongs to the system GL stack -- glvnd or a vendor ICD, of
// which there is one per process. A private GL keeps its names untouched; the
// overlay still sees every frame because ANGLE presents through the system
// library.
//
// dladdr answers "whose pointer is this" from memory, with no file syscall
// (this runs in sandboxed processes). The trailing dot is load-bearing: the
// system library is libEGL.so.1..., ANGLE's is exactly libEGL.so.
bool is_system_gl(void* pointer) {
    Dl_info info;
    if (!pointer || dladdr(pointer, &info) == 0 || !info.dli_fname) {
        return false;
    }
    const char* base = strrchr(info.dli_fname, '/');
    base = base ? base + 1 : info.dli_fname;
    static const char* const prefixes[] = {
        // libglvnd's dispatchers, versioned -- the dot excludes ANGLE's "libEGL.so".
        "libEGL.so.", "libGLX.so.", "libGL.so.", "libOpenGL.so.",
        "libGLdispatch.so.", "libGLESv2.so.", "libGLESv1_CM.so.",
        // Vendor ICDs: libEGL_nvidia.so.0, libGLX_mesa.so.0, and their kin.
        "libEGL_", "libGLX_",
    };
    for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        if (strncmp(base, prefixes[i], strlen(prefixes[i])) == 0) {
            return true;
        }
    }
    return false;
}

// One entry per interposed name, with two remembered pointers:
//
//   `seen`: what the dlsym hook saw the application resolve, vetted by
//   is_system_gl(). The only way to reach a GL library opened RTLD_LOCAL.
//
//   `next`: RTLD_NEXT, for an application that linked its GL normally. Not
//   vetted: at level 1 whatever is next in the global scope is the only
//   correct place to forward to.
//
// Neither is cached as null (the first frame can precede the library being
// mapped), yet RTLD_NEXT is looked up once, the attempt kept in a flag: the
// lookup takes the loader's lock, which a child forked from Chromium's zygote
// may find held. The dlsym hook can still fill `seen` later, atomically.
//
// No dlopen of any kind here: dlopen(RTLD_NOLOAD) of an unmapped soname walks
// the search path first, which is SIGSYS under Chromium's seccomp filters
// (tests/shim_seccomp.cpp).
struct Hook {
    const char* name;
    void* hook;
    void* seen;
    void* next;
    int next_attempted;
};

Hook g_hooks[] = {
    {"glXSwapBuffers", reinterpret_cast<void*>(&glXSwapBuffers), nullptr, nullptr, 0},
    // A second, separately exported GLX present entry point.
    {"glXSwapBuffersMscOML", reinterpret_cast<void*>(&glXSwapBuffersMscOML), nullptr, nullptr, 0},
    {"eglSwapBuffers", reinterpret_cast<void*>(&eglSwapBuffers), nullptr, nullptr, 0},
    // Damage-aware presents: libEGL.so.1 does not export these, so they are
    // reachable only through eglGetProcAddress (doors 2 and 3).
    {"eglSwapBuffersWithDamageEXT", reinterpret_cast<void*>(&eglSwapBuffersWithDamageEXT), nullptr,
     nullptr, 0},
    {"eglSwapBuffersWithDamageKHR", reinterpret_cast<void*>(&eglSwapBuffersWithDamageKHR), nullptr,
     nullptr, 0},
    // The dispatch functions have to hand back *themselves*, or a program that
    // fetches `glXGetProcAddress` through dlsym gets the real one and step 2 above
    // is bypassed along with it.
    {"glXGetProcAddress", reinterpret_cast<void*>(&glXGetProcAddress), nullptr, nullptr, 0},
    {"glXGetProcAddressARB", reinterpret_cast<void*>(&glXGetProcAddressARB), nullptr, nullptr, 0},
    {"eglGetProcAddress", reinterpret_cast<void*>(&eglGetProcAddress), nullptr, nullptr, 0},
    // Context teardown: what the overlay built in a context (program, buffer,
    // textures) must be released before the context goes.
    {"glXDestroyContext", reinterpret_cast<void*>(&glXDestroyContext), nullptr, nullptr, 0},
    {"eglDestroyContext", reinterpret_cast<void*>(&eglDestroyContext), nullptr, nullptr, 0},
    {"eglTerminate", reinterpret_cast<void*>(&eglTerminate), nullptr, nullptr, 0},
};

Hook* find_hook(const char* name) {
    if (!name) {
        return nullptr;
    }
    for (Hook& hook : g_hooks) {
        if (strcmp(hook.name, name) == 0) {
            return &hook;
        }
    }
    return nullptr;
}

// The real function behind a hook: `seen`, else RTLD_NEXT looked up once. Null
// when neither has an answer yet.
//
// The answer is stored before the attempt is marked: the other order would
// tell threads arriving in between that the function does not exist
// (tests/shim_lookup_race.cpp). Threads arriving together may each look the
// name up, which is harmless; the flag still stops it repeating.
void* real_for(Hook& entry) {
    if (void* seen = __atomic_load_n(&entry.seen, __ATOMIC_ACQUIRE)) {
        return seen;
    }
    if (void* next = __atomic_load_n(&entry.next, __ATOMIC_ACQUIRE)) {
        return next;
    }
    if (__atomic_load_n(&entry.next_attempted, __ATOMIC_ACQUIRE)) {
        // Read again: the finished attempt may have stored its answer after
        // `next` was read above, and the acquire on the flag is what makes
        // that store visible now.
        return __atomic_load_n(&entry.next, __ATOMIC_ACQUIRE);
    }
    void* next = real_dlsym(RTLD_NEXT, entry.name);
    if (next) {
        __atomic_store_n(&entry.next, next, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&entry.next_attempted, 1, __ATOMIC_RELEASE);
    return next;
}

void* real_for_name(const char* name) {
    Hook* entry = find_hook(name);
    return entry ? real_for(*entry) : nullptr;
}

using PFN_egl_get_proc = void* (*)(const char*);

// The real eglGetProcAddress, for the entry points libEGL does not export and
// which therefore cannot be reached by symbol lookup at all.
void* real_egl_proc(const char* name) {
    PFN_egl_get_proc real =
        reinterpret_cast<PFN_egl_get_proc>(real_for_name("eglGetProcAddress"));
    return real ? real(name) : nullptr;
}

// The damage-aware presents get slots of their own, so the dispatcher (and
// glvnd's lock) is asked once, not on every frame. As everywhere: only
// system-GL pointers are remembered, never a null (entry 36).
void* g_swap_damage_ext = nullptr;
void* g_swap_damage_khr = nullptr;

void* real_damage_swap(const char* name, void** slot) {
    if (void* cached = __atomic_load_n(slot, __ATOMIC_ACQUIRE)) {
        return cached;
    }
    void* found = real_egl_proc(name);
    if (found && is_system_gl(found)) {
        void* expected = nullptr;
        __atomic_compare_exchange_n(slot, &expected, found, false, __ATOMIC_RELEASE,
                                    __ATOMIC_RELAXED);
    }
    return found;
}

// Hand the frame over, once, before the real present puts it on the screen.
void present_glx(void* display, unsigned long drawable) {
    if (disabled()) {
        return;
    }
    load_overlay();
    if (PFN_present_glx present = __atomic_load_n(&g_present_glx, __ATOMIC_ACQUIRE)) {
        present(display, drawable);
    }
}

void present_egl(void* display, void* surface) {
    if (disabled()) {
        return;
    }
    load_overlay();
    if (PFN_present_egl present = __atomic_load_n(&g_present_egl, __ATOMIC_ACQUIRE)) {
        present(display, surface);
    }
}

// The shared body of the three dispatch hooks. The real dispatcher is asked
// first: a dispatcher also answers whether a name exists, and its answer is
// the only place ANGLE's native present functions are ever seen (ANGLE does
// not use dlsym for them). Our hook is offered only when the real function
// exists -- a dispatcher that answers null to every other name is a broken EGL.
//
// Only for a name of the dispatcher's own family (egl* from eglGetProcAddress,
// glX* from the GLX ones): glvnd answers the other family's names with a
// libGLdispatch stub that passes is_system_gl() and does nothing, which would
// hold the slot for good (tests/shim_dispatch_family.cpp).
bool same_family(const Hook& dispatcher, const char* name) {
    const bool egl_dispatcher = dispatcher.name[0] == 'e';
    return strncmp(name, egl_dispatcher ? "egl" : "glX", 3) == 0;
}

void* dispatch(Hook& own, const char* name) {
    void* real = real_for(own);
    if (!real) {
        return nullptr;
    }
    void* answer = reinterpret_cast<PFN_egl_get_proc>(real)(name);
    if (answer && !disabled() && name && same_family(own, name)) {
        if (Hook* entry = find_hook(name)) {
            if (is_system_gl(answer)) {
                void* expected = nullptr;
                __atomic_compare_exchange_n(&entry->seen, &expected, answer, false,
                                            __ATOMIC_RELEASE, __ATOMIC_RELAXED);
            }
            return entry->hook;
        }
    }
    return answer;
}

}  // namespace

extern "C" {

void glXSwapBuffers(void* display, unsigned long drawable) {
    using PFN = void (*)(void*, unsigned long);
    PFN real = reinterpret_cast<PFN>(real_for_name("glXSwapBuffers"));
    present_glx(display, drawable);
    if (real) {
        real(display, drawable);
    }
}

long long glXSwapBuffersMscOML(void* display, unsigned long drawable, long long target_msc,
                               long long divisor, long long remainder) {
    using PFN = long long (*)(void*, unsigned long, long long, long long, long long);
    PFN real = reinterpret_cast<PFN>(real_for_name("glXSwapBuffersMscOML"));
    present_glx(display, drawable);
    return real ? real(display, drawable, target_msc, divisor, remainder) : -1;
}

unsigned int eglSwapBuffers(void* display, void* surface) {
    using PFN = unsigned int (*)(void*, void*);
    PFN real = reinterpret_cast<PFN>(real_for_name("eglSwapBuffers"));
    present_egl(display, surface);
    return real ? real(display, surface) : 0u;
}

unsigned int eglSwapBuffersWithDamageEXT(void* display, void* surface, int* rects, int n_rects) {
    using PFN = unsigned int (*)(void*, void*, int*, int);
    PFN real = reinterpret_cast<PFN>(
        real_damage_swap("eglSwapBuffersWithDamageEXT", &g_swap_damage_ext));
    present_egl(display, surface);
    // The damage rectangles are the application's and are passed through
    // untouched. What we drew outside them may not reach the screen -- that is the
    // point of the extension -- which is a known limit rather than a fix.
    return real ? real(display, surface, rects, n_rects) : 0u;
}

unsigned int eglSwapBuffersWithDamageKHR(void* display, void* surface, int* rects, int n_rects) {
    using PFN = unsigned int (*)(void*, void*, int*, int);
    PFN real = reinterpret_cast<PFN>(
        real_damage_swap("eglSwapBuffersWithDamageKHR", &g_swap_damage_khr));
    present_egl(display, surface);
    return real ? real(display, surface, rects, n_rects) : 0u;
}

// The end of a context. Told first, destroyed after: while the real call has not
// run yet the context still exists, which is the only state in which what is inside
// it can be given back.
void glXDestroyContext(void* display, void* context) {
    using PFN = void (*)(void*, void*);
    PFN real = reinterpret_cast<PFN>(real_for_name("glXDestroyContext"));
    context_gone(false, display, context);
    if (real) {
        real(display, context);
    }
}

unsigned int eglDestroyContext(void* display, void* context) {
    using PFN = unsigned int (*)(void*, void*);
    PFN real = reinterpret_cast<PFN>(real_for_name("eglDestroyContext"));
    context_gone(true, display, context);
    return real ? real(display, context) : 0u;
}

// The whole display goes, and every context on it with it. No particular context to
// make current, so the overlay is told with none and drops its state without
// touching GL.
unsigned int eglTerminate(void* display) {
    using PFN = unsigned int (*)(void*);
    PFN real = reinterpret_cast<PFN>(real_for_name("eglTerminate"));
    context_gone(true, display, nullptr);
    return real ? real(display) : 0u;
}

// Applications that resolve entry points dynamically have to receive our hooks as
// well, or the preload is bypassed for the one function that matters. Answered from
// the shared table, so adding a present function in one place adds it everywhere.
void* glXGetProcAddress(const unsigned char* name) {
    return dispatch(*find_hook("glXGetProcAddress"), reinterpret_cast<const char*>(name));
}

void* glXGetProcAddressARB(const unsigned char* name) {
    return dispatch(*find_hook("glXGetProcAddressARB"), reinterpret_cast<const char*>(name));
}

void* eglGetProcAddress(const char* name) {
    return dispatch(*find_hook("eglGetProcAddress"), name);
}

// The main door, not a last line of defence: SDL, GLFW and glad reach the
// present through dlopen + dlsym on that handle. VOCEM_NO_DLSYM=1 turns it off.
//
// The real lookup runs first and the hook is substituted only on success, and
// only when the pointer belongs to the system GL stack (is_system_gl()):
// dlsym is also how a program asks whether something exists. With
// VOCEM_DISABLE=1 nothing is remembered or replaced -- the way out of a broken
// interposition switches off the interposition, not just the drawing.
//
// Known limit: interposing dlsym makes the application's RTLD_NEXT and
// RTLD_DEFAULT resolve relative to this library. MangoHud's workaround
// (return address + dlopen(RTLD_NOLOAD)) is not copied: it crashes on NVIDIA.
void* dlsym(void* handle, const char* name) {
    static int hook_enabled = -1;  // -1 unknown, 0 off, 1 on; the disabled() shape
    int enabled = __atomic_load_n(&hook_enabled, __ATOMIC_ACQUIRE);
    if (enabled < 0) {
        const char* env = getenv("VOCEM_NO_DLSYM");
        enabled = (env && env[0] == '1') ? 0 : 1;
        __atomic_store_n(&hook_enabled, enabled, __ATOMIC_RELEASE);
    }

    void* real = real_dlsym(handle, name);
    if (enabled == 1 && real && !disabled()) {
        if (Hook* entry = find_hook(name)) {
            if (is_system_gl(real)) {
                // The only moment the real function of an RTLD_LOCAL library is
                // visible. First one wins: one system GL stack per process.
                void* expected = nullptr;
                __atomic_compare_exchange_n(&entry->seen, &expected, real, false,
                                            __ATOMIC_RELEASE, __ATOMIC_RELAXED);
                return entry->hook;
            }
        }
    }
    return real;
}

}  // extern "C"

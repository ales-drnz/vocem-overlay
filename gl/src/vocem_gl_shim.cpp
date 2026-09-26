// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Vocem Overlay - OpenGL shim.
//
// Vulkan games are covered automatically: the Vulkan loader loads our layer, in
// every application, with nothing to configure. OpenGL has no such mechanism, so
// the only way to reach OpenGL games without asking the user to do anything is to
// be preloaded into the session -- and that means being preloaded into *everything*
// the session starts, browsers and shells included.
//
// This file is what makes that defensible. It is the only thing preloaded: a few
// kilobytes that interpose the swap functions and do nothing else. No ImGui, no
// image decoding, no configuration parsing, no static constructors that touch
// anything. A process that never presents an OpenGL frame -- which is almost all of
// them -- pays for one small mapping and nothing more.
//
// The first time a real GL frame is presented, the shim loads the actual overlay
// and hands the frame over. Everything expensive lives there, and is therefore only
// ever paid for by processes that draw.

// Written without the C++ runtime on purpose. A function-local static with a
// dynamic initialiser would emit __cxa_guard calls, which pull in libstdc++ -- and
// this object is mapped into every process in the session, including ones that
// never load libstdc++ themselves. Plain globals plus atomic builtins keep the
// dependency list at libc and libdl.

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "real_dlsym.h"

namespace {

using PFN_dlsym = void* (*)(void*, const char*);
PFN_dlsym g_real_dlsym = nullptr;

// The real dlsym, reached through dlvsym so it bypasses our own interposed dlsym
// below: asking the interposed one for a symbol would return our own hook.
//
// The version has to be looked for and not assumed -- see real_dlsym.h for what
// assuming it cost. Tried in order, this architecture's own first.
void* real_dlsym(void* handle, const char* name) {
    PFN_dlsym real = __atomic_load_n(&g_real_dlsym, __ATOMIC_ACQUIRE);
    if (!real) {
        static const char* const versions[] = VOCEM_DLSYM_VERSIONS;
        for (unsigned i = 0; i < sizeof(versions) / sizeof(versions[0]) && !real; ++i) {
            real = reinterpret_cast<PFN_dlsym>(dlvsym(RTLD_NEXT, "dlsym", versions[i]));
        }
        // Nothing sensible is left to do here -- there is no way to answer a dlsym
        // without one -- so at least say so once, on the way down. A single line
        // naming this file is the difference between an afternoon and a minute:
        // what the application reports is its own symbol failing, which points
        // anywhere but here.
        // Said once, not once per lookup. The null is deliberately not cached --
        // a null resolution is never remembered here -- so without a separate
        // flag this printed a line on every dlsym for the life of the process,
        // in every process in the session, which is noise rather than a report.
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

void load_overlay() {
    if (__atomic_load_n(&g_load_attempted, __ATOMIC_ACQUIRE)) {
        return;
    }
    __atomic_store_n(&g_load_attempted, 1, __ATOMIC_RELEASE);

    // An absolute path can be given explicitly, which is what running from a build
    // tree needs: without it the overlay would have to be on the library search
    // path, and putting a build directory there for every process in the session is
    // a worse trade than one extra variable.
    const char* override_path = getenv("VOCEM_GL_LIBRARY");

    // RTLD_LOCAL: the overlay's symbols must not leak into the application's global
    // namespace, where they could shadow something it defines itself.
    void* handle = dlopen(override_path && override_path[0] ? override_path : "libvocem_gl.so",
                          RTLD_NOW | RTLD_LOCAL);

    // Inside a container, by its path on the host.
    //
    // A Steam game runs in the Steam Linux Runtime, which is a container with a
    // filesystem of its own: `/usr` is the runtime's, and the soname above is not on
    // any search path in there. pressure-vessel does bring *this* shim in -- it
    // copies every LD_PRELOAD module into the container and expands `$LIB` while it
    // does, measured: `libvocem_gl_shim.so` turns up in both `lib` and `lib32` of
    // `/tmp/pressure-vessel-libs-*`, beside MangoHud's -- but the shim is all it
    // knows about. Nothing tells it that this small library opens a larger one.
    //
    // What it does do is mount the host at `/run/host`, so the larger one is
    // reachable by the path it was installed to with that prefix. Measured from
    // inside a running game: `/run/host/usr/lib/libvocem_gl.so` and
    // `/run/host/usr/lib32/libvocem_gl.so` are both there.
    //
    // The directory is the one this build installs to rather than `$LIB`, because
    // the two are not the same question -- `$LIB` is what the linker calls the
    // architecture, `VOCEM_LIBDIR` is where the package actually put the file -- and
    // each of the two builds knows its own answer at compile time.
    //
    // This dlopen pair lives on the *present* path, after a frame has actually been
    // drawn by the application, never in a resolution path. The distinction is
    // load-bearing: dlopen walks the filesystem, and a resolution path runs inside
    // sandboxed processes where a file syscall is SIGSYS. See the comment block
    // above the dispatch table.
    if (!handle) {
        handle = dlopen("/run/host" VOCEM_LIBDIR "/libvocem_gl.so", RTLD_NOW | RTLD_LOCAL);
    }
    // And by its own path, unprefixed. Inside a Flatpak the overlay is mounted
    // from the VulkanLayer extension at a directory the loader does not search:
    // the extension point has no add-ld-path, so the soname above resolves to
    // nothing and there is no /run/host either. The build that goes into the
    // extension compiles VOCEM_LIBDIR to where it will be mounted, which is the
    // one place left to look.
    if (!handle) {
        handle = dlopen(VOCEM_LIBDIR "/libvocem_gl.so", RTLD_NOW | RTLD_LOCAL);
    }
    if (!handle) {
        return;  // not installed, or the wrong architecture: stay out of the way
    }
    // Stored with the atomic builtins, like every other slot in this file: a
    // second thread presenting at the same moment reads these after seeing
    // g_load_attempted set, and a plain store is a data race by the letter
    // (harmless on x86, where it costs a skipped frame at worst; still not
    // the shape this file promises).
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
// The three ways a program reaches a present function, and why all three are
// needed.
//
// This was written as "interpose the symbol, and hook dlsym as a last line of
// defence". That has it backwards, and the measurements say so:
//
//   1. **The program's own call**, resolved by the dynamic linker. We are ahead of
//      libGL in the global scope, so our definition wins. This is the *only*
//      interposable point in the whole chain: libglvnd links libGL, libGLX and
//      libEGL with `-Bsymbolic`, and libGL reaches libGLX's `glXSwapBuffers`
//      through a relative relocation with no symbol name at all. Nothing
//      downstream of the application can be hooked, ever. Godot 3, RetroArch and
//      Dolphin are caught here -- and not many others.
//
//   2. **`glXGetProcAddress` / `eglGetProcAddress`.** Measured: these return
//      libglvnd's *internal* pointer, byte-identical with and without the preload,
//      because the dispatch table took its address at link time under -Bsymbolic.
//      An application that asks for `glXSwapBuffers` this way gets the real one
//      and we never see a frame. So these are not a nicety: without them every GL
//      loader -- glad, GLEW, epoxy -- bypasses us.
//
//   3. **`dlsym`.** The one that actually carries the weight. SDL, GLFW and glad
//      all `dlopen` the GL library and `dlsym` on *that handle*, which searches the
//      object's own scope and never the global one, so interposition is invisible
//      to them. That is Unity, Godot 4, PCSX2, and Minecraft through LWJGL/GLFW --
//      and Balatro, which is LÖVE on SDL. `readelf` on the installed libSDL2,
//      libSDL3 and libglfw shows zero undefined `glX*`/`egl*` symbols and only
//      `dlopen`/`dlsym`: for those libraries there is nothing else to hook.
//
// One table, consulted by 2 and 3 alike, so the two cannot drift apart -- which
// they had, quietly: the proc-address hooks answered for two names and dlsym for
// five.

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
// Which pointers the shim is allowed to remember, and why that is the question.
//
// A process does not contain one implementation of these names. Every Electron
// application contains **two**: ANGLE's bundled libEGL.so, which Chromium opens
// privately, and the system's libEGL.so.1, which ANGLE's own native backend opens
// privately underneath it. Both have an `eglGetProcAddress`; both pass through
// our `dlsym` hook.
//
// The 0.1.0-41/-42 shim kept one global slot per name, first pointer wins. The
// first one seen is ANGLE's, so when ANGLE asked what it believed was the
// *driver's* dispatcher for a native entry point, the shim forwarded the question
// back to ANGLE. ANGLE re-entered its own initialisation, took its own display
// lock a second time, and the GPU process froze before it could rewrite its own
// argv. Measured on Discord under -42: five threads, the main one in futex_wait,
// libEGL.so.1 mapped but the NVIDIA vendor library never loaded, the splash
// screen up forever -- where -40 had broken Electron loudly, -42 broke it
// silently. Reproduced on the first relaunch, and by `tests/shim_two_egls.cpp`.
//
// The same first-wins slot had a second corpse in it: `dlsym(RTLD_DEFAULT,
// "eglGetProcAddress")` resolves the shim's *own* export -- the shim is first in
// the global scope -- and a shim that remembers that pointer as "the real one"
// has its hook forwarding to itself. The forward is a tail call, so it does not
// even crash: it loops at constant stack, forever. Also in the test.
//
// So the rule is not "remember the first pointer". It is: **remember a pointer
// only when it belongs to the system's GL stack** -- glvnd's libraries or a
// vendor ICD -- because that is the level the overlay draws at, and it is the
// only level of which there is one per process. A private implementation such as
// ANGLE keeps its own names: whoever resolves them gets the real thing,
// untouched, and the shim stays out of that layer entirely. The overlay still
// sees every frame, because ANGLE's backend presents through the system library,
// and *that* resolution is remembered and hooked.
//
// `dladdr` is what answers "whose pointer is this": it walks the loaded objects
// in memory and makes no file syscall, which matters because this runs inside
// sandboxed processes. The name compared is the basename of the object's path,
// against the glvnd family -- with the trailing dot doing real work: the system
// library is `libEGL.so.1...`, ANGLE's bundled copy is exactly `libEGL.so`, and
// the dot is what tells them apart.
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

// One entry per interposed name. Two remembered pointers, with different trust:
//
//   `seen` is what the `dlsym` hook saw the application resolve, already vetted
//   by is_system_gl(). This is the only way to reach a library that was opened
//   with RTLD_LOCAL -- Chromium, ANGLE, SDL -- and it is filled at the exact
//   moment the application resolves the name, with no lookup of our own.
//
//   `next` is RTLD_NEXT, tried once, for the application that linked its GL
//   library normally, where our exported symbol won the relocation and no dlsym
//   was ever involved. Deliberately *not* vetted: at level 1 the substitution
//   already happened at link time, and whatever is next in the global scope --
//   even a privately-shipped GL an application linked against -- is the only
//   correct place to forward to.
//
// Neither is ever cached as null: `seen` stores only successes, and `next`
// remembers the *finished* attempt in a separate flag. A null answer must not become
// permanent -- the first frame of a process can precede the library being mapped
// -- but the lookup must not repeat forever either: `dlsym(RTLD_NEXT, ...)`
// takes the dynamic loader's lock, and Chromium forks its children from a
// zygote whose other threads may have been holding it. Look once, remember the
// attempt, and let the `dlsym` hook fill `seen` in later if the application
// resolves the name -- an atomic store, with no loader involvement at all.
//
// **And no dlopen of any kind in here.** The -41 shim fell back to
// `dlopen(soname, RTLD_NOLOAD)` on the theory that NOLOAD only looks at what is
// already mapped. Measured with a trapping seccomp filter: for a soname that is
// *not* mapped -- precisely the case the fallback existed for -- glibc walks the
// search path first, three openat and four newfstatat, before honouring NOLOAD.
// Chromium's children run under seccomp filters where that is SIGSYS, and the
// process is killed rather than told no. Discord went from starting without
// hardware acceleration to not starting at all. `tests/shim_seccomp.cpp` holds
// the door shut.
struct Hook {
    const char* name;
    void* hook;
    void* seen;
    void* next;
    int next_attempted;
};

Hook g_hooks[] = {
    {"glXSwapBuffers", reinterpret_cast<void*>(&glXSwapBuffers), nullptr, nullptr, 0},
    // A second, separately exported GLX present entry point. It is a real function
    // in the installed libGL, and MangoHud hooks it; we did not.
    {"glXSwapBuffersMscOML", reinterpret_cast<void*>(&glXSwapBuffersMscOML), nullptr, nullptr, 0},
    {"eglSwapBuffers", reinterpret_cast<void*>(&eglSwapBuffers), nullptr, nullptr, 0},
    // Damage-aware present. Measured: `libEGL.so.1` does **not** export these --
    // `nm -D` finds nothing -- so they exist only through `eglGetProcAddress`, and
    // an application that uses them is invisible to symbol interposition by
    // construction. MangoHud does not cover them either (checked: zero occurrences
    // in its EGL injection), so this is our own judgement rather than borrowed:
    // the cost is two forwarding functions and the failure it prevents is an
    // overlay that never appears and never explains why.
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
    // The end of a context, which is the end of everything the overlay built in
    // it. Hooked for the same reason MangoHud hooks them: what we hold -- a shader
    // program, a vertex buffer, a texture per person in the channel -- belongs to
    // the context and means nothing once it is gone.
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

// The real function behind a hook: what the dlsym hook vetted and remembered,
// or RTLD_NEXT, once. Null when neither has an answer *yet* -- the dlsym hook
// keeps filling `seen` as the application resolves names.
//
// The attempt is remembered only AFTER the lookup has its answer, and the
// answer is stored before the attempt. The other order -- "attempted" first,
// then the lookup -- told every thread that arrived in between that the real
// function did not exist: a null eglGetProcAddress("glClear") to a game, a
// skipped real swap in a present hook. Measured with sixteen threads asking at
// once: some thread was told null in 181 of 300 fresh processes
// (tests/shim_lookup_race.cpp). Threads that arrive together now each look
// the name up, which is harmless -- the lookup is idempotent, and what the
// rule above forbids is a lookup repeated for the life of the process, which
// the flag still prevents once one of them has finished.
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

// The two damage-aware presents exist only through eglGetProcAddress (libEGL
// does not export them -- the table in DESIGN), so they have slots of their
// own: the damage hooks used to ask the dispatcher by name on every frame,
// which put a per-name lookup, and whatever lock glvnd holds around it, on the
// present path -- the one place this file promises "resolve once per slot". A
// pointer is remembered only when it belongs to the system GL stack, and a
// null is never remembered (entry 36, both halves).
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

// The shared body of the three dispatch hooks: forward to the real dispatcher,
// answering with our own hook for the names we interpose. Our hook is offered
// only when the real function has been found -- handing back a dispatch function
// that knows five names and answers null to the rest is not a partial hook, it
// is a broken EGL, and it is what put every Electron application on this machine
// into software compositing under -40.
//
// For an interposed name, the real dispatcher is asked *first*, for two reasons
// that are both load-bearing. A dispatcher is also how a program asks whether
// something exists -- an EGL dispatcher that has no `glXSwapBuffers` must keep
// saying so, not hand out ours. And the answer is the only chance to learn where
// this name's real function lives: ANGLE resolves the native present functions
// through `eglGetProcAddress` alone, never through `dlsym`, so without keeping
// this pointer our present hook would have nothing to forward to in exactly the
// process the dispatch substitution exists for.
void* dispatch(Hook& own, const char* name) {
    void* real = real_for(own);
    if (!real) {
        return nullptr;
    }
    void* answer = reinterpret_cast<PFN_egl_get_proc>(real)(name);
    if (answer && !disabled()) {
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

// Not a last line of defence -- the main one. SDL, GLFW and glad reach their
// present function through `dlopen` plus `dlsym` on that handle, which searches the
// object's own scope and never the global one, so nothing we interpose is visible
// to them. That covers Unity, Godot 4, PCSX2 and Minecraft, and LÖVE, which is what
// Balatro is. VOCEM_NO_DLSYM=1 turns it off if it ever upsets an application.
//
// **The real lookup happens first, and the hook is substituted only when it
// succeeded** -- `dlsym` is also how a program asks *whether* something exists, and
// answering `glXSwapBuffers` out of an EGL-only handle tells a caller that GLX is
// available when it is not. **And the substitution happens only when the real
// pointer belongs to the system's GL stack** -- see is_system_gl() above for the
// two ways one global slot per name froze every Electron application. A private
// GL keeps its private names; the system's are remembered and replaced. With
// VOCEM_DISABLE=1 nothing is remembered and nothing is replaced, because the
// promised way out of a broken interposition has to switch off the interposition
// and not just the drawing.
//
// Two things about interposing dlsym at all, neither of which has a good answer:
// it changes the search scope, so `RTLD_NEXT` and `RTLD_DEFAULT` from the
// application are now resolved relative to *this* library rather than to the
// caller. MangoHud has an opt-in workaround using the return address and
// `dlopen(RTLD_NOLOAD)` which their own code says crashes on NVIDIA, and which was
// force-enabled in 0.8.2 and turned back off in 0.8.3. It is not copied here.
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
                // This lookup is the only place the real function is ever visible
                // when the application opened its GL library with RTLD_LOCAL, and a
                // moment later we hand back ours instead. First one wins: there is
                // one system GL stack per process, so a second sighting is the same
                // pointer or a symlinked spelling of it.
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

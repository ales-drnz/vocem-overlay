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
#include <link.h>
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
PFN_dlsym find_real_dlsym() {
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
    return real;
}

void* real_dlsym(void* handle, const char* name) {
    PFN_dlsym real = find_real_dlsym();
    return real ? real(handle, name) : nullptr;
}

// What this thread is doing inside the shim, for the two re-entries a chain
// makes possible. Initial-exec: the shim is always preloaded, so its TLS is
// static and reading it neither allocates nor calls into the loader.
//   kForwarding  a call is on its way down to another interposer, which may
//                come back to our exports looking for the system's function
//   kAskingNext  the next dlsym in the chain is answering us, and may call
//                the global dlsym -- which is ours
//   kDispatching a dispatcher down the chain is answering us
enum : int { kForwarding = 1, kAskingNext = 2, kDispatching = 4 };
__thread int t_inside __attribute__((tls_model("initial-exec"))) = 0;

bool inside(int what) {
    return (t_inside & what) != 0;
}

// Sets a bit for the life of one call down the chain.
struct Inside {
    int saved;
    explicit Inside(int what) : saved(t_inside) { t_inside |= what; }
    ~Inside() { t_inside = saved; }
};

PFN_dlsym g_next_dlsym = nullptr;

bool another_copy(void* address);

// The dlsym the application would have reached had we not been preloaded: the
// next definition in the global scope after this library, asked unversioned.
// It is libc's own, the same function as find_real_dlsym(), unless another
// interposer that exports dlsym is preloaded after us -- MangoHud's shim does,
// unversioned, which is exactly why the versioned lookup above never finds it
// (tests/shim_chain.cpp). The application's own questions are put to this one,
// so an interposer behind us answers them as it would alone; our internal
// lookups keep using libc's, which no interposer can redirect.
void* next_dlsym(void* handle, const char* name) {
    PFN_dlsym next = __atomic_load_n(&g_next_dlsym, __ATOMIC_ACQUIRE);
    if (!next) {
        next = reinterpret_cast<PFN_dlsym>(real_dlsym(RTLD_NEXT, "dlsym"));
        // Another copy of this shim behind us (a build tree or a second
        // prefix beside the installed one) would hand the game its own hooks
        // and the overlay every frame twice: libc's answers instead, as
        // before the chain was followed (measured, refutation of this change).
        if (next && another_copy(reinterpret_cast<void*>(next))) {
            next = find_real_dlsym();
        }
        if (next) {
            __atomic_store_n(&g_next_dlsym, next, __ATOMIC_RELEASE);
        }
    }
    // Asked from inside its own answer -- an interposer whose dlsym calls the
    // global one -- libc's answers, or the two would take turns forever.
    if (!next || inside(kAskingNext)) {
        return real_dlsym(handle, name);
    }
    Inside asking(kAskingNext);
    return next(handle, name);
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
// allocation. Only under VOCEM_DEBUG, and once per process and cause, for the
// failures this file can have that look exactly like success.
void say(const char* a, const char* b = "", const char* c = "", const char* d = "",
         const char* e = "", const char* f = "", const char* g = "") {
    const char* parts[] = {"[vocem/gl-shim] ", a, b, c, d, e, f, g, "\n"};
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

// The same functions under hidden names, for the table. Taking the address of
// an exported name in a shared library binds to the first definition in the
// global scope, which is another interposer's whenever one is preloaded ahead
// of us: the table then held THAT interposer's present as "our hook", and a
// frame went round it twice and was dropped (measured in the refutation of
// this change). A hidden alias binds here. Not exported: still 12.
#define VOCEM_OWN(result, name, params) \
    result own_##name params __attribute__((alias(#name), visibility("hidden")));
VOCEM_OWN(void, glXSwapBuffers, (void*, unsigned long))
VOCEM_OWN(long long, glXSwapBuffersMscOML, (void*, unsigned long, long long, long long, long long))
VOCEM_OWN(unsigned int, eglSwapBuffers, (void*, void*))
VOCEM_OWN(unsigned int, eglSwapBuffersWithDamageEXT, (void*, void*, int*, int))
VOCEM_OWN(unsigned int, eglSwapBuffersWithDamageKHR, (void*, void*, int*, int))
VOCEM_OWN(void*, glXGetProcAddress, (const unsigned char*))
VOCEM_OWN(void*, glXGetProcAddressARB, (const unsigned char*))
VOCEM_OWN(void*, eglGetProcAddress, (const char*))
VOCEM_OWN(void, glXDestroyContext, (void*, void*))
VOCEM_OWN(unsigned int, eglDestroyContext, (void*, void*))
VOCEM_OWN(unsigned int, eglTerminate, (void*))
#undef VOCEM_OWN

}  // extern "C"

namespace {

const char* base_name(const char* path) {
    const char* slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// ---------------------------------------------------------------------------
// Which pointers the shim may remember.
//
// A process can hold more than one implementation of these names: every
// Electron app has ANGLE's bundled libEGL.so and the system's libEGL.so.1
// beneath it. Forwarding ANGLE's question back to ANGLE deadlocks its GPU
// process, and dlsym(RTLD_DEFAULT, ...) returns the shim's own export, a
// forward-to-self loop (tests/shim_two_egls.cpp). So a pointer is remembered
// only when it belongs to the system GL stack -- glvnd or a vendor ICD, of
// which there is one per process -- or to another interposer preloaded after
// us (is_chain(), below). A private GL keeps its names untouched; the
// overlay still sees every frame because ANGLE presents through the system
// library.
//
// dladdr answers "whose pointer is this" from memory, with no file syscall
// (this runs in sandboxed processes). Two names are judged, and the trailing
// dot is load-bearing in both: the system library is libEGL.so.1..., ANGLE's
// is exactly libEGL.so.
//
//   * The file's name, as it was opened: entry 36's rule, unchanged.
//   * The object's SONAME, read from its own dynamic section in memory -- but
//     only for an object in libc's own directory. A game that opened the
//     system's libGL through its unversioned development link `libGL.so` was
//     taken for a private GL by its file name alone, and the overlay never saw
//     a frame and said nothing (tests/gl_draw_local.cpp, scenario `soname`).
//     Believed anywhere, the SONAME is the wrong witness: a private Mesa ships
//     `libEGL.so` whose SONAME is libEGL.so.1 (the Android emulator's
//     llvmpipe build, measured by the refutation of this change), and taking
//     it for the system's hands its questions to the system's library --
//     entry 36's cross-wiring (tests/shim_two_egls.cpp, the versioned stub;
//     entry 304).

// The object's SONAME when it has one, else its file's basename; `file` gets
// the path it was opened by.
const char* object_name(void* pointer, const char** file) {
    Dl_info info;
    struct link_map* map = nullptr;
    if (!pointer ||
        dladdr1(pointer, &info, reinterpret_cast<void**>(&map), RTLD_DL_LINKMAP) == 0 ||
        !info.dli_fname) {
        return nullptr;
    }
    if (file) {
        *file = info.dli_fname;
    }
    if (map && map->l_ld) {
        ElfW(Addr) strtab = 0;
        ElfW(Addr) soname = 0;
        bool has_soname = false;
        for (const ElfW(Dyn)* entry = map->l_ld; entry->d_tag != DT_NULL; ++entry) {
            if (entry->d_tag == DT_STRTAB) {
                strtab = entry->d_un.d_ptr;
            } else if (entry->d_tag == DT_SONAME) {
                soname = entry->d_un.d_val;
                has_soname = true;
            }
        }
        if (has_soname && strtab) {
            // ld.so relocates the dynamic section in place where it is
            // writable (x86); where it is read-only (the vDSO) the address is
            // still relative to the object.
            if (strtab < map->l_addr) {
                strtab += map->l_addr;
            }
            return reinterpret_cast<const char*>(strtab + soname);
        }
    }
    return base_name(info.dli_fname);
}

bool name_is_system_gl(const char* name) {
    static const char* const prefixes[] = {
        // libglvnd's dispatchers, versioned -- the dot excludes ANGLE's "libEGL.so".
        "libEGL.so.", "libGLX.so.", "libGL.so.", "libOpenGL.so.",
        "libGLdispatch.so.", "libGLESv2.so.", "libGLESv1_CM.so.",
        // Vendor ICDs: libEGL_nvidia.so.0, libGLX_mesa.so.0, and their kin.
        "libEGL_", "libGLX_",
    };
    for (unsigned i = 0; i < sizeof(prefixes) / sizeof(prefixes[0]); ++i) {
        if (strncmp(name, prefixes[i], strlen(prefixes[i])) == 0) {
            return true;
        }
    }
    return false;
}

// Whether a path lies directly in the directory libc was loaded from -- the
// system's library directory as this process sees it, the Steam container's
// overrides included. dladdr of a libc function: memory only.
bool in_libc_directory(const char* path) {
    Dl_info libc;
    if (dladdr(reinterpret_cast<void*>(&dladdr), &libc) == 0 || !libc.dli_fname) {
        return false;
    }
    const char* libc_slash = strrchr(libc.dli_fname, '/');
    const char* path_slash = strrchr(path, '/');
    if (!libc_slash || !path_slash) {
        return false;
    }
    const size_t length = static_cast<size_t>(libc_slash - libc.dli_fname);
    return static_cast<size_t>(path_slash - path) == length &&
           strncmp(path, libc.dli_fname, length) == 0;
}

bool is_system_gl(void* pointer) {
    const char* file = nullptr;
    const char* soname = object_name(pointer, &file);
    if (!soname || !file) {
        return false;
    }
    return name_is_system_gl(base_name(file)) ||
           (name_is_system_gl(soname) && in_libc_directory(file));
}

// LD_PRELOAD as it was the first time the chain was asked about, kept: a
// process that rewrites or clears the variable afterwards (for its children)
// does not change what was loaded into it. glibc never frees an environment
// string, so the pointer stays good. "" when there was none.
const char* g_preload = nullptr;

const char* preload_list() {
    const char* list = __atomic_load_n(&g_preload, __ATOMIC_ACQUIRE);
    if (!list) {
        list = getenv("LD_PRELOAD");
        if (!list) {
            list = "";
        }
        __atomic_store_n(&g_preload, list, __ATOMIC_RELEASE);
    }
    return list;
}

// This library's load address and file name, asked of dladdr once.
void* g_self_base = nullptr;
const char* g_self_name = nullptr;

// Whether an address lies in an object LD_PRELOAD names AFTER this library's
// own entry. After, because only what is behind us is a link we can forward
// to: an object ahead of us that we reached again is the chain looping back
// (an interposer ahead, whose hook MangoHud's table binds to, measured in the
// refutation of this change). Not this library, not another copy of it (a
// build tree beside the installed one), never an empty entry or an empty name
// -- the session's own value starts with a colon, and dladdr names the
// executable by argv[0], which may be "" (both measured: the game itself was
// taken for a link). Compared by basename: an entry may be a bare soname or
// carry $LIB, and the loaded path is what dladdr reports. getenv and dladdr
// only: no allocation, no file syscall. False when our own entry is not found.
bool preloaded_after_us(void* address) {
    const char* list = preload_list();
    Dl_info info;
    if (!address || !list[0] || dladdr(address, &info) == 0 || !info.dli_fname) {
        return false;
    }
    const char* self_name = __atomic_load_n(&g_self_name, __ATOMIC_ACQUIRE);
    if (!self_name) {
        Dl_info self;
        if (dladdr(reinterpret_cast<void*>(&preloaded_after_us), &self) == 0 ||
            !self.dli_fname) {
            return false;
        }
        __atomic_store_n(&g_self_base, self.dli_fbase, __ATOMIC_RELAXED);
        self_name = base_name(self.dli_fname);
        __atomic_store_n(&g_self_name, self_name, __ATOMIC_RELEASE);
    }
    const char* base = base_name(info.dli_fname);
    const size_t length = strlen(base);
    const size_t self_length = strlen(self_name);
    if (length == 0 || info.dli_fbase == __atomic_load_n(&g_self_base, __ATOMIC_RELAXED) ||
        strcmp(base, self_name) == 0) {
        return false;
    }
    bool after_us = false;
    // ld.so separates entries with colons or spaces.
    for (const char* entry = list; *entry;) {
        const char* end = entry + strcspn(entry, ": ");
        const char* entry_base = end;
        while (entry_base > entry && entry_base[-1] != '/') {
            --entry_base;
        }
        const size_t entry_length = static_cast<size_t>(end - entry_base);
        if (entry_length == self_length && strncmp(entry_base, self_name, self_length) == 0) {
            after_us = true;
        } else if (entry_length == length && strncmp(entry_base, base, length) == 0) {
            return after_us;
        }
        entry = *end ? end + 1 : end;
    }
    return false;
}

// Whether an address lies in another copy of this library.
bool another_copy(void* address) {
    Dl_info info;
    const char* self_name = __atomic_load_n(&g_self_name, __ATOMIC_ACQUIRE);
    if (!self_name) {
        preloaded_after_us(address);  // fills the cache
        self_name = __atomic_load_n(&g_self_name, __ATOMIC_ACQUIRE);
    }
    return self_name && address && dladdr(address, &info) != 0 && info.dli_fname &&
           info.dli_fbase != __atomic_load_n(&g_self_base, __ATOMIC_RELAXED) &&
           strcmp(base_name(info.dli_fname), self_name) == 0;
}

// Whether a pointer belongs to another interposer in the chain: not the
// system's GL stack, and in an object LD_PRELOAD names after us
// (preloaded_after_us()). The test is structural rather than a list of
// names, as entry 115 asked: an interposer that hooks presents is preloaded
// (MangoHud is measured; obs-glcapture and Steam's overlay arrive the same
// way), while a GL a program carries privately (ANGLE, a bundled Mesa) is
// linked or dlopened and never is, so entry 36's rule stands for it
// (tests/shim_private_dispatch.cpp, tests/shim_two_egls.cpp).
//
// Where this cannot see an interposer the shim does what it did before it
// looked, and cuts it out on the dlsym door: one that arrives by
// /etc/ld.so.preload (not in the variable), or one preloaded into a process
// that emptied the variable before its first GL lookup. Reading the loader's
// own list instead (dl_iterate_phdr up to the executable's first DT_NEEDED)
// would see both, and was not done: parsing dynamic sections in every process
// of the session, where a wrong end of the list would call a linked private
// GL a link and cross-wire it -- a failure worse than the one it removes.
bool is_chain(void* pointer) {
    return pointer && !is_system_gl(pointer) && preloaded_after_us(pointer);
}

// One entry per interposed name, with three remembered pointers:
//
//   `seen`: what the dlsym hook saw the application resolve, vetted by
//   is_system_gl(). The only way to reach a GL library opened RTLD_LOCAL.
//
//   `chain`: the same, when what the application resolved belongs to another
//   interposer preloaded after us (is_chain()) -- its hook, handed out by its
//   own dlsym or dispatcher. Preferred over `seen`: the frame goes down the
//   chain and the other interposer calls the system's function itself.
//
//   `next`: RTLD_NEXT, for an application that linked its GL normally. Not
//   vetted: at level 1 whatever is next in the global scope is the only
//   correct place to forward to. `next_chain` says whether it is another
//   interposer, stored before `next` so a reader that sees one sees both.
//
// None is cached as null (the first frame can precede the library being
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
    void* chain = nullptr;
    int next_chain = 0;
    int said_not_followed = 0;
};

Hook g_hooks[] = {
    {"glXSwapBuffers", reinterpret_cast<void*>(&own_glXSwapBuffers), nullptr, nullptr, 0},
    // A second, separately exported GLX present entry point.
    {"glXSwapBuffersMscOML", reinterpret_cast<void*>(&own_glXSwapBuffersMscOML), nullptr, nullptr, 0},
    {"eglSwapBuffers", reinterpret_cast<void*>(&own_eglSwapBuffers), nullptr, nullptr, 0},
    // Damage-aware presents: libEGL.so.1 does not export these, so they are
    // reachable only through eglGetProcAddress (doors 2 and 3).
    {"eglSwapBuffersWithDamageEXT", reinterpret_cast<void*>(&own_eglSwapBuffersWithDamageEXT), nullptr,
     nullptr, 0},
    {"eglSwapBuffersWithDamageKHR", reinterpret_cast<void*>(&own_eglSwapBuffersWithDamageKHR), nullptr,
     nullptr, 0},
    // The dispatch functions have to hand back *themselves*, or a program that
    // fetches `glXGetProcAddress` through dlsym gets the real one and step 2 above
    // is bypassed along with it.
    {"glXGetProcAddress", reinterpret_cast<void*>(&own_glXGetProcAddress), nullptr, nullptr, 0},
    {"glXGetProcAddressARB", reinterpret_cast<void*>(&own_glXGetProcAddressARB), nullptr, nullptr, 0},
    {"eglGetProcAddress", reinterpret_cast<void*>(&own_eglGetProcAddress), nullptr, nullptr, 0},
    // Context teardown: what the overlay built in a context (program, buffer,
    // textures) must be released before the context goes.
    {"glXDestroyContext", reinterpret_cast<void*>(&own_glXDestroyContext), nullptr, nullptr, 0},
    {"eglDestroyContext", reinterpret_cast<void*>(&own_eglDestroyContext), nullptr, nullptr, 0},
    {"eglTerminate", reinterpret_cast<void*>(&own_eglTerminate), nullptr, nullptr, 0},
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

// Where a call goes: the function, and whether it belongs to another
// interposer (is_chain()). A call down the chain is made with kForwarding set.
struct Target {
    void* function;
    bool chain;
};

// Under VOCEM_DEBUG, once per name: the application found a present or a
// dispatcher somewhere that is neither the system's GL nor a link of the
// chain, so the overlay will not see the frames that go through it. Right for
// ANGLE; for a system library under a name this file does not know, it is the
// only line anything says (the shape of entry 38). Our own exports, which
// RTLD_DEFAULT hands back, are not news, nor another copy's.
void not_followed(Hook& entry, void* pointer) {
    static int debug = -1;  // -1 unknown, 0 off, 1 on
    int enabled = __atomic_load_n(&debug, __ATOMIC_ACQUIRE);
    if (enabled < 0) {
        const char* env = getenv("VOCEM_DEBUG");
        enabled = (env && env[0] == '1') ? 1 : 0;
        __atomic_store_n(&debug, enabled, __ATOMIC_RELEASE);
    }
    if (enabled == 0 || !pointer || __atomic_load_n(&entry.said_not_followed, __ATOMIC_ACQUIRE)) {
        return;
    }
    const char* file = nullptr;
    const char* name = object_name(pointer, &file);
    Dl_info self;
    if (!name || !file || another_copy(pointer) ||
        (dladdr(reinterpret_cast<void*>(&not_followed), &self) != 0 && self.dli_fname &&
         strcmp(file, self.dli_fname) == 0)) {
        return;
    }
    if (__atomic_exchange_n(&entry.said_not_followed, 1, __ATOMIC_ACQ_REL)) {
        return;
    }
    say("not following ", entry.name, " into ", file, " (", name,
        "): not the system's GL library, so no overlay on frames presented there");
}

// Remembers what a road showed us for a hooked name: the system's function in
// `seen`, another interposer's in `chain`, first one wins in each. Anything
// else -- a private GL, our own export -- is not remembered (entry 36). True
// when remembered. Only for the application's own questions: one asked from
// further down the chain, or by another interposer, is that interposer
// looking for what is underneath it, and its answer in `seen` would take the
// frame past it from then on.
bool remember(Hook& entry, void* pointer) {
    void** slot = is_system_gl(pointer)    ? &entry.seen
                  : preloaded_after_us(pointer) ? &entry.chain
                                             : nullptr;
    if (!slot) {
        not_followed(entry, pointer);
        return false;
    }
    void* expected = nullptr;
    __atomic_compare_exchange_n(slot, &expected, pointer, false, __ATOMIC_RELEASE,
                                __ATOMIC_RELAXED);
    return true;
}

// RTLD_NEXT for a hooked name, looked up once. The answer is stored before the
// attempt is marked: the other order would tell threads arriving in between
// that the function does not exist (tests/shim_lookup_race.cpp). Threads
// arriving together may each look the name up, which is harmless; the flag
// still stops it repeating. Whether the answer is another interposer is
// decided once, with it, and stored first.
void* next_for(Hook& entry) {
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
        if (is_chain(next)) {
            __atomic_store_n(&entry.next_chain, 1, __ATOMIC_RELEASE);
        }
        __atomic_store_n(&entry.next, next, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&entry.next_attempted, 1, __ATOMIC_RELEASE);
    return next;
}

// The real function behind a hook: `chain`, else `seen`, else RTLD_NEXT.
// Null when none has an answer yet.
//
// While a call of ours is on its way down the chain (kForwarding), only the
// system's function will do. An interposer that comes back to our export is
// looking for what is underneath -- through the global eglGetProcAddress, or
// a dlsym that handed it our hook -- and the chain would send it back to
// itself. Such a re-entry gets `seen` or a system `next`, or nothing.
Target real_for(Hook& entry) {
    const bool forwarding = inside(kForwarding);
    if (!forwarding) {
        if (void* chain = __atomic_load_n(&entry.chain, __ATOMIC_ACQUIRE)) {
            return {chain, true};
        }
    }
    if (void* seen = __atomic_load_n(&entry.seen, __ATOMIC_ACQUIRE)) {
        return {seen, false};
    }
    void* next = next_for(entry);
    if (next && __atomic_load_n(&entry.next_chain, __ATOMIC_ACQUIRE)) {
        return forwarding ? Target{nullptr, false} : Target{next, true};
    }
    return {next, false};
}

Target real_for_name(const char* name) {
    Hook* entry = find_hook(name);
    return entry ? real_for(*entry) : Target{nullptr, false};
}

using PFN_egl_get_proc = void* (*)(const char*);

// The real eglGetProcAddress, for the entry points libEGL does not export and
// which therefore cannot be reached by symbol lookup at all.
void* real_egl_proc(const char* name) {
    const Target real = real_for_name("eglGetProcAddress");
    if (!real.function) {
        return nullptr;
    }
    Inside forwarding(real.chain ? kForwarding : 0);
    return reinterpret_cast<PFN_egl_get_proc>(real.function)(name);
}

// The damage-aware presents, which libEGL does not export. The application
// found our hook through a dispatcher, and dispatch() remembered what the real
// one answered in the same slots as every other name; RTLD_NEXT finds an
// interposer after us that exports them. Only when neither has an answer (our
// export reached by name) is the dispatcher asked here -- once, the answer
// remembered, never a null (entry 36). And never our own hook: a dispatcher
// behind us can hand it back (MangoHud's table binds to the first definition
// in the global scope, which is ours), and calling it would be calling
// ourselves until the stack ran out.
Target real_damage_swap(Hook& entry) {
    const Target known = real_for(entry);
    if (known.function) {
        return known;
    }
    void* found = real_egl_proc(entry.name);
    if (!found || found == entry.hook) {
        return {nullptr, false};
    }
    const bool chain = is_chain(found);
    if (inside(kForwarding)) {
        // Asked from further down the chain: only the system's will do.
        return chain ? Target{nullptr, false} : Target{found, false};
    }
    remember(entry, found);
    return {found, chain};
}

// Hand the frame over, once, before the real present puts it on the screen --
// once: a present that comes back to us from further down the chain
// (kForwarding) was handed over on its way in.
void present_glx(void* display, unsigned long drawable) {
    if (disabled() || inside(kForwarding)) {
        return;
    }
    load_overlay();
    if (PFN_present_glx present = __atomic_load_n(&g_present_glx, __ATOMIC_ACQUIRE)) {
        present(display, drawable);
    }
}

void present_egl(void* display, void* surface) {
    if (disabled() || inside(kForwarding)) {
        return;
    }
    load_overlay();
    if (PFN_present_egl present = __atomic_load_n(&g_present_egl, __ATOMIC_ACQUIRE)) {
        present(display, surface);
    }
}

// Whether a question about a hooked name comes from the interposer that is
// our link for that very name: the return address of our export lies in the
// object that `chain` or a chain `next` points into. Such a caller is asking
// for what is underneath it -- the function it forwards to -- and gets the
// real answer, never our hook: handed our hook, it forwards to us and we to
// it (steam-for-linux#4630 is that loop between two overlays). Any other
// preloaded library asking -- SDL preloaded as a fix, which looks the present
// up for the game -- is the game asking, and gets our hook (measured in the
// refutation of this change: "anything LD_PRELOAD names" cut the overlay out
// of such a game). Read, never dlopened: the return address is the half of
// MangoHud's workaround that is harmless.
bool same_object(void* a, void* b) {
    Dl_info first;
    Dl_info second;
    return a && b && dladdr(a, &first) != 0 && dladdr(b, &second) != 0 &&
           first.dli_fbase == second.dli_fbase;
}

bool asked_by_link(Hook& entry, void* return_address) {
    if (!preloaded_after_us(return_address)) {
        return false;
    }
    if (same_object(return_address, __atomic_load_n(&entry.chain, __ATOMIC_ACQUIRE))) {
        return true;
    }
    void* next = next_for(entry);
    return next && __atomic_load_n(&entry.next_chain, __ATOMIC_ACQUIRE) &&
           same_object(return_address, next);
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

// The real dispatcher may be another interposer's (its hooks are then what it
// answers, and remembered in `chain`). A question asked from further down the
// chain, or by another interposer, gets the answer as it is.
//
// Asked from further down the chain with no system dispatcher known -- the
// link's own present looking up some other name through the global
// dispatcher -- the link's dispatcher is asked once more, and a question that
// comes back from inside that is answered null (kDispatching): an answer for
// every name, and no loop (both measured in the refutation of this change,
// which found every name answered null).
void* dispatch(Hook& own, const char* name, void* caller) {
    Target real = real_for(own);
    if (!real.function && inside(kForwarding) && !inside(kDispatching)) {
        if (void* next = next_for(own)) {
            real = {next, true};
        }
    }
    if (!real.function) {
        return nullptr;
    }
    void* answer = nullptr;
    {
        Inside forwarding(real.chain ? kForwarding | kDispatching : 0);
        answer = reinterpret_cast<PFN_egl_get_proc>(real.function)(name);
    }
    if (answer && !disabled() && name && same_family(own, name) && !inside(kForwarding)) {
        Hook* entry = find_hook(name);
        if (entry && !asked_by_link(*entry, caller)) {
            // A dispatcher behind us may answer with our own hook (MangoHud's
            // table binds to the first definition in the global scope, which
            // is ours when we are first): that is not remembered, anything
            // else is where it may be (remember()), and the application gets
            // our hook either way.
            if (answer != entry->hook) {
                remember(*entry, answer);
            }
            return entry->hook;
        }
    }
    return answer;
}

// Calls a Target with the arguments given, kForwarding set when it is another
// interposer's. `fallback` when there is nothing to call.
template <typename Result, typename... Args>
Result call(const Target& target, Result fallback, Args... args) {
    if (!target.function) {
        return fallback;
    }
    Inside forwarding(target.chain ? kForwarding : 0);
    return reinterpret_cast<Result (*)(Args...)>(target.function)(args...);
}

template <typename... Args>
void call_void(const Target& target, Args... args) {
    if (target.function) {
        Inside forwarding(target.chain ? kForwarding : 0);
        reinterpret_cast<void (*)(Args...)>(target.function)(args...);
    }
}

}  // namespace

extern "C" {

void glXSwapBuffers(void* display, unsigned long drawable) {
    const Target real = real_for_name("glXSwapBuffers");
    present_glx(display, drawable);
    call_void(real, display, drawable);
}

long long glXSwapBuffersMscOML(void* display, unsigned long drawable, long long target_msc,
                               long long divisor, long long remainder) {
    const Target real = real_for_name("glXSwapBuffersMscOML");
    present_glx(display, drawable);
    return call(real, -1LL, display, drawable, target_msc, divisor, remainder);
}

unsigned int eglSwapBuffers(void* display, void* surface) {
    const Target real = real_for_name("eglSwapBuffers");
    present_egl(display, surface);
    return call(real, 0u, display, surface);
}

unsigned int eglSwapBuffersWithDamageEXT(void* display, void* surface, int* rects, int n_rects) {
    const Target real = real_damage_swap(*find_hook("eglSwapBuffersWithDamageEXT"));
    present_egl(display, surface);
    // The damage rectangles are the application's and are passed through
    // untouched. What we drew outside them may not reach the screen -- that is the
    // point of the extension -- which is a known limit rather than a fix.
    return call(real, 0u, display, surface, rects, n_rects);
}

unsigned int eglSwapBuffersWithDamageKHR(void* display, void* surface, int* rects, int n_rects) {
    const Target real = real_damage_swap(*find_hook("eglSwapBuffersWithDamageKHR"));
    present_egl(display, surface);
    return call(real, 0u, display, surface, rects, n_rects);
}

// The end of a context. Told first, destroyed after: while the real call has not
// run yet the context still exists, which is the only state in which what is inside
// it can be given back. Told once: a teardown that comes back to us from further
// down the chain was told on its way in.
void glXDestroyContext(void* display, void* context) {
    const Target real = real_for_name("glXDestroyContext");
    if (!inside(kForwarding)) {
        context_gone(false, display, context);
    }
    call_void(real, display, context);
}

unsigned int eglDestroyContext(void* display, void* context) {
    const Target real = real_for_name("eglDestroyContext");
    if (!inside(kForwarding)) {
        context_gone(true, display, context);
    }
    return call(real, 0u, display, context);
}

// The whole display goes, and every context on it with it. No particular context to
// make current, so the overlay is told with none and drops its state without
// touching GL.
unsigned int eglTerminate(void* display) {
    const Target real = real_for_name("eglTerminate");
    if (!inside(kForwarding)) {
        context_gone(true, display, nullptr);
    }
    return call(real, 0u, display);
}

// Applications that resolve entry points dynamically have to receive our hooks as
// well, or the preload is bypassed for the one function that matters. Answered from
// the shared table, so adding a present function in one place adds it everywhere.
void* glXGetProcAddress(const unsigned char* name) {
    return dispatch(*find_hook("glXGetProcAddress"), reinterpret_cast<const char*>(name),
                    __builtin_return_address(0));
}

void* glXGetProcAddressARB(const unsigned char* name) {
    return dispatch(*find_hook("glXGetProcAddressARB"), reinterpret_cast<const char*>(name),
                    __builtin_return_address(0));
}

void* eglGetProcAddress(const char* name) {
    return dispatch(*find_hook("eglGetProcAddress"), name, __builtin_return_address(0));
}

// The main door, not a last line of defence: SDL, GLFW and glad reach the
// present through dlopen + dlsym on that handle. VOCEM_NO_DLSYM=1 turns it off.
//
// The lookup runs first and the hook is substituted only on success, and only
// when the pointer belongs to the system GL stack or to another interposer
// (remember()): dlsym is also how a program asks whether something exists.
// The lookup is the NEXT dlsym's (next_dlsym()): an interposer behind us that
// hands the application its own hook still does, and we forward to it. With
// VOCEM_DISABLE=1 nothing is remembered or replaced -- the way out of a broken
// interposition switches off the interposition, not just the drawing -- and
// the application gets exactly what it would get without us.
//
// Not substituted either for a question from further down the chain, or from
// another interposer (asked_by_chain()): both want what is underneath.
//
// RTLD_NEXT is answered by a tail call, never looked at. glibc finds the
// caller of dlsym by its return address; a jump leaves the caller's own on the
// stack, so the lookup is relative to whoever asked. Called normally, it was
// relative to this library: an interposer preloaded after us that forwards with
// dlsym(RTLD_NEXT, name) was handed its own function and recursed until the
// stack ran out (tests/shim_chain.cpp). Nothing is dlopened -- MangoHud's
// workaround (return address + dlopen(RTLD_NOLOAD)) is still not copied: it
// crashes on NVIDIA. Nothing is substituted on this road, and nothing is lost:
// an object after us asks for what is after it, which is never this library,
// and the executable's RTLD_NEXT finds our export first. RTLD_DEFAULT searches
// the global scope from the top whoever asks, so it needs no such care.
#if !defined(__has_attribute) || !__has_attribute(musttail)
#error "the shim's dlsym needs a guaranteed tail call (GCC 15, Clang 13): without it RTLD_NEXT is answered for the wrong caller"
#endif
void* dlsym(void* handle, const char* name) {
    if (handle == RTLD_NEXT) {
        PFN_dlsym real = find_real_dlsym();
        if (!real) {
            return nullptr;
        }
        __attribute__((musttail)) return real(handle, name);
    }

    static int hook_enabled = -1;  // -1 unknown, 0 off, 1 on; the disabled() shape
    int enabled = __atomic_load_n(&hook_enabled, __ATOMIC_ACQUIRE);
    if (enabled < 0) {
        const char* env = getenv("VOCEM_NO_DLSYM");
        enabled = (env && env[0] == '1') ? 0 : 1;
        __atomic_store_n(&hook_enabled, enabled, __ATOMIC_RELEASE);
    }

    void* real = next_dlsym(handle, name);
    if (enabled == 1 && real && !disabled()) {
        if (Hook* entry = find_hook(name)) {
            // The only moment the real function of an RTLD_LOCAL library is
            // visible. First one wins: one system GL stack per process.
            if (!inside(kForwarding | kAskingNext) &&
                !asked_by_link(*entry, __builtin_return_address(0)) && remember(*entry, real)) {
                return entry->hook;
            }
        }
    }
    return real;
}

}  // extern "C"

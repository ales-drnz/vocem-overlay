// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Another OpenGL interposer, preloaded after the shim, for tests/shim_chain.cpp.
//
// It exports the EGL present and counts the frames it is given,
// remembering where it forwarded each one. Three ways a real interposer finds
// the function it forwards to decide whether a chain holds, and each is a
// build of this file (the first twice, and once more with CHAIN_ASKS):
//
//   CHAIN_OWN_HANDLE  MangoHud's shape: an unversioned `dlsym` of its own that
//                     hands its hooks to an application asking a GL handle,
//                     and the real function taken from its own dlopen of the
//                     system library through libc's dlsym. Nothing it does
//                     passes through the shim. Built twice: as MangoHud 0.8.4
//                     is, its table of hooks relocated against its own
//                     exported names (R_X86_64_64 in libMangoHud_shim.so) --
//                     which bind to the first definition in the global scope,
//                     so behind the shim it hands out the SHIM's hooks -- and
//                     linked -Bsymbolic, handing out its own.
//   CHAIN_NEXT        dlsym(RTLD_NEXT, name), the textbook forward. It goes
//                     through the shim's interposed dlsym, so it measures
//                     whether RTLD_NEXT is still answered for its caller.
//   CHAIN_DISPATCHER  the real function from eglGetProcAddress, itself found
//                     with dlsym(RTLD_DEFAULT) -- alone, libEGL's; behind the
//                     shim, the shim's, which hands back the shim's own hook.
//                     It measures a forward that lands back in the shim.
//
// CHAIN_ASKS, beside CHAIN_OWN_HANDLE: its present also asks the GLOBAL
// eglGetProcAddress for a name it does not hook (eglGetDisplay) and counts
// the nulls -- a link looking something up through the shim mid-frame, which
// the refutation of the chain change found answered null for every name.
//
// Only the first shape has a dispatcher and a damage-aware present of its own,
// as MangoHud does: the other two could not reach the system's dispatcher
// without passing through the shim, and exporting one would make the
// RTLD_DEFAULT lookup above find itself.
//
// A frame that enters the same interposer more than four times on one thread
// is a loop; it is said and the process exits 99 rather than running out of
// stack, so the test reports the loop instead of a crash.

#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "real_dlsym.h"

#define EXPORT extern "C" __attribute__((visibility("default")))

namespace {

using PFN_swap = unsigned (*)(void*, void*);
using PFN_damage = unsigned (*)(void*, void*, int*, int);
using PFN_dlsym = void* (*)(void*, const char*);
using PFN_proc = void* (*)(const char*);

__thread int t_depth = 0;
int g_calls = 0;
int g_nulls = 0;
char g_target[256] = "";

PFN_dlsym libc_dlsym() {
    static const char* const versions[] = VOCEM_DLSYM_VERSIONS;
    for (const char* version : versions) {
        if (void* found = dlvsym(RTLD_NEXT, "dlsym", version)) {
            return reinterpret_cast<PFN_dlsym>(found);
        }
    }
    return nullptr;
}

void* real_of(const char* name) {
#if defined(CHAIN_OWN_HANDLE)
    void* handle = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    PFN_dlsym real = libc_dlsym();
    return handle && real ? real(handle, name) : nullptr;
#elif defined(CHAIN_NEXT)
    return dlsym(RTLD_NEXT, name);
#elif defined(CHAIN_DISPATCHER)
    PFN_proc get_proc = reinterpret_cast<PFN_proc>(dlsym(RTLD_DEFAULT, "eglGetProcAddress"));
    return get_proc ? get_proc(name) : nullptr;
#else
#error "build with one of CHAIN_OWN_HANDLE, CHAIN_NEXT, CHAIN_DISPATCHER"
#endif
}

void enter(const char* name) {
    if (++t_depth > 4) {
        char line[160];
        const int length = snprintf(line, sizeof line, "[chain] %s re-entered itself: a loop\n", name);
        (void)!write(2, line, static_cast<size_t>(length));
        _exit(99);
    }
    ++g_calls;
}

void note_target(void* target) {
    Dl_info info;
    const char* where = "(null)";
    if (target && dladdr(target, &info) && info.dli_fname) {
        const char* slash = strrchr(info.dli_fname, '/');
        where = slash ? slash + 1 : info.dli_fname;
    }
    snprintf(g_target, sizeof g_target, "%s", where);
}

}  // namespace

#if defined(CHAIN_ASKS)
EXPORT void* eglGetProcAddress(const char* name);
#endif

EXPORT unsigned eglSwapBuffers(void* display, void* surface) {
    enter("eglSwapBuffers");
#if defined(CHAIN_ASKS)
    if (!eglGetProcAddress("eglGetDisplay")) {
        ++g_nulls;
    }
#endif
    PFN_swap real = reinterpret_cast<PFN_swap>(real_of("eglSwapBuffers"));
    note_target(reinterpret_cast<void*>(real));
    const unsigned result = real ? real(display, surface) : 0u;
    --t_depth;
    return result;
}

// Hooked here although MangoHud does not: the shim remembers what a
// dispatcher answers for these, and this is the answer that comes from the
// chain rather than from the system.
#if defined(CHAIN_OWN_HANDLE)
EXPORT unsigned eglSwapBuffersWithDamageEXT(void* display, void* surface, int* rects, int n) {
    enter("eglSwapBuffersWithDamageEXT");
    PFN_proc real_proc = reinterpret_cast<PFN_proc>(real_of("eglGetProcAddress"));
    PFN_damage real =
        real_proc ? reinterpret_cast<PFN_damage>(real_proc("eglSwapBuffersWithDamageEXT")) : nullptr;
    note_target(reinterpret_cast<void*>(real));
    const unsigned result = real ? real(display, surface, rects, n) : 0u;
    --t_depth;
    return result;
}

EXPORT void* eglGetProcAddress(const char* name) {
    if (name && strcmp(name, "eglSwapBuffers") == 0) {
        return reinterpret_cast<void*>(&eglSwapBuffers);
    }
    if (name && strcmp(name, "eglSwapBuffersWithDamageEXT") == 0) {
        return reinterpret_cast<void*>(&eglSwapBuffersWithDamageEXT);
    }
    PFN_proc real = reinterpret_cast<PFN_proc>(real_of("eglGetProcAddress"));
    return real ? real(name) : nullptr;
}

// MangoHud's shape: unversioned, and consulted only if whoever is ahead asks
// the next dlsym in the chain rather than libc's own.
EXPORT void* dlsym(void* handle, const char* name) {
    PFN_dlsym real = libc_dlsym();
    void* answer = real ? real(handle, name) : nullptr;
    if (answer && name && handle != RTLD_NEXT && handle != RTLD_DEFAULT) {
        if (strcmp(name, "eglSwapBuffers") == 0) {
            return reinterpret_cast<void*>(&eglSwapBuffers);
        }
        if (strcmp(name, "eglGetProcAddress") == 0) {
            return reinterpret_cast<void*>(&eglGetProcAddress);
        }
    }
    return answer;
}
#endif

// What the test reads back.
EXPORT int vocem_chain_calls() {
    return g_calls;
}

EXPORT const char* vocem_chain_target() {
    return g_target;
}

EXPORT int vocem_chain_nulls() {
    return g_nulls;
}

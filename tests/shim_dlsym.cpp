// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The shim's `dlsym` answers, at the architecture it was built for.
//
// The shim interposes `dlsym` for the whole session, which means it does not get to
// be approximately right: an application that asks for any symbol at all gets our
// answer, and every answer comes from the real `dlsym` we found underneath. Find
// nothing and the answer is null for everything, in every process on the machine.
//
// That is not hypothetical. `GLIBC_2.2.5` was hardcoded as the symbol version, which
// is what x86-64 uses and what i386 has never had, so the 64-bit half of the install
// worked and the 32-bit half turned every lookup into null. The Steam client's own
// binary is `ubuntu12_32/steam`: it asked for `setenv`, got null, and shut down a
// second after starting. Nothing in the build said anything -- the 32-bit libraries
// are configured in a tree of their own with tests switched off, so nothing had ever
// run against them.
//
// This test therefore exists to be built twice. The second build is the one that
// matters; see tests/CMakeLists.txt, which compiles it again with `-m32` whenever
// the toolchain can, and reports itself skipped rather than passing when it cannot.
//
// It checks the resolution and then the whole path: a symbol that has nothing to do
// with graphics, asked for the way an application asks, through the interposed
// `dlsym` in the preloaded library -- `setenv` itself, which is the one Steam wanted.

#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "real_dlsym.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

int main() {
    printf("built for %zu-bit, native dlsym version %s\n", sizeof(void*) * 8,
           VOCEM_DLSYM_NATIVE_VERSION);

    // The architecture's own version has to be one this libc actually defines.
    // Asked of libc directly rather than through RTLD_NEXT, because this program
    // is not the shim and has nothing in front of it.
    void* native = dlvsym(RTLD_DEFAULT, "dlsym", VOCEM_DLSYM_NATIVE_VERSION);
    check(native != nullptr, "the version this architecture was given resolves");

    // And the list as a whole, which is what the shim walks: at least one of them
    // must resolve, or the shim answers null to everything.
    static const char* const versions[] = VOCEM_DLSYM_VERSIONS;
    void* found = nullptr;
    const char* found_as = nullptr;
    for (unsigned i = 0; i < sizeof(versions) / sizeof(versions[0]) && !found; ++i) {
        found = dlvsym(RTLD_DEFAULT, "dlsym", versions[i]);
        found_as = versions[i];
    }
    check(found != nullptr, "and so does something in the list the shim walks");
    if (found) {
        printf("     resolved as %s\n", found_as);
    }
    check(found == native, "the list finds the same one the architecture names first");

    // The whole path, as an application walks it. This binary is run with the shim
    // preloaded, so `dlsym` here *is* the shim's, and a symbol with nothing to do
    // with graphics has to come back unchanged. `setenv` by name: it is the one the
    // Steam client asked for and did not get.
    void* setenv_symbol = dlsym(RTLD_DEFAULT, "setenv");
    check(setenv_symbol != nullptr, "a symbol that is nothing to do with us still resolves");

    void* also_fine = dlsym(RTLD_DEFAULT, "getenv");
    check(also_fine != nullptr, "and another, in case the first was a lucky cache");

    // The hooks the shim is actually for still come back as the shim's own, so a
    // fix to the above cannot quietly turn the interposition off.
    void* swap = dlsym(RTLD_DEFAULT, "glXSwapBuffers");
    if (getenv("VOCEM_SHIM_PRELOADED")) {
        check(swap != nullptr, "and the swap function the shim exists for is still hooked");
    }

    // The dispatch function we hand back has to be able to answer for everything,
    // not only for the names we replace.
    //
    // This is the way an application that never links libEGL reaches it: `dlopen`
    // with RTLD_LOCAL, then `dlsym` on that handle. Nothing opened that way is in
    // the global scope, so the shim's own `RTLD_NEXT` lookup for the real
    // `eglGetProcAddress` finds nothing -- while the interposed `dlsym` hands our
    // `eglGetProcAddress` to the caller regardless. Every extension then came back
    // null from a function the caller had no reason to distrust.
    //
    // ANGLE resolves `eglCreateImageKHR` during `eglInitialize`, gives up when it is
    // null, and Chromium then runs with no GPU process at all. Measured on Discord,
    // on a session where this shim was preloaded: the entire application composited
    // in software. Every Electron application on the machine was affected the same
    // way. Nothing in the build said anything, because every test until this one
    // asked the shim for names the shim knows.
    // Whether the ANGLE half below was actually walked. A `skip` printed into
    // the middle of a run is not a skip: this file's own line was the only
    // `printf("skip ` in tests/ that was not followed by `return 77`, and the
    // test was registered with no SKIP flag either -- so on a machine without
    // libEGL.so.1, INCLUDING the 32-bit twin, which is the width entries
    // 30/33/34 say fails invisibly, the whole entry-35 check did not run and
    // ctest printed Passed. Entry 123 swept this shape out of the CMake side
    // and left this one in a .cpp.
    bool angle_walked = false;
    if (getenv("VOCEM_SHIM_PRELOADED")) {
        void* egl = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
        if (!egl) {
            printf("skip libEGL.so.1 is not installed, so the ANGLE path cannot be walked\n");
        } else {
            angle_walked = true;
            using PFN = void* (*)(const char*);
            PFN get_proc = reinterpret_cast<PFN>(dlsym(egl, "eglGetProcAddress"));
            check(get_proc != nullptr, "eglGetProcAddress resolves out of a private handle");
            if (get_proc) {
                // Two extensions rather than one: the first is the one ANGLE stops
                // at, and the second says the answer is a working dispatch function
                // rather than a single name that happened to be special-cased.
                check(get_proc("eglCreateImageKHR") != nullptr,
                      "and answers for eglCreateImageKHR, which is where ANGLE gives up");
                check(get_proc("eglDestroyImageKHR") != nullptr,
                      "and for another extension beside it");
                // And still hooks what it is here to hook.
                check(get_proc("eglSwapBuffers") != nullptr,
                      "while still answering for the present function");
            }
        }
    }

    if (failures == 0 && getenv("VOCEM_SHIM_PRELOADED") && !angle_walked) {
        // Everything else passed and the half this file exists for did not run.
        // Reported as the skip it is, whole: a partial measurement that calls
        // itself a pass is what entry 123 is about, and the checks above are
        // covered at the other width and in shim_two_egls besides.
        printf("skip the ANGLE path could not be walked here, so this is not a measurement\n");
        return 77;
    }
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Two dispatch functions with the same name, one process.
//
// Every Electron application carries this topology: ANGLE's bundled libEGL.so
// (soname `libEGL.so`) is opened privately by Chromium, and later ANGLE's own
// native backend opens the *system* libEGL.so.1 privately underneath it. Two
// different `eglGetProcAddress` therefore live in one process, and the shim's
// `dlsym` hook meets both.
//
// The shim as shipped in 0.1.0-42 kept one global slot per name, first pointer
// wins. The first one seen is ANGLE's, so when ANGLE asked what it believed was
// the *driver's* dispatcher for a native entry point, the shim forwarded the
// question to ANGLE itself. ANGLE re-entered its own initialisation, took its
// own display lock a second time, and the GPU process of every Electron
// application froze before it could rewrite its own argv -- measured on Discord:
// five threads, the main one in futex_wait, the NVIDIA vendor library never
// loaded, and the splash screen up forever. Where -40 broke Electron loudly,
// -42 broke it silently.
//
// This test walks exactly that sequence with a stub playing ANGLE, and asserts
// that a dispatcher obtained from the *system* library answers with the system's
// pointers. It fails against the -42 shim -- the answer comes back the stub's --
// and against the -40 one, where the answer comes back null.
//
// The first check runs in a forked child because the failure it looks for is
// unbounded recursion: `dlsym(RTLD_DEFAULT, "eglGetProcAddress")` resolves the
// shim's *own* export -- the shim is first in the global scope -- and a shim that
// remembers that pointer as "the real one" has its hook forwarding to itself. It
// has to run before anything else touches EGL, on slots nothing has filled yet.

#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

using PFN_get_proc = void* (*)(const char*);
using PFN_marker = void* (*)(void);

}  // namespace

int main() {
    if (!getenv("VOCEM_SHIM_PRELOADED")) {
        printf("skip meant to run with the shim preloaded; set VOCEM_SHIM_PRELOADED=1\n");
        return 77;
    }

    const char* stub_path = getenv("VOCEM_STUB_EGL");
    if (!stub_path || !stub_path[0]) {
        printf("skip VOCEM_STUB_EGL not set: no stub library to play ANGLE\n");
        return 77;
    }

    // 0. The shim's own export, asked back through RTLD_DEFAULT and then called,
    //    *before anything else has filled the shim's slots*. RTLD_DEFAULT searches
    //    the global scope from the front, and the front is the shim itself -- so the
    //    "real" dispatcher this lookup produces is the shim's own hook. A shim that
    //    remembers that pointer as the real one has its hook forwarding to itself,
    //    and the first call recurses until the stack is gone. In a child, so the
    //    parent can report the corpse instead of becoming one.
    fflush(stdout);
    pid_t early = fork();
    if (early == 0) {
        alarm(5);
        PFN_get_proc gpd =
            reinterpret_cast<PFN_get_proc>(dlsym(RTLD_DEFAULT, "eglGetProcAddress"));
        if (gpd) {
            gpd("eglCreateImageKHR");
        }
        _exit(0);
    }
    int early_status = 0;
    waitpid(early, &early_status, 0);
    check(WIFEXITED(early_status) && WEXITSTATUS(early_status) == 0,
          "asking the shim's own export, on virgin slots, does not recurse into itself");
    if (WIFSIGNALED(early_status)) {
        printf("     child died with signal %d\n", WTERMSIG(early_status));
    }

    // 1. ANGLE arrives first, privately -- as in Chromium.
    void* h1 = dlopen(stub_path, RTLD_LAZY | RTLD_LOCAL);
    if (!h1) {
        printf("FAIL could not open the stub: %s\n", dlerror());
        return 1;
    }
    // Asked under VOCEM_DEBUG, with this process's stderr caught in a file:
    // the shim leaves ANGLE's dispatcher alone, and says so -- the one line a
    // system library under a name it does not know would get too (the shape of
    // entry 38; 0.1.12-2's shim said nothing). Set before the first question
    // the shim declines, because it reads the variable once.
    setenv("VOCEM_DEBUG", "1", 1);
    char said_path[] = "/tmp/vocem-two-egls-XXXXXX";
    const int said_fd = mkstemp(said_path);
    const int saved_stderr = dup(2);
    check(said_fd >= 0 && saved_stderr >= 0, "stderr can be caught");
    dup2(said_fd, 2);
    PFN_get_proc gp1 = reinterpret_cast<PFN_get_proc>(dlsym(h1, "eglGetProcAddress"));
    dup2(saved_stderr, 2);
    close(saved_stderr);
    char said[1024] = {};
    const ssize_t said_length = pread(said_fd, said, sizeof(said) - 1, 0);
    close(said_fd);
    unlink(said_path);
    check(said_length > 0 && strstr(said, "not following eglGetProcAddress into ") &&
              strstr(said, stub_path),
          "under VOCEM_DEBUG the shim says it does not follow the private dispatcher");
    PFN_marker marker_fn = reinterpret_cast<PFN_marker>(dlsym(h1, "vocem_stub_marker"));
    check(gp1 != nullptr, "the first library's dispatcher resolves");
    check(marker_fn != nullptr, "and its marker with it");
    if (!gp1 || !marker_fn) {
        return 1;
    }
    void* marker = marker_fn();

    // 2. The system's libEGL arrives second, also privately -- as in ANGLE's
    //    native backend.
    void* h2 = dlopen("libEGL.so.1", RTLD_LAZY | RTLD_LOCAL);
    if (!h2) {
        printf("skip libEGL.so.1 is not installed, so the two-EGL topology cannot be built\n");
        return 77;
    }
    PFN_get_proc gp2 = reinterpret_cast<PFN_get_proc>(dlsym(h2, "eglGetProcAddress"));
    check(gp2 != nullptr, "the system dispatcher resolves out of its own handle");
    if (!gp2) {
        return 1;
    }

    // 3. The question ANGLE asks the driver. The answer must be the system's:
    //    the stub's marker here is the -42 poisoning, null is the -40 breakage.
    void* q = gp2("eglCreateImageKHR");
    check(q != marker,
          "a dispatcher from the system library must not answer with the first library's pointers");
    check(q != nullptr, "and must answer at all (eglCreateImageKHR, where ANGLE gives up)");

    // 4. The first library's own consumer still gets the first library's answers:
    //    whoever holds ANGLE's dispatcher expects ANGLE's frontend, not the driver.
    void* p = gp1("eglCreateImageKHR");
    check(p == marker, "the first library's dispatcher still answers with its own pointers");

    // 5. The present function on the *system* handle is still the shim's hook --
    //    the fix must not quietly turn the interposition off.
    void* sw2 = dlsym(h2, "eglSwapBuffers");
    void* ours = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
    check(sw2 != nullptr && sw2 == ours,
          "the system library's present function is still hooked");

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

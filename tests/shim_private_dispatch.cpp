// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What the shim's dispatch hooks hand back when the real dispatcher is not the
// system's.
//
// Entry 36's rule is that a pointer is remembered only when it belongs to the
// system's GL stack, and that a private GL keeps its private names. The `dlsym`
// hook obeys both halves: it substitutes our hook only inside
// `if (is_system_gl(real))`, so a private library's `eglSwapBuffers` comes back
// as itself. `dispatch()` -- the shared body of the three proc-address hooks --
// obeys the first half and not the second:
//
//     if (Hook* entry = find_hook(name)) {
//         if (is_system_gl(answer)) { ...remember... }
//         return entry->hook;          // whatever the answer's origin
//     }
//
// The `return` is outside the test. Whether that can be reached at all depends
// on `real_for()`, whose `next` slot is `dlsym(RTLD_NEXT, ...)` and is
// **deliberately** unvetted -- the comment above `Hook` gives a reason for that
// which is about level 1, the link-time substitution, and does not obviously
// carry to level 2. Reasoning about which of two implementations lands in
// RTLD_NEXT is exactly what entry 36 was written to stop somebody doing, so it
// is constructed here instead.
//
// **The case can be constructed, and the asymmetry is real.** A private EGL
// opened RTLD_GLOBAL lands in the shim's own RTLD_NEXT order, fills `next`, and
// from then on the shim's exported `eglGetProcAddress` answers `eglSwapBuffers`
// with OUR hook -- while `dlsym` on the same library, for the same name, answers
// with the library's own function. One process, two roads, two answers.
//
// What that costs is a second question, and the honest answer is: in the
// ordinary shape of it, nothing. Our hook forwards through
// `real_for_name("eglSwapBuffers")`, which reaches the same RTLD_NEXT and so the
// same private library. The two roads disagree about the pointer and agree about
// where the frame goes.
//
// The shape where they would not agree is the conjunction in case 3: `seen` for
// `eglSwapBuffers` already holds the SYSTEM library's function -- put there by
// the `dlsym` hook, which is right to remember it -- while the dispatcher being
// asked is the private one. Then the caller asks a private GL for its present
// function, is handed our hook, and our hook forwards to the system's. That
// conjunction is constructed here and the two pointers are printed. What is NOT
// done here is calling the thing: `eglSwapBuffers` on this path opens the heavy
// library and wants a real display, and a test that presents a frame is not what
// this file is. So the mismatch is shown as pointers, and stated as pointers.
//
// Kept as a measurement rather than turned into a fix, because the fix is not
// obviously the symmetric one: moving the `return` inside `is_system_gl` would
// make the shim hand a private dispatcher's answer straight back, which is
// entry 35's defect -- a dispatch function that stops answering for names it
// used to answer for -- in the case where `next` legitimately IS the
// application's own linked GL. The decision is the owner's; this file is so that
// it is taken on a measurement.
//
// **Taken. The answer is that it stays as it is, and the measurement that
// settles it is in tests/gl_beside_mangohud.cmake.** The question was which
// rule wins, and it turns on what actually stands in `next` on a real machine.
// It is not ANGLE. MangoHud 0.8.4 preloads `libMangoHud_shim.so`, which exports
// the same ten names this shim does and whose basename fails `is_system_gl()`
// exactly as ANGLE's `libEGL.so` does -- and the `mangohud` wrapper appends it
// AFTER the session's preload, so it is our RTLD_NEXT. Returning `entry->hook`
// from outside the test is therefore not a hole: it is what keeps this overlay
// in the chain when the thing below it is another interposer. Under the
// symmetric change that same call hands the application MangoHud's hook, which
// is exactly the state measured in the other preload order -- zero of our log
// lines, none of our pixels. The change would trade the one order we win for
// the one we already lose.
//
// What `is_system_gl` cannot do is separate two questions that both fail its
// name test and want opposite answers: "another implementation I must not
// cross-wire" (ANGLE -- do not substitute) and "the next link in the chain"
// (MangoHud, Steam -- substitute and forward there). Telling them apart by name
// would be MangoHud's own blacklist, which this project refuses on principle.
// The shape a real answer would take is structural rather than nominal: a chain
// interposer is always in LD_PRELOAD and ANGLE never is, and `dladdr` already
// yields the object's path. Not attempted and not measured -- written down so
// the next reader starts from it instead of from the symmetric change.
//
// Each case runs in its own forked child. The shim's slots are process-global
// and fill once, so a case that ran after another would be measuring the first
// one's leftovers.

#include <dlfcn.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

using PFN_get_proc = void* (*)(const char*);
using PFN_marker = void* (*)(void);

const char* g_stub = nullptr;

// Runs `body` in a child and reports its exit code. The children here load two
// EGLs and call dispatchers on virgin slots; a failure mode of that is
// unbounded recursion, which must be a result rather than a dead suite.
template <typename Body>
int in_child(Body body) {
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        alarm(10);
        _exit(body());
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        printf("     child died with signal %d\n", WTERMSIG(status));
        return -1;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

// Exit codes the children speak in, so the parent does the reporting.
enum : int {
    kOurHook = 10,        // the shim answered with its own hook
    kPrivateAnswer = 11,  // the shim answered with the private library's pointer
    kSomethingElse = 12,
    kCouldNotBuild = 13,  // the topology could not be built: not an answer
};

}  // namespace

int main() {
    if (!getenv("VOCEM_SHIM_PRELOADED")) {
        printf("skip meant to run with the shim preloaded; set VOCEM_SHIM_PRELOADED=1\n");
        return 77;
    }
    g_stub = getenv("VOCEM_STUB_PRIVATE_EGL");
    if (!g_stub || !g_stub[0]) {
        printf("skip VOCEM_STUB_PRIVATE_EGL not set: no private EGL to put in the global scope\n");
        return 77;
    }

    // -----------------------------------------------------------------------
    // 1. The control: the `dlsym` road. A private library's present function,
    //    asked for through `dlsym` on that library's own handle, must come back
    //    as the library's own -- entry 36's rule, the half that is obeyed.
    // -----------------------------------------------------------------------
    const int dlsym_road = in_child([] {
        void* handle = dlopen(g_stub, RTLD_NOW | RTLD_GLOBAL);
        if (!handle) {
            return static_cast<int>(kCouldNotBuild);
        }
        auto marker = reinterpret_cast<PFN_marker>(dlsym(handle, "vocem_private_swap_marker"));
        if (!marker) {
            return static_cast<int>(kCouldNotBuild);
        }
        void* ours = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
        void* asked = dlsym(handle, "eglSwapBuffers");
        if (asked == ours) {
            return static_cast<int>(kOurHook);
        }
        // The stub's exported eglSwapBuffers, not its marker function.
        return asked ? static_cast<int>(kPrivateAnswer) : static_cast<int>(kSomethingElse);
    });
    if (dlsym_road == kCouldNotBuild) {
        printf("skip the private EGL could not be placed in the global scope\n");
        return 77;
    }
    check(dlsym_road == kPrivateAnswer,
          "dlsym on a private EGL answers with the private EGL's own present function");

    // -----------------------------------------------------------------------
    // 2. The road in question: the shim's own exported `eglGetProcAddress`,
    //    with the private EGL in RTLD_NEXT so that `real_for()` resolves the
    //    dispatcher to it. Same process shape, same name, other road.
    // -----------------------------------------------------------------------
    const int dispatch_road = in_child([] {
        // RTLD_GLOBAL is what puts it in the shim's RTLD_NEXT order; RTLD_LOCAL
        // -- what Chromium really does with ANGLE -- is invisible there, which
        // is why the existing two-EGL test cannot reach this path at all.
        void* handle = dlopen(g_stub, RTLD_NOW | RTLD_GLOBAL);
        if (!handle) {
            return static_cast<int>(kCouldNotBuild);
        }
        auto ours_proc = reinterpret_cast<PFN_get_proc>(dlsym(RTLD_DEFAULT, "eglGetProcAddress"));
        if (!ours_proc) {
            return static_cast<int>(kCouldNotBuild);
        }
        void* ours_swap = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
        void* answer = ours_proc("eglSwapBuffers");
        if (answer == ours_swap) {
            return static_cast<int>(kOurHook);
        }
        return answer ? static_cast<int>(kPrivateAnswer) : static_cast<int>(kSomethingElse);
    });
    if (dispatch_road == kCouldNotBuild) {
        printf("skip the dispatcher road could not be built\n");
        return 77;
    }

    // This is the finding, asserted in the direction it was measured. If a
    // later shim makes the two roads agree, this check is what says so, and the
    // entry above has to be rewritten rather than the assertion flipped
    // quietly.
    check(dispatch_road == kOurHook,
          "the dispatch road answers with OUR hook, where dlsym answered with the private one");
    printf("--  dlsym road: %s;  dispatch road: %s\n",
           dlsym_road == kOurHook ? "our hook" : "the private EGL's own",
           dispatch_road == kOurHook ? "our hook" : "the private EGL's own");
    printf("--  so entry 36's second half -- a private GL keeps its private names -- holds on\n");
    printf("--  one road and not the other. Reachable only with a private EGL in the GLOBAL\n");
    printf("--  scope: RTLD_LOCAL, which is what Chromium does, never reaches it.\n");

    // -----------------------------------------------------------------------
    // 3. The conjunction that would actually send a frame the wrong way: the
    //    system's `eglSwapBuffers` remembered in `seen` first -- which the
    //    `dlsym` hook is right to do -- and the private dispatcher asked
    //    afterwards. The caller then holds our hook, and our hook forwards to
    //    the system library rather than to the one it asked.
    // -----------------------------------------------------------------------
    const int conjunction = in_child([] {
        void* system_egl = dlopen("libEGL.so.1", RTLD_NOW | RTLD_LOCAL);
        if (!system_egl) {
            return static_cast<int>(kCouldNotBuild);
        }
        // Fills the shim's `seen` slot for eglSwapBuffers with the system's,
        // because is_system_gl() passes for it. The hook comes back to us here,
        // which is entry 36 working exactly as intended.
        void* from_system = dlsym(system_egl, "eglSwapBuffers");
        void* ours_swap = dlsym(RTLD_DEFAULT, "eglSwapBuffers");
        if (from_system != ours_swap) {
            return static_cast<int>(kCouldNotBuild);  // the system road is not hooked: no case
        }
        void* handle = dlopen(g_stub, RTLD_NOW | RTLD_GLOBAL);
        if (!handle) {
            return static_cast<int>(kCouldNotBuild);
        }
        (void)handle;
        // Asked through the shim's OWN exported dispatcher, which is the only
        // road into dispatch(). Asking the private library's handle directly
        // does not reach it: the dlsym hook declines to substitute for a
        // private pointer and hands back the private dispatcher itself, so that
        // call never enters our code at all -- measured, and the reason this
        // case is written this way round.
        auto ours_proc = reinterpret_cast<PFN_get_proc>(dlsym(RTLD_DEFAULT, "eglGetProcAddress"));
        if (!ours_proc) {
            return static_cast<int>(kCouldNotBuild);
        }
        void* answer = ours_proc("eglSwapBuffers");
        if (answer == ours_swap) {
            return static_cast<int>(kOurHook);
        }
        return answer ? static_cast<int>(kPrivateAnswer) : static_cast<int>(kSomethingElse);
    });
    if (conjunction == kCouldNotBuild) {
        printf("--  case 3 not built here (no system libEGL, or its present is not hooked)\n");
    } else {
        printf("--  case 3, dispatch asked while seen already holds the SYSTEM's swap: %s\n",
               conjunction == kOurHook ? "our hook" : "the private EGL's own");
        printf("--  The precondition is measured: dlsym on the system handle came back as our\n");
        printf("--  hook, which is the shim having vetted and remembered that pointer. What\n");
        printf("--  follows from the code rather than from this run is where the hook then\n");
        printf("--  forwards -- real_for_name() returns `seen` before `next`, so it forwards to\n");
        printf("--  the system's function while the dispatcher consulted was the private one.\n");
        printf("--  Not called, only compared: eglSwapBuffers on this path opens the heavy\n");
        printf("--  library and wants a real display.\n");
    }

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

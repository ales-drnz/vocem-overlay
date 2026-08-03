// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// No file syscalls in the symbol-resolution paths -- measured, not promised.
//
// The shim is preloaded into processes whose sandbox rules it does not know and
// cannot ask about. Chromium runs its children under seccomp filters where an
// unexpected file syscall is SIGSYS and the process is killed rather than told
// no. The 0.1.0-41 shim put `dlopen(soname, RTLD_LAZY | RTLD_NOLOAD)` into the
// dispatch-resolution path on the theory that NOLOAD only looks at what is
// already mapped. Measured here with a trapping filter: for a soname that is
// *not* mapped, glibc walks the search path first -- three openat and four
// newfstatat -- before honouring NOLOAD. And "not mapped" is precisely the case
// the fallback existed for. Discord went from starting without hardware
// acceleration to not starting at all.
//
// This test is that sandbox, in miniature: a child installs a filter that kills
// the process on any file syscall, then walks the shim's resolution paths the
// way a sandboxed Chromium child does -- ordinary dlsym traffic, then the
// dispatch hooks, called while nothing they look for is mapped, which forces the
// fallback in a shim that has one. The parent reports the corpse. It fails
// against the -41 shim, SIGSYS, and passes against -40 and -42.

#include <dlfcn.h>
#include <linux/filter.h>
#include <signal.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "real_dlsym.h"

namespace {

// The real dlsym, reached the same way the shim reaches it, so asking for the
// shim's own exports does not pass through the shim's hook. Going through the
// hook would let the shim *remember* its own export as "the real dispatcher"
// and short-circuit the very resolution path this test exists to walk.
void* bypass_dlsym(void* handle, const char* name) {
    using PFN_dlsym = void* (*)(void*, const char*);
    static const char* const versions[] = VOCEM_DLSYM_VERSIONS;
    PFN_dlsym real = nullptr;
    for (unsigned i = 0; i < sizeof(versions) / sizeof(versions[0]) && !real; ++i) {
        real = reinterpret_cast<PFN_dlsym>(dlvsym(RTLD_DEFAULT, "dlsym", versions[i]));
    }
    return real ? real(handle, name) : nullptr;
}

int failures = 0;

void check(bool condition, const char* what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

// Kill the process on any file syscall. SECCOMP_RET_KILL_PROCESS rather than
// TRAP: the point is to fail the way a sandbox fails, loudly and terminally.
bool forbid_file_syscalls() {
#define KILL_ON(nr)                                       \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (nr), 0, 1),      \
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS)
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
#ifdef SYS_open
        KILL_ON(SYS_open),
#endif
        KILL_ON(SYS_openat),
#ifdef SYS_openat2
        KILL_ON(SYS_openat2),
#endif
#ifdef SYS_access
        KILL_ON(SYS_access),
#endif
        KILL_ON(SYS_faccessat),
#ifdef SYS_faccessat2
        KILL_ON(SYS_faccessat2),
#endif
#ifdef SYS_stat
        KILL_ON(SYS_stat),
#endif
#ifdef SYS_stat64
        KILL_ON(SYS_stat64),
#endif
#ifdef SYS_newfstatat
        KILL_ON(SYS_newfstatat),
#endif
#ifdef SYS_fstatat64
        KILL_ON(SYS_fstatat64),
#endif
#ifdef SYS_statx
        KILL_ON(SYS_statx),
#endif
#ifdef SYS_getdents64
        KILL_ON(SYS_getdents64),
#endif
#ifdef SYS_readlink
        KILL_ON(SYS_readlink),
#endif
#ifdef SYS_readlinkat
        KILL_ON(SYS_readlinkat),
#endif
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
#undef KILL_ON
    struct sock_fprog prog = {sizeof(filter) / sizeof(filter[0]), filter};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
        return false;
    }
    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) == 0;
}

using PFN_get_proc = void* (*)(const char*);

}  // namespace

int main() {
    if (!getenv("VOCEM_SHIM_PRELOADED")) {
        printf("skip meant to run with the shim preloaded; set VOCEM_SHIM_PRELOADED=1\n");
        return 77;
    }

    // The hooks' addresses, taken *around* the interposed dlsym rather than
    // through it, so the shim's slots stay exactly as fork left them: empty.
    PFN_get_proc egl =
        reinterpret_cast<PFN_get_proc>(bypass_dlsym(RTLD_DEFAULT, "eglGetProcAddress"));
    PFN_get_proc glx =
        reinterpret_cast<PFN_get_proc>(bypass_dlsym(RTLD_DEFAULT, "glXGetProcAddressARB"));

    fflush(stdout);
    pid_t pid = fork();
    if (pid == 0) {
        // A hook that loops instead of crashing must still produce a corpse the
        // parent can read, not a test that never ends.
        alarm(10);

        // Warm nothing on purpose: a sandboxed Chromium child meets the shim with
        // whatever state fork gave it, and the dangerous case is the unresolved one.
        if (!forbid_file_syscalls()) {
            _exit(66);
        }

        // Ordinary dlsym traffic, the thing every process does constantly.
        if (!dlsym(RTLD_DEFAULT, "getenv")) {
            _exit(65);
        }

        // The dispatch hooks, called while no GL library is mapped anywhere in
        // this process, so every lookup inside them comes up empty -- which is
        // exactly when a fallback that touches the filesystem would fire, on
        // every single call in a shim that does not remember the attempt.
        for (int i = 0; i < 3; ++i) {
            if (egl) {
                egl("eglCreateImageKHR");
                egl("eglSwapBuffersWithDamageKHR");
            }
            if (glx) {
                glx("glXSwapIntervalEXT");
            }
        }
        _exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status) && WEXITSTATUS(status) == 66) {
        printf("skip this kernel refused the seccomp filter, so the sandbox cannot be built\n");
        return 77;
    }
    check(!WIFSIGNALED(status), "the resolution paths made no file syscall under a killing sandbox");
    if (WIFSIGNALED(status)) {
        const char* what = "";
        if (WTERMSIG(status) == SIGSYS) {
            what = " (SIGSYS: a forbidden syscall, which is the -41 bug)";
        } else if (WTERMSIG(status) == SIGALRM) {
            what = " (SIGALRM: the child never finished -- a hook looping on itself)";
        }
        printf("     child killed by signal %d%s\n", WTERMSIG(status), what);
    }
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "and the child walked every path to the end");
    if (WIFEXITED(status) && WEXITSTATUS(status) == 65) {
        printf("     dlsym itself failed under the sandbox\n");
    }

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

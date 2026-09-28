// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A process that is running in a Flatpak sandbox of a given application id,
// as far as the daemon can tell: a bwrap with its own user and pid namespace,
// a tmpfs root, and a /.flatpak-info naming the id -- the shape
// daemon_peer_sandbox's listener has. It sleeps and does nothing else.
//
// The Flatpak bridge serves a directory under $XDG_RUNTIME_DIR/app only while
// a process of that application is running (flatpak_process.h in the daemon):
// a directory's name is not evidence, because a sandbox holding the
// xdg-run/app grant can make one under any name. The bridge's tests used to
// serve hand-made directories with nothing behind them, which is exactly the
// shape that is refused now, so the ones that expect to be served start one of
// these first.
//
// Under the daemon's unit (unit_confinement.h) no bwrap can be built -- the
// unit's RestrictNamespaces refuses one -- and a /.flatpak-info could not be
// read from there anyway (entry 285). What the daemon sees of a real Flatpak
// from its unit is the systemd scope Flatpak starts it in, so there the
// process is a `sleep` in `app-flatpak-<id>-<n>.scope`, which is what
// daemon_unit_peer's bridge case measures.

#ifndef VOCEM_TESTS_FAKE_FLATPAK_H
#define VOCEM_TESTS_FAKE_FLATPAK_H

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

namespace vocem_test {

struct FakeFlatpak {
    pid_t pid = -1;
};

// Whether bwrap is there to build one. A test without it skips (77).
inline bool fake_flatpak_available() {
    if (getenv("VOCEM_UNIT_CONFINED")) {
        return system("command -v systemd-run >/dev/null 2>&1") == 0;
    }
    return system("command -v bwrap >/dev/null 2>&1") == 0;
}

// Starts the process and returns once it is inside its sandbox (it says so on
// a pipe before it execs sleep, so its root is the sandbox's by then).
// `scratch` is a directory the /.flatpak-info can be written into.
inline FakeFlatpak start_fake_flatpak(const std::string& scratch, const char* app_id) {
    FakeFlatpak process;
    const std::string info = scratch + "/flatpak-info-" + app_id;
    if (FILE* file = fopen(info.c_str(), "wb")) {
        fprintf(file, "[Application]\nname=%s\nruntime=runtime/x/y/z\n", app_id);
        fclose(file);
    } else {
        return process;
    }
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        return process;
    }
    fflush(stdout);
    fflush(stderr);
    const pid_t pid = fork();
    if (pid == 0) {
        close(pipe_fds[0]);
        dup2(pipe_fds[1], 1);
        close(pipe_fds[1]);
        if (getenv("VOCEM_UNIT_CONFINED")) {
            const std::string scope = std::string("--unit=app-flatpak-") + app_id + "-" +
                                      std::to_string(getpid()) + ".scope";
            execlp("systemd-run", "systemd-run", "--user", "--quiet", "--scope", "--collect",
                   "--slice=app.slice", scope.c_str(), "/usr/bin/sh", "-c",
                   "echo ready; exec sleep 600", (char*)nullptr);
            _exit(127);
        }
        execlp("bwrap", "bwrap", "--unshare-user", "--unshare-pid", "--die-with-parent",
               "--tmpfs", "/", "--ro-bind", "/usr", "/usr", "--symlink", "usr/lib", "/lib",
               "--symlink", "usr/lib", "/lib64", "--symlink", "usr/bin", "/bin", "--proc",
               "/proc", "--dev", "/dev", "--ro-bind", info.c_str(), "/.flatpak-info",
               "--unsetenv", "LD_PRELOAD", "/usr/bin/sh", "-c", "echo ready; exec sleep 600",
               (char*)nullptr);
        _exit(127);
    }
    close(pipe_fds[1]);
    if (pid < 0) {
        close(pipe_fds[0]);
        return process;
    }
    std::string said;
    for (int waited = 0; waited < 100 && said.find('\n') == std::string::npos; ++waited) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(pipe_fds[0], &set);
        timeval tv{0, 100 * 1000};
        if (select(pipe_fds[0] + 1, &set, nullptr, nullptr, &tv) <= 0) {
            continue;
        }
        char c = 0;
        if (read(pipe_fds[0], &c, 1) != 1) {
            break;
        }
        said.push_back(c);
    }
    close(pipe_fds[0]);
    if (said != "ready\n") {
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        return process;
    }
    process.pid = pid;
    return process;
}

inline void stop_fake_flatpak(FakeFlatpak& process) {
    if (process.pid > 0) {
        kill(process.pid, SIGKILL);  // bwrap's --die-with-parent takes the sandbox with it
        waitpid(process.pid, nullptr, 0);
        process.pid = -1;
    }
}

// The same, stopped when it goes out of scope, for a test with many returns.
struct ScopedFakeFlatpak {
    FakeFlatpak process;
    ScopedFakeFlatpak(const std::string& scratch, const char* app_id)
        : process(start_fake_flatpak(scratch, app_id)) {}
    ~ScopedFakeFlatpak() { stop_fake_flatpak(process); }
    ScopedFakeFlatpak(const ScopedFakeFlatpak&) = delete;
    ScopedFakeFlatpak& operator=(const ScopedFakeFlatpak&) = delete;
    bool running() const { return process.pid > 0; }
};

}  // namespace vocem_test

#endif  // VOCEM_TESTS_FAKE_FLATPAK_H

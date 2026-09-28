// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A stop reaches a daemon that is waiting inside a system call.
//
// vocemd installed its SIGTERM handler with std::signal, which on glibc is BSD
// semantics: SA_RESTART. A handler that only sets a flag then changes nothing
// for a call that blocks -- the kernel restarts the call and the flag is never
// read. The review found the shape with a FIFO planted at an avatar cache name:
// rescan() waited in open() through SIGTERM until the unit's SIGKILL, ten
// seconds later, left the segment published (entry 81's leftover). The bridge's
// opens do not wait any more (avatar_cache_hostile), but every open the daemon
// makes is not the bridge's: a FIFO at config.ini holds the settings reload the
// same way, and so would whatever blocking call comes next.
//
// Measured against the real vocemd, sandboxed like daemon_notification: a FIFO
// where config.ini should be, SIGTERM two seconds after start, and the time to
// exit. With SA_RESTART the daemon was still alive six seconds later; without
// it the open returns EINTR, the flag is read and the daemon exits in ~0.1 s.

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

#include "discord_stub.h"
#include "unit_confinement.h"
#include "probe_alarm.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    const char* daemon_path = getenv("VOCEM_DAEMON");
    if (!daemon_path || !daemon_path[0]) {
        printf("skip VOCEM_DAEMON not set: no daemon binary to drive\n");
        return 77;
    }
    if (const int gate = vocem_test::ensure_daemon_confinement(); gate >= 0) {
        return gate;
    }
    vocem_test::set_alarm(60, "a stop inside a blocked call");

    const std::string base =
        vocem_test::scratch_dir("vocem-stop-blocked", {"/state", "/state/vocem", "/config",
                                                       "/config/vocem", "/cache", "/runtime"});
    if (base.empty()) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    check(mkfifo((base + "/config/vocem/config.ini").c_str(), 0600) == 0,
          "a FIFO stands where config.ini should be");

    const pid_t pid = fork();
    if (pid == 0) {
        setenv("XDG_STATE_HOME", (base + "/state").c_str(), 1);
        setenv("XDG_CONFIG_HOME", (base + "/config").c_str(), 1);
        setenv("XDG_CACHE_HOME", (base + "/cache").c_str(), 1);
        setenv("XDG_RUNTIME_DIR", (base + "/runtime").c_str(), 1);
        const int log = open((base + "/daemon.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        dup2(log, 2);
        execl(daemon_path, daemon_path, nullptr);
        _exit(127);
    }
    sleep(2);
    const double told = vocem_test::monotonic();
    kill(pid, SIGTERM);
    int status = 0;
    double took = -1.0;
    for (int i = 0; i < 120; ++i) {
        if (waitpid(pid, &status, WNOHANG) == pid) {
            took = vocem_test::monotonic() - told;
            break;
        }
        usleep(50 * 1000);
    }
    if (took < 0.0) {
        kill(pid, SIGKILL);
        waitpid(pid, nullptr, 0);
        printf("--  the daemon was still alive 6 s after SIGTERM and was SIGKILLed\n");
    } else {
        printf("--  the daemon exited %.2f s after SIGTERM\n", took);
    }
    check(took >= 0.0 && took <= 2.0, "a SIGTERM ends a daemon blocked in open() within 2 s");
    check(took >= 0.0 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "and it exits cleanly, through its own shutdown");

    vocem_test::remove_tree(base);
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

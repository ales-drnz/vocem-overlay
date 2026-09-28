// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// One daemon per user.
//
// The segment is `/vocem-<uid>`, opened O_CREAT without O_EXCL, and its seqlock
// assumes one writer. Nothing stopped a second vocemd -- one started by hand
// beside the unit, or by the window while the unit was restarting -- from
// opening the same segment and publishing into it under the first one's
// sequence, and when either of them stopped, unlink_segment() took the name the
// other was still publishing under: every game went empty with a daemon
// running.
//
// The daemon now takes an flock on a file beside the segment in /dev/shm before
// it opens anything. In /dev/shm and not in $XDG_RUNTIME_DIR, because /dev/shm
// is what the tests make private: a lock in the runtime directory would make
// every sandboxed test daemon collide with the live one. The second daemon says
// so and exits 0 (the unit's Restart=on-failure does not answer a success).
//
// Held with two real daemons in one private /dev/shm (daemon_notification's
// sandbox).

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
#include "vocem/shm.h"

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what.c_str());
    if (!condition) {
        ++failures;
    }
}

std::string read_file(const std::string& path) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        return {};
    }
    std::string out;
    char buffer[4096];
    size_t got = 0;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        out.append(buffer, got);
    }
    fclose(file);
    return out;
}

bool segment_named() {
    char name[64];
    vocem::shm_name(name, sizeof(name), getuid());
    struct stat info {};
    return stat((std::string("/dev/shm") + name).c_str(), &info) == 0;
}

pid_t start_daemon(const char* daemon_path, const std::string& base, const std::string& log) {
    const pid_t pid = fork();
    if (pid == 0) {
        setenv("XDG_STATE_HOME", (base + "/state").c_str(), 1);
        setenv("XDG_CONFIG_HOME", (base + "/config").c_str(), 1);
        setenv("XDG_CACHE_HOME", (base + "/cache").c_str(), 1);
        setenv("XDG_RUNTIME_DIR", (base + "/runtime").c_str(), 1);
        const int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        dup2(fd, 2);
        execl(daemon_path, daemon_path, nullptr);
        _exit(127);
    }
    return pid;
}

// The exit status within `seconds`, or -1 while it is still running.
int exited_within(pid_t pid, double seconds) {
    for (int i = 0; i < static_cast<int>(seconds * 20); ++i) {
        int status = 0;
        if (waitpid(pid, &status, WNOHANG) == pid) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        }
        usleep(50 * 1000);
    }
    return -1;
}

}  // namespace

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
    vocem_test::set_alarm(60, "one daemon per user");

    char root[] = "/tmp/vocem-single-daemon-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    const std::string base = root;
    for (const char* leaf :
         {"/state", "/state/vocem", "/config", "/config/vocem", "/cache", "/runtime"}) {
        mkdir((base + leaf).c_str(), 0700);
    }

    const pid_t first = start_daemon(daemon_path, base, base + "/first.log");
    for (int i = 0; i < 40 && !segment_named(); ++i) {
        usleep(50 * 1000);
    }
    check(segment_named(), "the first daemon publishes its segment");

    const pid_t second = start_daemon(daemon_path, base, base + "/second.log");
    int code = exited_within(second, 3.0);
    const std::string said = read_file(base + "/second.log");
    printf("--  the second daemon %s\n",
           code < 0 ? "was still running 3 s later" : ("exited " + std::to_string(code)).c_str());
    check(code == 0, "a second daemon for the same user exits by itself, with status 0");
    check(said.find("another vocemd") != std::string::npos, "and says why in its log");
    if (code < 0) {
        // What two writers leave: the second one's stop unlinks the name the
        // first is still publishing under.
        kill(second, SIGTERM);
        exited_within(second, 5.0);
    }
    check(exited_within(first, 0.5) < 0, "the first daemon is still running");
    check(segment_named(), "and its segment is still named for every game to find");

    kill(first, SIGTERM);
    code = exited_within(first, 10.0);
    check(code == 0, "the first daemon stops cleanly when told");
    if (code < 0) {
        kill(first, SIGKILL);
        waitpid(first, nullptr, 0);
    }

    (void)!system(("rm -rf '" + base + "'").c_str());
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

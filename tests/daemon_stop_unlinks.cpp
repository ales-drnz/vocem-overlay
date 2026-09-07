// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// How long the segment stays published after the daemon is told to stop, with
// a download stalled underneath it.
//
// The unit says `TimeoutStopSec=10`, and says why: "a SIGKILL skips the
// unlink". The daemon's own shutdown, through 0.1.7, joined its avatar worker
// FIRST and unlinked the segment after -- and the worker had no way out of a
// transfer already under way. A CDN that stalled, or a proxy that accepted the
// connection and went quiet, held the join for the transfer's whole fifteen
// seconds: SIGTERM at 0, SIGKILL at 10, and `/dev/shm/vocem-<uid>` still there
// with the note segment and every Flatpak mirror beside it, which is the exact
// leftover entry 81 forbids (entry 134). Two changes: the names go first and the
// join comes last, and the transfer is aborted from its progress callback the
// moment stop() is called.
//
// The stall is built rather than described: `https_proxy` is pointed at a
// listener in this process that accepts and never answers, which libcurl
// honours for an https URL, so the daemon's CONNECT waits on its timeout
// exactly as a quiet CDN would make it wait. The seconds are printed and not
// only asserted (entry 103's rule). The daemon's own sandbox is
// daemon_notification's: private /dev/shm, private network.

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

#include "discord_stub.h"
#include "private_shm.h"
#include "vocem/shared_state.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

void write_file(const std::string& path, const char* contents) {
    FILE* file = fopen(path.c_str(), "w");
    if (file) {
        fputs(contents, file);
        fclose(file);
    }
}

bool segment_named() {
    char name[64];
    vocem::shm_name(name, sizeof(name), getuid());
    const int fd = shm_open(name, O_RDONLY, 0);
    if (fd >= 0) {
        close(fd);
        return true;
    }
    return false;
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    signal(SIGPIPE, SIG_IGN);

    const char* daemon_path = getenv("VOCEM_DAEMON");
    if (!daemon_path || !daemon_path[0]) {
        printf("skip VOCEM_DAEMON not set: no daemon binary to drive\n");
        return 77;
    }
    if (const int gate = vocem_test::ensure_private_shm(true); gate >= 0) {
        return gate;
    }
    alarm(120);

    char root[] = "/tmp/vocem-stop-unlinks-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    const std::string base = root;
    for (const char* leaf :
         {"/state", "/state/vocem", "/config", "/config/vocem", "/cache", "/runtime"}) {
        mkdir((base + leaf).c_str(), 0700);
    }
    write_file(base + "/state/vocem/token", "test-token\n");
    write_file(base + "/config/vocem/config.ini", "");

    // The quiet proxy: accepts, and never says a word.
    const int proxy = vocem_test::listen_on(6480);
    // Discord, on its usual port.
    const int discord = vocem_test::listen_on(6463);
    if (proxy < 0 || discord < 0) {
        printf("FAIL cannot listen on loopback -- is the sandbox missing --unshare-net?\n");
        return 1;
    }

    const pid_t daemon_pid = fork();
    if (daemon_pid == 0) {
        setenv("XDG_STATE_HOME", (base + "/state").c_str(), 1);
        setenv("XDG_CONFIG_HOME", (base + "/config").c_str(), 1);
        setenv("XDG_CACHE_HOME", (base + "/cache").c_str(), 1);
        setenv("XDG_RUNTIME_DIR", (base + "/runtime").c_str(), 1);
        setenv("https_proxy", "http://127.0.0.1:6480", 1);
        setenv("HTTPS_PROXY", "http://127.0.0.1:6480", 1);
        execl(daemon_path, daemon_path, nullptr);
        _exit(127);
    }

    const int fd = accept(discord, nullptr, nullptr);
    check(fd >= 0, "the daemon connected to the stub Discord");
    if (fd < 0) {
        kill(daemon_pid, SIGKILL);
        return 1;
    }
    std::string buffer;
    check(vocem_test::accept_upgrade(fd), "and upgraded");
    // A channel with one participant whose picture is not cached: the daemon
    // has to fetch it, and the fetch goes to the quiet proxy.
    const std::string channel =
        R"({"cmd":"GET_SELECTED_VOICE_CHANNEL","evt":null,"nonce":"chan","data":{)"
        R"("id":"1","name":"stalled","voice_states":[{"nick":"Face","user":{"id":"77",)"
        R"("username":"face","avatar":"a71d433becd902959baa0b8e59e9095c"},"voice_state":{}}]}})";
    check(vocem_test::bring_up_session(fd, buffer, channel, vocem_test::monotonic() + 10.0),
          "the session came up and the channel was adopted");
    check(segment_named(), "the segment is published");

    // The download in flight: the proxy sees the daemon's connection, and
    // leaves it hanging.
    const int stalled = accept(proxy, nullptr, nullptr);
    check(stalled >= 0, "the daemon's avatar download reached the proxy, which stays quiet");

    // SIGTERM, and the clock starts.
    const double told = vocem_test::monotonic();
    kill(daemon_pid, SIGTERM);
    double gone_after = -1.0;
    for (int i = 0; i < 300; ++i) {  // 30 s: past the transfer's own 15 s timeout
        usleep(100 * 1000);
        if (!segment_named()) {
            gone_after = vocem_test::monotonic() - told;
            break;
        }
    }
    if (gone_after >= 0.0) {
        printf("--  the segment's name went %.1f s after SIGTERM, with a download stalled\n",
               gone_after);
    } else {
        printf("--  the segment's name was STILL there %.1f s after SIGTERM\n",
               vocem_test::monotonic() - told);
    }
    check(gone_after >= 0.0, "the segment is unlinked on SIGTERM whatever a download is doing");
    // The unit gives the daemon ten seconds; the transfer's own timeout is
    // fifteen. Three is generous over the tick-and-abort the fix makes, and
    // refuses the class of "waited for the transfer".
    check(gone_after >= 0.0 && gone_after <= 3.0,
          "and long before the unit's TimeoutStopSec, not on the transfer's clock");

    int status = 0;
    const pid_t reaped = waitpid(daemon_pid, &status, 0);
    check(reaped == daemon_pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "the daemon exited cleanly");
    if (stalled >= 0) {
        close(stalled);
    }
    close(fd);
    close(proxy);
    close(discord);
    (void)!system(("rm -rf " + base).c_str());

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

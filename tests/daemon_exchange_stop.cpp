// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// How long the daemon takes to stop while it is exchanging an authorisation
// code for a token, with the exchange stalled.
//
// The exchange is the one transfer the daemon makes on its own thread rather
// than on the avatar worker's, and it blocks by design: rpc_client.cpp says so,
// and says what stalls while it does (the bridge, the display re-read). What
// it did not say is that a SIGTERM during it had no way in. Entry 134 gave the
// avatar worker's transfers a way out through their progress callback, because
// a stalled CDN held `systemctl stop` past the unit's ten-second TimeoutStopSec
// and the daemon was killed with its segment published -- and the token
// exchange, one file over, had no such callback. So a stop asked for during a
// stalled exchange waited for libcurl's connect timeout (ten seconds), which is
// the unit's whole budget, with the segment's name still in /dev/shm for all
// of it.
//
// The stall is built the way daemon_stop_unlinks builds it: `https_proxy`
// points at a listener in this process that accepts and never answers. The
// daemon starts with no token, so the stub Discord's READY draws an AUTHORIZE,
// which the stub answers with a code; the daemon then goes to the token
// endpoint through the quiet proxy, and once the proxy has the connection the
// daemon is told to stop. The seconds are printed and not only asserted (entry
// 103's rule). Against the daemon shipped in 0.1.8 the stop takes the connect
// timeout; with the flag consulted from the transfer it takes about a second.
//
// Isolation is daemon_notification's: bwrap, private /dev/shm, private network.

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

    char root[] = "/tmp/vocem-exchange-stop-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    const std::string base = root;
    for (const char* leaf :
         {"/state", "/state/vocem", "/config", "/config/vocem", "/cache", "/runtime"}) {
        mkdir((base + leaf).c_str(), 0700);
    }
    // No token: the daemon asks Discord for authorisation, which is the road
    // the exchange is on.
    write_file(base + "/config/vocem/config.ini", "");

    const int proxy = vocem_test::listen_on(6480);
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
    vocem_test::send_text(fd, R"({"cmd":"DISPATCH","evt":"READY","data":{"v":1},"nonce":null})");

    // With no token the daemon asks to be authorised; the stub hands it a code,
    // and the daemon goes to exchange it -- through the quiet proxy.
    std::string message;
    bool asked = false;
    const double deadline = vocem_test::monotonic() + 10.0;
    while (vocem_test::recv_text(fd, buffer, message, deadline)) {
        if (message.find("\"cmd\":\"AUTHORIZE\"") != std::string::npos) {
            vocem_test::send_text(fd,
                                  R"({"cmd":"AUTHORIZE","evt":null,"nonce":"authorize",)"
                                  R"("data":{"code":"stub-code"}})");
            asked = true;
            break;
        }
    }
    check(asked, "with no token the daemon asked Discord for authorisation");
    check(segment_named(), "the segment is published while it waits");

    const int stalled = accept(proxy, nullptr, nullptr);
    check(stalled >= 0, "the token exchange reached the proxy, which stays quiet");

    // SIGTERM, and the clock starts.
    const double told = vocem_test::monotonic();
    kill(daemon_pid, SIGTERM);
    int status = 0;
    double ended_after = -1.0;
    for (int i = 0; i < 400; ++i) {  // 40 s: past both of the exchange's timeouts
        if (waitpid(daemon_pid, &status, WNOHANG) == daemon_pid) {
            ended_after = vocem_test::monotonic() - told;
            break;
        }
        usleep(100 * 1000);
    }
    if (ended_after >= 0.0) {
        printf("--  the daemon ended %.1f s after SIGTERM, with the token exchange stalled\n",
               ended_after);
    } else {
        printf("--  the daemon was STILL running %.1f s after SIGTERM\n",
               vocem_test::monotonic() - told);
        kill(daemon_pid, SIGKILL);
        waitpid(daemon_pid, &status, 0);
    }
    check(ended_after >= 0.0, "SIGTERM ends the daemon whatever the exchange is doing");
    // The connect timeout is ten seconds and the unit gives the daemon ten;
    // three is generous over the callback's one-second cadence and refuses the
    // class of "waited for the transfer".
    check(ended_after >= 0.0 && ended_after <= 3.0,
          "and long before the unit's TimeoutStopSec, not on the transfer's clock");
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "as a clean exit");
    check(!segment_named(), "with the segment's name unlinked behind it");

    if (stalled >= 0) {
        close(stalled);
    }
    close(fd);
    close(proxy);
    close(discord);
    if (system(("rm -rf " + base).c_str()) != 0) {
        printf("  (could not remove %s)\n", base.c_str());
    }
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

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
//
// Two more doors into the same room, chosen with VOCEM_DAEMON_STOP_SCENARIO,
// because what a stop must leave behind is one question and the ways in are
// several:
//
//   `handshake` -- the stop arrives while the daemon is inside a handshake
//   rather than inside a download. A peer accepts the TCP connection and never
//   answers the upgrade, which the daemon is right to wait for: it has an
//   absolute ten-second deadline for it (entry 77) and honoured it to the
//   letter, with `g_stop` already set and the unit's TimeoutStopSec at ten. The
//   class has been repaired four times in four functions (entries 72, 77, 102,
//   112) and this was the instance left; daemon_ws_bounds cannot reach it,
//   because its stub answers.
//
//   `trickle` -- the same handshake, with a peer that answers it one byte
//   every tenth of a second and never finishes. The stop flag was read only
//   when a 200 ms slice expired empty, and a byte every 100 ms never lets one
//   expire: the handshake's ten seconds again, against the unit's ten, through
//   the door entry 161 had closed for a silent peer (entry 196).
//
//   `note-mirror` -- what a CLEAN stop leaves in a Flatpak sandbox. The daemon
//   unlinked the mirrored copies of the STATE and then emptied its list of
//   mirrors, and only then retired the note -- whose retirement is what unlinks
//   the mirrored words. So the hook iterated an empty list: a message's text
//   stayed in `$XDG_RUNTIME_DIR/app/<id>/vocem/note`, readable by that
//   application until logout, after the tray's Quit. `vocem/note.h` promises
//   the opposite in so many words, and tests/flatpak_bridge.cpp drives the two
//   halves in the order that works and never in this one.

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

#include "probe_alarm.h"
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

bool exists(const std::string& path) {
    struct stat info {};
    return stat(path.c_str(), &info) == 0;
}

// Waits for a predicate, up to `seconds`, at ten looks a second. Returns the
// time it took, or -1.
template <typename Fn>
double wait_for(double seconds, Fn ready) {
    const double started = vocem_test::monotonic();
    for (int i = 0; i < static_cast<int>(seconds * 10); ++i) {
        if (ready()) {
            return vocem_test::monotonic() - started;
        }
        usleep(100 * 1000);
    }
    return -1.0;
}

}  // namespace


namespace {

// The environment every scenario gives the daemon: its own state, config,
// cache and runtime directories under one temporary root.
void make_sandbox(const std::string& base) {
    for (const char* leaf :
         {"/state", "/state/vocem", "/config", "/config/vocem", "/cache", "/runtime"}) {
        mkdir((base + leaf).c_str(), 0700);
    }
    write_file(base + "/state/vocem/token", "test-token\n");
    write_file(base + "/config/vocem/config.ini", "");
}

pid_t start_daemon(const char* daemon_path, const std::string& base, bool quiet_proxy) {
    const pid_t pid = fork();
    if (pid == 0) {
        setenv("XDG_STATE_HOME", (base + "/state").c_str(), 1);
        setenv("XDG_CONFIG_HOME", (base + "/config").c_str(), 1);
        setenv("XDG_CACHE_HOME", (base + "/cache").c_str(), 1);
        setenv("XDG_RUNTIME_DIR", (base + "/runtime").c_str(), 1);
        if (quiet_proxy) {
            setenv("https_proxy", "http://127.0.0.1:6480", 1);
            setenv("HTTPS_PROXY", "http://127.0.0.1:6480", 1);
        }
        execl(daemon_path, daemon_path, nullptr);
        _exit(127);
    }
    return pid;
}

// How long from SIGTERM to the process being reaped, and whether it went
// cleanly. -1 when it had to be killed.
double term_and_reap(pid_t pid, double allow_seconds, int* status_out) {
    const double told = vocem_test::monotonic();
    kill(pid, SIGTERM);
    for (int i = 0; i < static_cast<int>(allow_seconds * 20); ++i) {
        int status = 0;
        const pid_t reaped = waitpid(pid, &status, WNOHANG);
        if (reaped == pid) {
            if (status_out) {
                *status_out = status;
            }
            return vocem_test::monotonic() - told;
        }
        usleep(50 * 1000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
    return -1.0;
}

// -------------------------------------------------------------------------
// The stop arrives with an avatar download stalled underneath it.
int stalled_download(const char* daemon_path, const std::string& base) {
    // The quiet proxy: accepts, and never says a word.
    const int proxy = vocem_test::listen_on(6480);
    // Discord, on its usual port.
    const int discord = vocem_test::listen_on(6463);
    if (proxy < 0 || discord < 0) {
        printf("FAIL cannot listen on loopback -- is the sandbox missing --unshare-net?\n");
        return 1;
    }

    const pid_t daemon_pid = start_daemon(daemon_path, base, true);

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
    return 0;
}

// -------------------------------------------------------------------------
// The stop arrives while the daemon is inside a handshake, from a peer that
// says nothing or -- `trickle` -- one byte of an upgrade that never ends.
int handshake_stop(const char* daemon_path, const std::string& base, bool trickle) {
    // Accepts the connection and answers nothing: the shape of a client still
    // starting, or of anything else that binds the port. The daemon is right to
    // wait -- what it may not do is wait through its own stop.
    const int silent = vocem_test::listen_on(6463);
    if (silent < 0) {
        printf("FAIL cannot listen on loopback -- is the sandbox missing --unshare-net?\n");
        return 1;
    }

    const pid_t daemon_pid = start_daemon(daemon_path, base, false);
    const int fd = accept(silent, nullptr, nullptr);
    check(fd >= 0, "the daemon connected to a peer that never answers the upgrade");
    if (fd < 0) {
        kill(daemon_pid, SIGKILL);
        waitpid(daemon_pid, nullptr, 0);
        return 1;
    }
    // Far enough into the handshake for the request to be written and the read
    // to be waiting. The deadline it is waiting on is ten seconds.
    usleep(500 * 1000);
    pid_t trickler = -1;
    if (trickle) {
        // A child keeps the socket readable: the start of a 101, then a header
        // that never ends, a byte every 100 ms -- under the 200 ms slice.
        trickler = fork();
        if (trickler == 0) {
            static const char kStart[] = "HTTP/1.1 101 Switching Protocols\r\nX-Slow: ";
            if (write(fd, kStart, sizeof(kStart) - 1) < 0) {
                _exit(0);
            }
            for (int i = 0; i < 300; ++i) {
                usleep(100 * 1000);
                if (write(fd, "a", 1) != 1) {
                    break;
                }
            }
            _exit(0);
        }
        usleep(300 * 1000);  // a few bytes in, so the read is taking them
    }

    int status = 0;
    const double took = term_and_reap(daemon_pid, 20.0, &status);
    if (trickler > 0) {
        kill(trickler, SIGKILL);
        waitpid(trickler, nullptr, 0);
    }
    if (took >= 0.0) {
        printf("--  the daemon exited %.1f s after SIGTERM, from inside the handshake\n", took);
    } else {
        printf("--  the daemon had to be SIGKILLed: it never exited\n");
    }
    check(took >= 0.0, "a stop reaches the daemon while it waits on a silent peer");
    // The handshake deadline is 10 s and the unit's TimeoutStopSec is 10, so
    // anything near ten is the defect: the stop was honoured after the wait
    // rather than during it. Two seconds is generous over the 200 ms slice.
    check(took >= 0.0 && took <= 2.0,
          "and reaches it during the wait, not after the handshake's own deadline");
    check(took >= 0.0 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "and the daemon exits cleanly rather than being killed");

    close(fd);
    close(silent);
    return 0;
}

// -------------------------------------------------------------------------
// What a clean stop leaves in a Flatpak sandbox.
int note_mirror(const char* daemon_path, const std::string& base) {
    // A sandbox asking to be served, and drawing. This is the file the injected
    // code writes from inside the game (vocem/flatpak.h).
    const std::string app = base + "/runtime/app/org.test.game";
    mkdir((base + "/runtime/app").c_str(), 0700);
    mkdir(app.c_str(), 0700);
    mkdir((app + "/vocem").c_str(), 0700);
    write_file(app + "/vocem/request", "pid=1\ndrawing=1\n");
    const std::string mirror_state = app + "/vocem/state";
    const std::string mirror_note = app + "/vocem/note";

    const int discord = vocem_test::listen_on(6463);
    if (discord < 0) {
        printf("FAIL cannot listen on loopback -- is the sandbox missing --unshare-net?\n");
        return 1;
    }
    const pid_t daemon_pid = start_daemon(daemon_path, base, false);

    const int fd = accept(discord, nullptr, nullptr);
    check(fd >= 0, "the daemon connected to the stub Discord");
    if (fd < 0) {
        kill(daemon_pid, SIGKILL);
        waitpid(daemon_pid, nullptr, 0);
        return 1;
    }
    std::string buffer;
    check(vocem_test::accept_upgrade(fd), "and upgraded");
    check(vocem_test::bring_up_session(
              fd, buffer, R"({"cmd":"GET_SELECTED_VOICE_CHANNEL","evt":null,"nonce":"chan",)"
                          R"("data":null})",
              vocem_test::monotonic() + 10.0),
          "the session came up");

    // Adoption is on the bridge's own one-second sweep.
    const double adopted = wait_for(10.0, [&] { return exists(mirror_state); });
    check(adopted >= 0.0, "the sandbox was adopted and its state mirror appeared");

    vocem_test::send_text(fd,
                          R"({"cmd":"DISPATCH","evt":"NOTIFICATION_CREATE","nonce":null,"data":{)"
                          R"("title":"Someone","body":"THE-SECRET-TEXT",)"
                          R"("message":{"author":{"id":"42","avatar":"","global_name":"Someone"}}}})");
    const double mirrored = wait_for(10.0, [&] { return exists(mirror_note); });
    check(mirrored >= 0.0, "and the message's words were mirrored into it");
    if (mirrored < 0.0) {
        kill(daemon_pid, SIGKILL);
        waitpid(daemon_pid, nullptr, 0);
        close(fd);
        close(discord);
        return 1;
    }

    // A clean stop: the tray's Quit does exactly this.
    int status = 0;
    const double took = term_and_reap(daemon_pid, 20.0, &status);
    check(took >= 0.0 && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "the daemon stopped cleanly");

    const bool note_left = exists(mirror_note);
    const bool state_left = exists(mirror_state);
    printf("--  after a clean stop the sandbox holds: note %s, state %s\n",
           note_left ? "STILL THERE" : "gone", state_left ? "STILL THERE" : "gone");
    check(!note_left,
          "the message's words are gone from the sandbox: nothing is left to read after Quit");
    check(!state_left, "and so is the mirrored state (entry 81: a leftover mirror is forever)");
    check(!segment_named(), "and the host segment's name is gone too");

    close(fd);
    close(discord);
    return 0;
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
    vocem_test::set_alarm(120, "the daemon unlinking on a stop");

    const char* env_scenario = getenv("VOCEM_DAEMON_STOP_SCENARIO");
    const std::string scenario = env_scenario ? env_scenario : "";

    char root[] = "/tmp/vocem-stop-unlinks-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    const std::string base = root;
    make_sandbox(base);

    int result = 0;
    if (scenario == "handshake") {
        printf("--  scenario: a stop arriving inside the handshake\n");
        result = handshake_stop(daemon_path, base, false);
    } else if (scenario == "trickle") {
        printf("--  scenario: a stop arriving inside a handshake answered a byte at a time\n");
        result = handshake_stop(daemon_path, base, true);
    } else if (scenario == "note-mirror") {
        printf("--  scenario: what a clean stop leaves in a Flatpak sandbox\n");
        result = note_mirror(daemon_path, base);
    } else if (scenario.empty()) {
        printf("--  scenario: a stop arriving with a download stalled\n");
        result = stalled_download(daemon_path, base);
    } else {
        printf("FAIL unknown VOCEM_DAEMON_STOP_SCENARIO '%s'\n", scenario.c_str());
        result = 1;
    }

    (void)!system(("rm -rf " + base).c_str());
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return (failures == 0 && result == 0) ? 0 : 1;
}

// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Whose clock the words of a message are retired on.
//
// `vocem/note.h` promises, in its own words, that the note segment "exists only
// while a toast is on screen ... so between messages there is nothing to read
// anywhere", and that the exposure is "the process that is drawing it, while it
// is drawing it". Both sentences are about time, and both were false in the
// ordinary case, because the tick that retires the note was reached from one
// place only: the recv loop of a LIVE Discord connection. Every other loop in
// the daemon -- the port walk, the reconnect pause, the wait after a refused
// authorisation -- serviced the Flatpak bridge and nothing else.
//
// So the exposure was not "while it is drawing it". A direct message arrives,
// the toast is drawn and fades, and Discord then quits (or crashes, or the RPC
// drops): the daemon leaves the recv loop and never runs the tick again. The
// words stay mapped and the name `/dev/shm/vocem-note-<uid>` stays openable by
// every process in the session, for as long as Discord is away. Measured
// against the packaged 0.1.4-1 daemon and against the build tree's own, three
// runs, identical: the text was still there forty seconds after the connection
// closed, which is only where the counting stopped -- nothing in that daemon
// ever ended it.
//
// Two cases here, one for each way the words outlived their toast:
//
//   1. **Discord leaves and the daemon lives.** The tick belongs on the
//      daemon's own clock, so the note goes whatever Discord is doing. Against
//      the defective daemon this case does not fail slowly, it fails at the
//      full length of its wait.
//   2. **The daemon is killed outright.** A SIGKILL skips `~NoteWriter()`, and
//      the unit carries `Restart=on-failure` with `MemoryMax=128M`, so this is
//      an ordinary Tuesday and not a hypothesis. Nothing can be done inside the
//      process that is being killed; what the next daemon can do is refuse to
//      inherit it. A note found at startup was written by a daemon that is gone.
//
// The seconds are printed and not only asserted (entry 103's rule: the number
// is the measurement, and a threshold is only where somebody put it).
//
// Not covered here, and said rather than implied: the Flatpak mirror of the
// note. It is removed by the same `on_publish` hook that `NoteWriter::clear()`
// fires, which `tests/flatpak_bridge.cpp` already drives through both real
// halves -- so it follows this fix rather than needing one of its own, and no
// second sandbox is built to watch it do so.
//
// Isolation is daemon_notification's: re-exec under bwrap with a private
// /dev/shm -- the real daemon's segment is never touched -- and a private
// network namespace, so 6463 is free even while Discord is running. Where bwrap
// is missing it reports itself skipped rather than passing without having run.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <string>

#include "discord_stub.h"
#include "unit_confinement.h"
#include "vocem/note.h"
#include "vocem/shm.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

// The stub Discord is tests/discord_stub.h's; this file carried a copy.
using vocem_test::monotonic;
using vocem_test::recv_text;
using vocem_test::send_text;

// ---------------------------------------------------------------------------

void write_file(const std::string& path, const char* contents) {
    FILE* file = fopen(path.c_str(), "w");
    if (file) {
        fputs(contents, file);
        fclose(file);
    }
}

template <typename Predicate>
bool wait_for(vocem::StateReader& reader, vocem::Snapshot& snapshot, double seconds,
              Predicate predicate) {
    const double deadline = monotonic() + seconds;
    for (;;) {
        if (reader.valid() || reader.open()) {
            if (reader.read(snapshot) && predicate(snapshot)) {
                return true;
            }
        }
        if (monotonic() >= deadline) {
            return false;
        }
        usleep(100 * 1000);
    }
}

std::string g_base;
const char* g_daemon = nullptr;

// Starts a daemon against the scratch world built below. The settings file is
// written before the first one starts, so the daemon has the toast's length
// from its very first tick rather than from a reparse.
pid_t start_daemon() {
    const pid_t pid = fork();
    if (pid == 0) {
        setenv("XDG_STATE_HOME", (g_base + "/state").c_str(), 1);
        setenv("XDG_CONFIG_HOME", (g_base + "/config").c_str(), 1);
        setenv("XDG_CACHE_HOME", (g_base + "/cache").c_str(), 1);
        setenv("XDG_RUNTIME_DIR", (g_base + "/runtime").c_str(), 1);
        execl(g_daemon, g_daemon, nullptr);
        _exit(127);
    }
    return pid;
}

// True while the words are still readable out of the note segment, by the same
// three syscalls a game uses.
bool note_still_readable(uint64_t serial) {
    vocem::NoteReader reader;
    return reader.body_for(serial)[0] != '\0';
}

bool note_name_exists() {
    char name[64];
    vocem::note_shm_name(name, sizeof(name), getuid());
    const int fd = shm_open(name, O_RDONLY, 0);
    if (fd >= 0) {
        close(fd);
        return true;
    }
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    // The alarm below kills the process outright, and a block-buffered stdout
    // dies with everything still in it: a run that says nothing at all is
    // indistinguishable from a run that never started.
    setvbuf(stdout, nullptr, _IONBF, 0);
    // The daemon reconnects once a second while this test is deliberately not
    // answering, so writing to a peer that has already given up is ordinary
    // here. It must be an error to check, not a signal that ends the run.
    signal(SIGPIPE, SIG_IGN);

    const char* daemon_path = getenv("VOCEM_DAEMON");
    if (!daemon_path || !daemon_path[0]) {
        printf("skip VOCEM_DAEMON not set: no daemon binary to drive\n");
        return 77;
    }

    if (const int gate = vocem_test::ensure_daemon_confinement(); gate >= 0) {
        return gate;
    }

    alarm(200);  // generous: against the defective daemon both waits run to the end

    g_daemon = daemon_path;

    // The one scenario flag this file has: `port-walk` replaces "Discord is
    // gone, and nothing answers" with "Discord is gone, and three ports accept
    // and say nothing", which is what makes the port walk SLOW -- ten seconds
    // of handshake deadline each. The daemon's tick, which is what retires
    // these words, sat after the walk's closing brace while its own comment and
    // DESIGN 112 both said it ran once per port: so with this shape the words
    // stayed for the whole walk. The measurement is case 1's own, unchanged and
    // with its own bound; only what is listening changes. It stops after case 1,
    // because the cases after it need the ports.
    const char* env_scenario = getenv("VOCEM_NOTE_EXPIRY_SCENARIO");
    const std::string scenario = env_scenario ? env_scenario : "";
    const bool port_walk = scenario == "port-walk";
    if (!scenario.empty() && !port_walk) {
        printf("FAIL unknown VOCEM_NOTE_EXPIRY_SCENARIO '%s'\n", scenario.c_str());
        return 1;
    }

    char root[] = "/tmp/vocem-note-expiry-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    g_base = root;
    for (const char* leaf :
         {"/state", "/state/vocem", "/config", "/config/vocem", "/cache", "/runtime"}) {
        mkdir((g_base + leaf).c_str(), 0700);
    }
    write_file(g_base + "/state/vocem/token", "test-token\n");
    // Before any daemon starts: a toast of one second, over which the daemon
    // allows itself one second of margin, so two seconds is the whole life of
    // these words and everything after that is the defect.
    write_file(g_base + "/config/vocem/config.ini", "notification_seconds = 1.0\n");

    // SOCK_CLOEXEC, inside listen_on(): the case below says what a listener
    // inherited across the daemon's exec cost the first version of this file.
    int listener = vocem_test::listen_on(6463);
    if (listener < 0) {
        printf("FAIL cannot listen on 6463 -- is the sandbox missing --unshare-net?\n");
        return 1;
    }

    pid_t daemon_pid = start_daemon();

    const int fd = accept(listener, nullptr, nullptr);
    check(fd >= 0, "the daemon connected to the stub Discord");
    if (fd < 0) {
        kill(daemon_pid, SIGKILL);
        return 1;
    }

    std::string buffer;
    check(vocem_test::accept_upgrade(fd), "and upgraded");
    send_text(fd, R"({"cmd":"DISPATCH","evt":"READY","data":{"v":1},"nonce":null})");

    std::string message;
    bool authenticated = false;
    const double handshake_deadline = monotonic() + 10.0;
    while (recv_text(fd, buffer, message, handshake_deadline)) {
        if (message.find("\"cmd\":\"AUTHENTICATE\"") != std::string::npos) {
            send_text(fd,
                      R"({"cmd":"AUTHENTICATE","evt":null,"nonce":"auth",)"
                      R"("data":{"user":{"id":"99","username":"stub"}}})");
            authenticated = true;
        } else if (message.find("\"cmd\":\"GET_SELECTED_VOICE_CHANNEL\"") != std::string::npos) {
            send_text(fd,
                      R"({"cmd":"GET_SELECTED_VOICE_CHANNEL","evt":null,"nonce":"chan",)"
                      R"("data":null})");
            break;  // the daemon asks this last; the connection is now idle
        }
    }
    check(authenticated, "and authenticated with the stored token");

    send_text(fd,
              R"({"cmd":"DISPATCH","evt":"NOTIFICATION_CREATE","nonce":null,"data":{)"
              R"("title":"Someone","body":"THE-SECRET-TEXT",)"
              R"("message":{"author":{"id":"42","avatar":"","global_name":"Someone"}}}})");

    vocem::StateReader reader;
    vocem::Snapshot snapshot{};
    const bool arrived = wait_for(reader, snapshot, 8.0, [](const vocem::Snapshot& s) {
        return s.notification.serial == 1;
    });
    check(arrived, "the notification reached the segment");
    const uint64_t serial = snapshot.notification.serial;
    check(note_still_readable(serial), "and the note segment carries the text");

    // -----------------------------------------------------------------------
    // Case 1: Discord leaves and the daemon lives.
    //
    // Nothing else changes -- no signal, no settings edit, no second message.
    // The toast has two seconds to live and the daemon is the only thing that
    // knows it.
    // -----------------------------------------------------------------------
    // Discord quitting means the port stops answering, so the listener goes
    // with the connection.
    //
    // Both of these have to be the only copies, which is why the listener is
    // created `SOCK_CLOEXEC` above: `start_daemon()` forks after it exists, a
    // plain socket survives `execl`, and the daemon therefore inherited the
    // stub's listening socket and held 6463 open for its own reconnect to find.
    // The words then went at 10.1 s rather than the expected two, because the
    // daemon spent a whole 10 s handshake deadline (`kHandshakeTimeoutMs`)
    // talking to a socket it was keeping alive itself -- and the run still
    // reported "Discord is gone", which it was not. The check would have passed
    // on a number that measured the deadline and not the expiry.
    close(listener);
    close(fd);
    // Three ports that accept and never answer, planted before the daemon can
    // look. The daemon is right to wait on each -- it has a ten-second absolute
    // deadline for a handshake -- so this is thirty seconds of walking, and the
    // question is whether anything ticks inside it.
    int silent[3] = {-1, -1, -1};
    if (port_walk) {
        for (int i = 0; i < 3; ++i) {
            silent[i] = vocem_test::listen_on(static_cast<uint16_t>(6463 + i));
            if (silent[i] < 0) {
                printf("FAIL cannot listen on %d\n", 6463 + i);
                kill(daemon_pid, SIGKILL);
                return 1;
            }
        }
        printf("--  and three ports accept without answering: thirty seconds of port walk\n");
    }
    printf("--  Discord is gone; the toast lasts 1.0 s over which the daemon allows 1.0 s\n");

    const double closed_at = monotonic();
    double cleared_after = -1.0;
    // 15 s for the ordinary case (against a fix that needs about two), 45 for
    // the port walk, whose defect takes the whole 30 s walk plus the tick.
    const int looks = port_walk ? 450 : 150;
    for (int i = 0; i < looks; ++i) {
        usleep(100 * 1000);
        if (!note_still_readable(serial)) {
            cleared_after = monotonic() - closed_at;
            break;
        }
    }
    if (cleared_after >= 0.0) {
        printf("--  the words went %.1f s after the connection closed\n", cleared_after);
    } else {
        printf("--  the words were STILL readable %.1f s after the connection closed\n",
               monotonic() - closed_at);
    }
    check(cleared_after >= 0.0,
          "the words are retired with Discord gone: the clock is the daemon's own");
    // An upper bound as well as existence. With nothing listening the budget
    // printed above is about two seconds (the toast's second plus the
    // allowance); the first version of this file measured 10.1 s and nearly
    // believed it -- the handshake deadline spent talking to the stub's own
    // leaked listener -- and a check reading "cleared within the 15 s window"
    // would have passed on that number too. Six seconds is generous over the
    // budget and still refuses the deadline class.
    //
    // Under `port-walk` the bound is a different claim, and the honest one:
    // three ports that accept and stay silent are thirty seconds of walking,
    // and a tick per port bounds the wait to ONE of those deadlines rather than
    // all three. Measured: **11.0 s** with the tick inside the loop against
    // **31.0 s** with it after the loop, which is what the code did while its
    // own comment and DESIGN 112 said otherwise. Fifteen is one deadline plus
    // margin; it is deliberately not two seconds, because nothing here makes a
    // silent peer answer sooner and a bound that pretended otherwise would be
    // measuring a wish.
    const double budget = port_walk ? 15.0 : 6.0;
    check(cleared_after >= 0.0 && cleared_after <= budget,
          port_walk ? "and they go within one handshake deadline, not after the whole walk"
                    : "and they go on the toast's own budget, not on some deadline's");
    check(!note_name_exists(),
          "and the name is unlinked too, so nothing in the session can open it");

    // And the sender goes with the words. vocem/note.h promises that "between
    // messages there is nothing to read anywhere"; the body had its own segment
    // and everything around it -- the serial, the timestamp, the avatar hash
    // and the TITLE, which is Discord's composed string of the sender's display
    // name plus the guild and the channel -- stayed in SharedState, which every
    // graphical process of the session maps as a matter of course. Nothing
    // zeroed the slot on expiry. Read here, through the same reader a game
    // uses, after the words have already gone.
    vocem::Snapshot retired{};
    const bool slot_cleared = wait_for(reader, retired, 6.0, [](const vocem::Snapshot& state) {
        return state.notification.serial == 0;
    });
    if (!slot_cleared) {
        printf("--  the retired toast still reads: serial %llu, title '%s', hash '%s'\n",
               static_cast<unsigned long long>(retired.notification.serial),
               retired.notification.title, retired.notification.avatar_hash);
    }
    check(slot_cleared, "the state segment's notification slot is retired with the words");
    check(slot_cleared && retired.notification.title[0] == '\0',
          "so who sent it is not left in the segment every game maps");
    check(slot_cleared && retired.notification.received == 0.0 &&
              retired.notification.user_id == 0 && retired.notification.avatar_hash[0] == '\0',
          "and neither is when, from whom, or which face");

    if (port_walk) {
        // The claim is about the walk and the cases below need the ports.
        kill(daemon_pid, SIGTERM);
        int status = 0;
        waitpid(daemon_pid, &status, 0);
        check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
              "and the daemon still stops cleanly from inside that walk");
        for (int one : silent) {
            if (one >= 0) {
                close(one);
            }
        }
        if (system(("rm -rf " + g_base).c_str()) != 0) {
            printf("  (could not remove %s)\n", g_base.c_str());
        }
        printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
        return failures == 0 ? 0 : 1;
    }

    // -----------------------------------------------------------------------
    // Case 2: the daemon is killed outright.
    //
    // A SIGKILL skips `~NoteWriter()`, so the words are left behind by
    // construction and nothing inside the dying process can help. What the
    // NEXT daemon can do is decline to inherit them: a note present at startup
    // belongs to a daemon that is gone, and its toast went with it.
    // -----------------------------------------------------------------------
    // Discord comes back: the port answers again. The daemon has been walking
    // 6463-6472 on a growing backoff throughout case 1, so its next attempt may
    // be up to half a minute away, and an attempt that lands while this is
    // still setting up is abandoned by its own end -- which is why this retries
    // rather than trusting one accept, and why SIGPIPE is ignored above.
    listener = vocem_test::listen_on(6463);
    if (listener < 0) {
        printf("FAIL cannot listen on 6463 again (%s)\n", strerror(errno));
        kill(daemon_pid, SIGKILL);
        return 1;
    }
    int second_fd = -1;
    for (int attempt = 0; attempt < 20 && second_fd < 0; ++attempt) {
        const int candidate = accept(listener, nullptr, nullptr);
        if (candidate < 0) {
            continue;
        }
        buffer.clear();
        if (!vocem_test::accept_upgrade(candidate) ||
            !send_text(candidate,
                       R"({"cmd":"DISPATCH","evt":"READY","data":{"v":1},"nonce":null})")) {
            close(candidate);
            continue;
        }
        const double second_deadline = monotonic() + 10.0;
        bool ready = false;
        while (recv_text(candidate, buffer, message, second_deadline)) {
            if (message.find("\"cmd\":\"AUTHENTICATE\"") != std::string::npos) {
                send_text(candidate,
                          R"({"cmd":"AUTHENTICATE","evt":null,"nonce":"auth",)"
                          R"("data":{"user":{"id":"99","username":"stub"}}})");
            } else if (message.find("\"cmd\":\"GET_SELECTED_VOICE_CHANNEL\"") !=
                       std::string::npos) {
                send_text(candidate,
                          R"({"cmd":"GET_SELECTED_VOICE_CHANNEL","evt":null,"nonce":"chan",)"
                          R"("data":null})");
                ready = true;
                break;
            }
        }
        if (ready) {
            second_fd = candidate;
        } else {
            close(candidate);
        }
    }
    if (second_fd < 0) {
        printf("FAIL the daemon never reconnected\n");
        kill(daemon_pid, SIGKILL);
        return 1;
    }
    send_text(second_fd,
              R"({"cmd":"DISPATCH","evt":"NOTIFICATION_CREATE","nonce":null,"data":{)"
              R"("title":"Someone","body":"KILLED-DAEMONS-SECRET",)"
              R"("message":{"author":{"id":"42","avatar":"","global_name":"Someone"}}}})");
    vocem::Snapshot second{};
    const bool second_arrived = wait_for(reader, second, 8.0, [serial](const vocem::Snapshot& s) {
        return s.notification.serial > serial;
    });
    check(second_arrived, "a second message reaches a daemon that is about to be killed");
    const uint64_t second_serial = second.notification.serial;
    check(note_still_readable(second_serial), "and its words are in the note segment");

    kill(daemon_pid, SIGKILL);
    waitpid(daemon_pid, nullptr, 0);
    close(second_fd);
    // The words are still there, and that is the operating system rather than a
    // defect: a killed process runs no destructor. Asserted so the case is
    // pinned as what it is, and so the check below cannot pass because the
    // first daemon happened to tidy up after all.
    check(note_still_readable(second_serial),
          "a SIGKILLed daemon leaves the words behind: no destructor runs");

    daemon_pid = start_daemon();
    bool inherited_gone = false;
    for (int i = 0; i < 100 && !inherited_gone; ++i) {  // 10 s
        usleep(100 * 1000);
        inherited_gone = !note_still_readable(second_serial) && !note_name_exists();
    }
    check(inherited_gone,
          "and the next daemon refuses to inherit them: a note at startup has no toast");

    kill(daemon_pid, SIGTERM);
    int status = 0;
    waitpid(daemon_pid, &status, 0);
    close(listener);
    system(("rm -rf " + g_base).c_str());

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

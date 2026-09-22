// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What a message becomes by the time a game can read it.
//
// The shared segment is mapped into every process the overlay attaches to --
// proprietary game code and, under Proton, anti-cheat -- and it used to carry
// the full text of every direct message. The first answer was a switch,
// defaulting to off, which meant the feature was off; the owner's answer is
// that the message must ALWAYS be drawn and must never be lying around in a
// game's memory. Those two are only compatible if the words travel somewhere
// else: they have a segment of their own now (vocem/note.h), created when a
// message arrives, opened only by a process about to draw that very toast,
// and removed once the toast has outlived its seconds.
//
// Held here end to end, against the real vocemd binary: a stub Discord RPC --
// a WebSocket server on the port Discord uses -- walks the daemon through
// READY, AUTHENTICATE and a NOTIFICATION_CREATE carrying a known body. Three
// claims, in order:
//
//   1. the state segment every process maps NEVER carries the text -- not
//      with a settings file, not without one, not ever;
//   2. the note segment does carry it, for the toast the serial names;
//   3. once the toast has outlived its seconds the note is gone -- scrubbed
//      and unlinked -- so between messages there is nothing to read anywhere.
//
// Claim 1 fails against the daemon shipped in 0.1.0-43, which published the
// body unconditionally, and claims 2 and 3 fail against every daemon up to
// 0.1.0-61, which had no note segment at all.
//
// Isolation: the test re-executes itself under bwrap with a private /dev/shm --
// so the real daemon's segment is never touched -- and a private network
// namespace, so port 6463 is free even while Discord is running. Where bwrap is
// missing it reports itself skipped rather than passing without having run.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <string>

#include "discord_stub.h"
#include "private_shm.h"
#include "vocem/note.h"
#include "vocem/shm.h"

namespace {

// The stub Discord is tests/discord_stub.h's (the fifteen-copies lesson of
// entry 124: this file carried its own copy until 0.1.8).
using vocem_test::monotonic;
using vocem_test::recv_text;
using vocem_test::send_text;

int failures = 0;

void check(bool condition, const char* what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

// ---------------------------------------------------------------------------

void write_file(const std::string& path, const char* contents) {
    FILE* file = fopen(path.c_str(), "w");
    if (file) {
        fputs(contents, file);
        fclose(file);
    }
}

// Reads the segment the way a game does, until the predicate holds or time runs
// out. Returns the last snapshot either way.
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

}  // namespace

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;

    const char* daemon_path = getenv("VOCEM_DAEMON");
    if (!daemon_path || !daemon_path[0]) {
        printf("skip VOCEM_DAEMON not set: no daemon binary to drive\n");
        return 77;
    }

    // Re-execute under bwrap: private /dev/shm so the real daemon's segment is
    // never touched, private network so 6463 is free while Discord runs.
    if (const int gate = vocem_test::ensure_private_shm(true); gate >= 0) {
        return gate;
    }

    alarm(90);  // a stuck handshake must fail, not hang the suite

    // A world for the daemon: a token so it authenticates instead of asking, and
    // no settings file, which is every fresh install.
    char root[] = "/tmp/vocem-daemon-test-XXXXXX";
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
    // A one-second toast, from the start. It was written after the message
    // arrived, which put the daemon's settings re-read (every two seconds,
    // vocem/live_config.h) in front of the expiry being waited for; read at
    // startup it costs nothing, and nothing before step 3 waits on the toast's
    // length -- steps 1 and 2 read the segments the moment the message lands,
    // with a second of toast and a second of margin still to run (DESIGN 193).
    write_file(base + "/config/vocem/config.ini", "notification_seconds = 1.0\n");

    // The stub Discord: listening before the daemon starts, so its first
    // connection attempt is the one that lands.
    const int listener = vocem_test::listen_on(6463);
    if (listener < 0) {
        printf("FAIL cannot listen on 6463 -- is the sandbox missing --unshare-net?\n");
        return 1;
    }

    const pid_t daemon_pid = fork();
    if (daemon_pid == 0) {
        setenv("XDG_STATE_HOME", (base + "/state").c_str(), 1);
        setenv("XDG_CONFIG_HOME", (base + "/config").c_str(), 1);
        setenv("XDG_CACHE_HOME", (base + "/cache").c_str(), 1);
        setenv("XDG_RUNTIME_DIR", (base + "/runtime").c_str(), 1);
        execl(daemon_path, daemon_path, nullptr);
        _exit(127);
    }

    const int fd = accept(listener, nullptr, nullptr);
    check(fd >= 0, "the daemon connected to the stub Discord");
    if (fd < 0) {
        kill(daemon_pid, SIGKILL);
        return 1;
    }

    // The HTTP upgrade. The client checks the status line and nothing else.
    std::string buffer;
    check(vocem_test::accept_upgrade(fd), "and upgraded");

    send_text(fd, R"({"cmd":"DISPATCH","evt":"READY","data":{"v":1},"nonce":null})");

    // AUTHENTICATE arrives; answer it, then swallow the subscriptions and answer
    // the channel query with "not in one".
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

    // Before the message this file is named for: a frame the daemon cannot
    // hold. `kMaxMessageBytes` is 8 MiB (entry 72's reassembly cap), and
    // `json::parse(raw, nullptr, false)` bounds parse ERRORS and nothing else
    // -- an allocation that fails comes back as std::bad_alloc, which used to
    // reach main() and end the process by abort(). Measured under the unit's
    // own MemoryMax of 128 MiB with RLIMIT_AS standing in for it: 8 MiB of '['
    // aborts, 8 MiB of string parses in 47 MB. `Restart=on-failure` then
    // brings the daemon back, and an abort runs no destructor -- so each turn
    // of that loop leaves the segment, the note's words and every Flatpak
    // mirror behind, which is the leftover entry 81 forbids.
    //
    // The claim is only that the daemon is STILL THERE afterwards: everything
    // below this line is the ordinary test, and it can only run against a
    // daemon that survived.
    {
        // What it costs is the measurement, not whether it survives: this
        // machine has the memory to parse 624 MB, so a daemon with no cgroup
        // around it comes through either way -- and the owner's has
        // MemoryMax=128M, where the same allocation is a SIGKILL. So the
        // probe reads the daemon's own high-water mark on both sides of the
        // message.
        auto peak_kb = [&]() -> long {
            char path[64];
            snprintf(path, sizeof(path), "/proc/%d/status", static_cast<int>(daemon_pid));
            FILE* status = fopen(path, "r");
            if (!status) {
                return -1;
            }
            char line[256];
            long value = -1;
            while (fgets(line, sizeof(line), status)) {
                if (sscanf(line, "VmHWM: %ld kB", &value) == 1) {
                    break;
                }
            }
            fclose(status);
            return value;
        };
        const long before = peak_kb();
        const std::string hostile(8u * 1024 * 1024 - 64, '[');
        send_text(fd, hostile);
        // Read, refused and dropped: the daemon answers its socket in order on
        // one thread, so an ordinary message sent after the hostile one is
        // handled after it, and the moment it reaches the segment the hostile
        // one is behind the daemon -- whatever it cost is in VmHWM. It was a
        // fixed 1.5 s, which is a guess at how long a parse takes; the parse
        // this refuses took longer than that when it was not refused.
        send_text(fd,
                  R"({"cmd":"DISPATCH","evt":"NOTIFICATION_CREATE","nonce":null,"data":{)"
                  R"("title":"Before","body":"after-the-hostile-one",)"
                  R"("message":{"author":{"id":"41","avatar":"","global_name":"Before"}}}})");
        {
            vocem::StateReader behind;
            vocem::Snapshot seen{};
            wait_for(behind, seen, 8.0, [](const vocem::Snapshot& s) {
                return s.notification.serial >= 1;
            });
        }
        const bool alive = kill(daemon_pid, 0) == 0 && waitpid(daemon_pid, nullptr, WNOHANG) == 0;
        const long after = peak_kb();
        printf("--  8 MiB of '[': the daemon's peak went %ld kB -> %ld kB\n", before, after);
        check(alive, "a message 8 MiB deep does not take the daemon down with it");
        if (!alive) {
            printf("--  the daemon is gone: 8 MiB of '[' ended it\n");
            close(fd);
            return 1;
        }
        // Parsing it costs 624 MB, measured twice, against a unit whose
        // MemoryMax is 128. Fifty is far above the few hundred kB a refusal
        // moves and far below anything the parse could do.
        check(before > 0 && after > 0 && after - before < 50 * 1024,
              "and is refused before it is parsed, so it costs no memory worth naming");
    }

    // The message under test, exactly once -- the second of the run, since the
    // first only marked the hostile frame as consumed. Nothing below sends
    // another.
    send_text(fd,
              R"({"cmd":"DISPATCH","evt":"NOTIFICATION_CREATE","nonce":null,"data":{)"
              R"("title":"Someone","body":"THE-SECRET-TEXT",)"
              R"("message":{"author":{"id":"42","avatar":"","global_name":"Someone"}}}})");

    vocem::StateReader reader;
    vocem::Snapshot snapshot{};

    // 1. The state segment carries who wrote, never what. The negative has to
    // be read *after* the notification demonstrably arrived, or an empty body
    // proves only that nothing was published yet.
    const bool arrived = wait_for(reader, snapshot, 8.0, [](const vocem::Snapshot& s) {
        return s.notification.serial == 2;
    });
    check(arrived, "the notification reached the segment");
    check(strcmp(snapshot.notification.title, "Someone") == 0, "with the author as its title");
    check(snapshot.notification.body[0] == '\0',
          "and the segment every process maps carries no text at all");

    // 2. The words are in the note segment, for this serial, where only a
    // process about to draw this toast will look.
    vocem::NoteReader note;
    check(strcmp(note.body_for(snapshot.notification.serial), "THE-SECRET-TEXT") == 0,
          "the note segment carries the text, for the toast the serial names");
    // And a reader asking about a message that is not the live one gets
    // nothing: a stale serial must not hand back somebody else's words.
    vocem::NoteReader other;
    check(other.body_for(snapshot.notification.serial + 1)[0] == '\0',
          "and nothing for a serial it was not published for");

    // 3. The toast outlives its seconds and the words go with it. The settings
    // file names a second so the wait is a wait rather than a sleep through
    // the default; the daemon allows itself a second of margin over it.
    bool gone = false;
    for (int i = 0; i < 120 && !gone; ++i) {
        usleep(100000);
        vocem::NoteReader after;
        gone = after.body_for(snapshot.notification.serial)[0] == '\0';
    }
    check(gone, "and once the toast has outlived its seconds the note is empty");
    // Unlinked, not merely scrubbed: nothing new can even open it.
    char note_name[64];
    vocem::note_shm_name(note_name, sizeof(note_name), getuid());
    const int probe = shm_open(note_name, O_RDONLY, 0);
    check(probe < 0, "and the name is gone, so nothing can open it at all");
    if (probe >= 0) {
        close(probe);
    }
    // And the slot goes with the words, which is what note.h's "between
    // messages there is nothing to read anywhere" says and what this check
    // used to assert the opposite of: it read `serial == 1`, pinning the state
    // segment's notification slot as SURVIVING its own words -- the sender's
    // display name plus the guild and the channel, in the segment every
    // graphical process of the session maps, until the next message or the next
    // daemon. The intent behind the old wording was "the expiry does not invent
    // a new notification", and that is still held: an invented one would read a
    // non-zero serial with a title beside it, and a retired one reads zero and
    // empty.
    vocem::Snapshot retired{};
    bool slot_cleared = false;
    for (int i = 0; i < 60 && !slot_cleared; ++i) {
        usleep(100000);
        slot_cleared = reader.read(retired) && retired.notification.serial == 0;
    }
    check(slot_cleared, "and the notification slot is retired with them, not left standing");
    check(slot_cleared && retired.notification.title[0] == '\0' &&
              retired.notification.user_id == 0,
          "so neither the words nor who sent them outlive the toast");

    // The daemon's death must leave the segment saying "draw nothing". A game
    // that was drawing keeps its mapping after the unlink -- unlinking removes
    // the name, not the pages -- so what the daemon publishes on its way out is
    // what every running game renders forever after. Stopping the daemon is also
    // what "quit the whole overlay" means to the interface, and a stop that
    // froze the last channel on screen would look exactly like a hang.
    kill(daemon_pid, SIGTERM);
    int status = 0;
    waitpid(daemon_pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0, "SIGTERM is a clean exit");
    check(reader.read(snapshot), "the mapping survives the daemon, as it does in a game");
    check(!snapshot.connected && !snapshot.in_channel && snapshot.user_count == 0,
          "and what it holds is a cleared state, not the last channel frozen");

    close(fd);
    close(listener);
    system(("rm -rf " + base).c_str());

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

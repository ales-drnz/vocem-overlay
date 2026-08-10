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

#include "vocem/note.h"
#include "vocem/shm.h"
#include "private_shm.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

double monotonic() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

// ---------------------------------------------------------------------------
// A WebSocket server that speaks just enough RFC 6455 for our own client.
// Frames from the daemon are masked, as the RFC requires of a client; replies
// go out unmasked, as it requires of a server.

bool write_all(int fd, const void* data, size_t length) {
    const char* p = static_cast<const char*>(data);
    while (length > 0) {
        const ssize_t n = write(fd, p, length);
        if (n <= 0) {
            return false;
        }
        p += n;
        length -= static_cast<size_t>(n);
    }
    return true;
}

bool send_text(int fd, const std::string& payload) {
    std::string frame;
    frame.push_back(static_cast<char>(0x81));  // FIN + text
    if (payload.size() < 126) {
        frame.push_back(static_cast<char>(payload.size()));
    } else {
        frame.push_back(126);
        frame.push_back(static_cast<char>((payload.size() >> 8) & 0xFF));
        frame.push_back(static_cast<char>(payload.size() & 0xFF));
    }
    frame += payload;
    return write_all(fd, frame.data(), frame.size());
}

// Pulls one complete text message out of `buffer`, reading more when it is
// short. Returns false on timeout or a closed peer.
bool recv_text(int fd, std::string& buffer, std::string& out, double deadline) {
    for (;;) {
        // Parse what is buffered first.
        if (buffer.size() >= 2) {
            const uint8_t b0 = static_cast<uint8_t>(buffer[0]);
            const uint8_t b1 = static_cast<uint8_t>(buffer[1]);
            const uint8_t opcode = b0 & 0x0F;
            const bool masked = (b1 & 0x80) != 0;
            size_t length = b1 & 0x7F;
            size_t offset = 2;
            if (length == 126) {
                if (buffer.size() < 4) {
                    goto need_more;
                }
                length = (static_cast<size_t>(static_cast<uint8_t>(buffer[2])) << 8) |
                         static_cast<uint8_t>(buffer[3]);
                offset = 4;
            } else if (length == 127) {
                return false;  // nothing here is remotely that large
            }
            const size_t mask_bytes = masked ? 4 : 0;
            if (buffer.size() >= offset + mask_bytes + length) {
                std::string payload = buffer.substr(offset + mask_bytes, length);
                if (masked) {
                    for (size_t i = 0; i < payload.size(); ++i) {
                        payload[i] = static_cast<char>(
                            payload[i] ^ buffer[offset + (i & 3)]);
                    }
                }
                buffer.erase(0, offset + mask_bytes + length);
                if (opcode == 0x1) {
                    out = payload;
                    return true;
                }
                if (opcode == 0x8) {
                    return false;  // close
                }
                continue;  // ping or continuation: nothing our client sends matters here
            }
        }
    need_more:
        const double remaining = deadline - monotonic();
        if (remaining <= 0) {
            return false;
        }
        timeval tv{};
        tv.tv_sec = static_cast<time_t>(remaining);
        tv.tv_usec = static_cast<suseconds_t>((remaining - static_cast<double>(tv.tv_sec)) * 1e6);
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        if (select(fd + 1, &set, nullptr, nullptr, &tv) <= 0) {
            return false;
        }
        char chunk[4096];
        const ssize_t n = read(fd, chunk, sizeof(chunk));
        if (n <= 0) {
            return false;
        }
        buffer.append(chunk, static_cast<size_t>(n));
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

    // The stub Discord: listening before the daemon starts, so its first
    // connection attempt is the one that lands.
    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    const int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(6463);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listener, 1) != 0) {
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
    {
        char c = 0;
        while (buffer.find("\r\n\r\n") == std::string::npos && read(fd, &c, 1) == 1) {
            buffer.push_back(c);
        }
        buffer.clear();
        const char* reply =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Upgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Accept: stub\r\n\r\n";
        write_all(fd, reply, strlen(reply));
    }

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

    // The message, exactly once. What varies below is the settings file, never
    // the notification.
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
        return s.notification.serial == 1;
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
    write_file(base + "/config/vocem/config.ini", "notification_seconds = 1.0\n");
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
    check(reader.read(snapshot) && snapshot.notification.serial == 1,
          "without inventing a new notification");

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

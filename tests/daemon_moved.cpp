// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Where you are is asked, not remembered -- and the names arrive without the
// invisible characters Discord wraps them in.
//
// Both halves are the owner's report, made while playing. Somebody moved him
// into another voice channel and the panel went on naming the old one, with the
// old people in it, for the rest of the session. And question marks appeared in
// the message box whenever anybody wrote in a channel's chat.
//
// The second one was diagnosed out of the daemon's own journal, which had
// written the title down: "<FSI>Lele<PDI> (<FSI>Chilling<PDI>, <FSI>Canali
// vocali<PDI>)" -- U+2068 FIRST STRONG ISOLATE and U+2069 POP DIRECTIONAL
// ISOLATE, which Discord puts around every name it interpolates so that a
// right-to-left name cannot scramble the sentence. No font in the overlay's
// atlas has a glyph for either (measured against the three cmaps), so ImGui drew
// its FallbackChar, which is '?'. Six isolates, six question marks.
//
// The first one has no single event to blame. VOICE_CHANNEL_SELECT is
// documented as "dispatched when the client joins a voice channel", and being
// dragged somewhere by a moderator is not the client joining anything; and even
// when it does arrive, the daemon used to answer it by asking a *different*
// question -- "which channel is selected now" -- whose answer can still be the
// old one. So this holds the property rather than the mechanism: whatever
// Discord does or does not announce, the daemon must end up describing the
// channel the user is actually in.
//
// Three claims, against the real vocemd driven by a stub Discord RPC:
//
//   1. moved with nothing announced but our own voice state disappearing from
//      the channel we were watching, the daemon asks again and adopts the new
//      channel;
//   2. told about a new channel while the "which one is selected" question
//      would still answer the old one, the daemon adopts the new one -- it asks
//      about the channel the event named;
//   3. a notification title and a channel name carrying the isolates reach the
//      segment without them, and with everything else intact.
//
// All three fail against the daemon shipped in 0.1.0-65.
//
// Isolation, exactly as tests/daemon_notification.cpp: re-executed under bwrap
// with a private /dev/shm, so the real daemon's segment is never touched, and a
// private network namespace, so port 6463 is free while Discord is running.

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

#include "vocem/shm.h"

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

// --------------------------------------------------------------- WebSocket
// The same minimal server tests/daemon_notification.cpp uses: frames from the
// daemon are masked, replies go out unmasked.

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
    frame.push_back(static_cast<char>(0x81));
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

bool recv_text(int fd, std::string& buffer, std::string& out, double deadline) {
    for (;;) {
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
                return false;
            }
            const size_t mask_bytes = masked ? 4 : 0;
            if (buffer.size() >= offset + mask_bytes + length) {
                std::string payload = buffer.substr(offset + mask_bytes, length);
                if (masked) {
                    for (size_t i = 0; i < payload.size(); ++i) {
                        payload[i] = static_cast<char>(payload[i] ^ buffer[offset + (i & 3)]);
                    }
                }
                buffer.erase(0, offset + mask_bytes + length);
                if (opcode == 0x1) {
                    out = payload;
                    return true;
                }
                if (opcode == 0x8) {
                    return false;
                }
                continue;
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

// The stub's whole job between assertions: answer whatever the daemon asks
// about channels with `answer`, until `until` seconds have passed. Every other
// message is swallowed, which is what a real client does with a SUBSCRIBE.
//
// `answer` is the full data object for a channel query, or "null" for "not in
// one". Returns how many channel questions were asked, which is itself a
// measurement -- a daemon that never asks cannot recover. `saw_connection_sub`,
// when given, records whether the daemon subscribed to VOICE_CONNECTION_STATUS.
int serve(int fd, std::string& buffer, const std::string& answer, double until,
          bool* saw_connection_sub = nullptr) {
    int asked = 0;
    std::string message;
    while (monotonic() < until && recv_text(fd, buffer, message, until)) {
        if (saw_connection_sub &&
            message.find("\"cmd\":\"SUBSCRIBE\"") != std::string::npos &&
            message.find("VOICE_CONNECTION_STATUS") != std::string::npos) {
            *saw_connection_sub = true;
        }
        if (message.find("\"cmd\":\"GET_SELECTED_VOICE_CHANNEL\"") != std::string::npos) {
            ++asked;
            send_text(fd, R"({"cmd":"GET_SELECTED_VOICE_CHANNEL","evt":null,"nonce":"chan","data":)" +
                              answer + "}");
        } else if (message.find("\"cmd\":\"GET_CHANNEL\"") != std::string::npos) {
            ++asked;
            send_text(fd, R"({"cmd":"GET_CHANNEL","evt":null,"nonce":"chan-id","data":)" + answer +
                              "}");
        }
    }
    return asked;
}

// A channel, as either query returns it.
std::string channel_json(const char* id, const char* name, const char* self_name) {
    return std::string(R"({"id":")") + id + R"(","name":")" + name +
           R"(","voice_states":[{"nick":")" + self_name +
           R"(","user":{"id":"99","username":")" + self_name +
           R"(","avatar":""},"voice_state":{}}]})";
}

}  // namespace

int main() {
    const char* daemon_path = getenv("VOCEM_DAEMON");
    if (!daemon_path || !daemon_path[0]) {
        printf("skip VOCEM_DAEMON not set: no daemon binary to drive\n");
        return 77;
    }

    if (!getenv("VOCEM_SANDBOXED")) {
        if (system("command -v bwrap >/dev/null 2>&1") != 0) {
            printf("skip bwrap is not installed, so the private /dev/shm cannot be built\n");
            return 77;
        }
        char self[4096];
        const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n <= 0) {
            printf("FAIL cannot find my own binary\n");
            return 1;
        }
        self[n] = '\0';
        setenv("VOCEM_SANDBOXED", "1", 1);
        execlp("bwrap", "bwrap", "--dev-bind", "/", "/", "--tmpfs", "/dev/shm", "--unshare-net",
               "--die-with-parent", self, nullptr);
        printf("FAIL could not exec bwrap\n");
        return 1;
    }

    alarm(120);

    char root[] = "/tmp/vocem-moved-test-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    const std::string base = root;
    for (const char* leaf :
         {"/state", "/state/vocem", "/config", "/config/vocem", "/cache", "/runtime"}) {
        mkdir((base + leaf).c_str(), 0700);
    }
    write_file((base + "/state/vocem/token").c_str(), "test-token\n");

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

    // Authenticate as user 99, which is who the voice states below belong to.
    std::string message;
    bool authenticated = false;
    bool watches_connection = false;
    const double handshake_deadline = monotonic() + 10.0;
    while (!authenticated && recv_text(fd, buffer, message, handshake_deadline)) {
        if (message.find("\"cmd\":\"AUTHENTICATE\"") != std::string::npos) {
            send_text(fd,
                      R"({"cmd":"AUTHENTICATE","evt":null,"nonce":"auth",)"
                      R"("data":{"user":{"id":"99","username":"stub"}}})");
            authenticated = true;
        }
    }
    check(authenticated, "and authenticated with the stored token");

    vocem::StateReader reader;
    vocem::Snapshot snapshot{};

    // ---- in the first channel, with the isolates around the name.
    // U+2068 is e2 81 a8 and U+2069 is e2 81 a9 in UTF-8; they are written out
    // here so the fixture carries the very bytes the owner's journal did.
    const char* fsi = "\xe2\x81\xa8";
    const char* pdi = "\xe2\x81\xa9";
    const std::string wrapped_name = std::string(fsi) + "Lele" + pdi;
    const std::string first = channel_json("111", "Chilling", wrapped_name.c_str());

    serve(fd, buffer, first, monotonic() + 3.0, &watches_connection);
    check(watches_connection,
          "it subscribed to VOICE_CONNECTION_STATUS, the documented event for a voice "
          "connection that changed without the client having joined anything");
    check(wait_for(reader, snapshot, 8.0,
                   [](const vocem::Snapshot& s) {
                       return s.in_channel && strcmp(s.channel_name, "Chilling") == 0;
                   }),
          "the daemon adopted the channel it was told it was in");
    check(snapshot.user_count == 1 && strcmp(snapshot.users[0].name, "Lele") == 0,
          "and the participant's name arrived without the isolates Discord wraps it in");

    // ---- 1. moved, with nothing announced but our own voice state going away.
    // This is what being dragged into another channel looks like from the
    // subscription we still hold on the old one. No VOICE_CHANNEL_SELECT at all.
    const std::string second = channel_json("222", "Gaming", "Lele");
    send_text(fd,
              R"({"cmd":"DISPATCH","evt":"VOICE_STATE_DELETE","nonce":null,)"
              R"("data":{"user":{"id":"99","username":"stub"}}})");
    const int asked = serve(fd, buffer, second, monotonic() + 12.0);
    check(asked > 0, "being moved made the daemon ask where it is, rather than assume");
    check(wait_for(reader, snapshot, 10.0,
                   [](const vocem::Snapshot& s) {
                       return s.in_channel && strcmp(s.channel_name, "Gaming") == 0;
                   }),
          "and it now names the channel the user is actually in");
    check(snapshot.user_count == 1 && strcmp(snapshot.users[0].name, "Lele") == 0,
          "with that channel's people, not the old channel's");

    // ---- 2. told about a third channel while "which one is selected" still
    // answers the second. The daemon has to ask about the channel the event
    // named; asking the other question would put it back where it was.
    const std::string third = channel_json("333", "Studio", "Lele");
    send_text(fd,
              R"({"cmd":"DISPATCH","evt":"VOICE_CHANNEL_SELECT","nonce":null,)"
              R"("data":{"channel_id":"333","guild_id":"1"}})");
    bool asked_by_id = false;
    {
        // The race, reproduced honestly. Until the client has finished
        // switching, "which channel is selected" still answers the old one; only
        // the event knows where we are going. So the stale answer is given until
        // the daemon asks about the channel by id, and the truth afterwards --
        // a stub that lied forever would be testing the stub.
        const double until = monotonic() + 8.0;
        std::string incoming;
        while (monotonic() < until && recv_text(fd, buffer, incoming, until)) {
            if (incoming.find("\"cmd\":\"GET_CHANNEL\"") != std::string::npos) {
                // And about the right channel: the id the event carried.
                if (incoming.find("\"channel_id\":\"333\"") != std::string::npos) {
                    asked_by_id = true;
                }
                send_text(fd, R"({"cmd":"GET_CHANNEL","evt":null,"nonce":"chan-id","data":)" +
                                  third + "}");
            } else if (incoming.find("\"cmd\":\"GET_SELECTED_VOICE_CHANNEL\"") !=
                       std::string::npos) {
                send_text(
                    fd,
                    R"({"cmd":"GET_SELECTED_VOICE_CHANNEL","evt":null,"nonce":"chan","data":)" +
                        (asked_by_id ? third : second) + "}");
            }
        }
    }
    check(asked_by_id,
          "an announced move asks about the channel the event named, by its id");
    check(wait_for(reader, snapshot, 6.0,
                   [](const vocem::Snapshot& s) {
                       return s.in_channel && strcmp(s.channel_name, "Studio") == 0;
                   }),
          "and the panel follows there rather than back to the stale answer");

    // ---- 3. the voice connection changing state, and nothing else. Being moved
    // makes the client re-establish its connection to the new channel's
    // endpoint, so the documented status walks through AWAITING_ENDPOINT on its
    // way back to VOICE_CONNECTED whoever started the move. Only the transition
    // is acted on: the payload carries the last twenty pings, so this event
    // arrives while nothing is happening.
    const std::string fourth = channel_json("444", "Musica", "Lele");
    send_text(fd,
              R"({"cmd":"DISPATCH","evt":"VOICE_CONNECTION_STATUS","nonce":null,)"
              R"("data":{"state":"AWAITING_ENDPOINT","pings":[20],"average_ping":20}})");
    send_text(fd,
              R"({"cmd":"DISPATCH","evt":"VOICE_CONNECTION_STATUS","nonce":null,)"
              R"("data":{"state":"VOICE_CONNECTED","pings":[20],"average_ping":20}})");
    serve(fd, buffer, fourth, monotonic() + 4.0);
    check(wait_for(reader, snapshot, 8.0,
                   [](const vocem::Snapshot& s) {
                       return s.in_channel && strcmp(s.channel_name, "Musica") == 0;
                   }),
          "a voice connection that changed state made the daemon ask, and it followed");

    // ---- 4. a notification title carrying the isolates, as Discord composes it
    // for a message written in a channel's chat.
    const std::string title = std::string(fsi) + "Lele" + pdi + " (" + fsi + "Chilling" + pdi +
                              ", " + fsi + "Canali vocali" + pdi + ")";
    send_text(fd,
              R"({"cmd":"DISPATCH","evt":"NOTIFICATION_CREATE","nonce":null,"data":{"title":")" +
                  title + R"(","body":"ciao",)" +
                  R"("message":{"author":{"id":"42","avatar":"","global_name":"Lele"}}}})");
    check(wait_for(reader, snapshot, 8.0,
                   [](const vocem::Snapshot& s) { return s.notification.serial >= 1; }),
          "the notification reached the segment");
    check(strcmp(snapshot.notification.title, "Lele (Chilling, Canali vocali)") == 0,
          "and its title carries no invisible character the atlas would draw as a question mark");

    kill(daemon_pid, SIGTERM);
    int status = 0;
    waitpid(daemon_pid, &status, 0);

    close(fd);
    close(listener);
    system(("rm -rf " + base).c_str());

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

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

#include <algorithm>
#include <arpa/inet.h>
#include <functional>
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

#include "probe_alarm.h"
#include "discord_stub.h"
#include "unit_confinement.h"
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

// The stub's whole job between assertions: answer whatever the daemon asks
// about channels with `answer`, until `until` seconds have passed. Every other
// message is swallowed, which is what a real client does with a SUBSCRIBE.
//
// `answer` is the full data object for a channel query, or "null" for "not in
// one". Returns how many channel questions were asked, which is itself a
// measurement -- a daemon that never asks cannot recover. `saw_connection_sub`,
// when given, records whether the daemon subscribed to VOICE_CONNECTION_STATUS.
//
// `done`, when given, ends the serving early: it is asked of the segment every
// tenth of a second, so a window lasts until what it waits for has happened
// and not a fixed span. It was only fixed spans -- 3, 12, 8 and 4 s, 27 s a
// run whatever the daemon did (DESIGN 193).
int serve(int fd, std::string& buffer, const std::string& answer, double until,
          bool* saw_connection_sub = nullptr,
          const std::function<bool()>& done = nullptr) {
    int asked = 0;
    std::string message;
    while (monotonic() < until) {
        if (done && done()) {
            break;
        }
        const double slice = std::min(until, monotonic() + 0.1);
        if (!recv_text(fd, buffer, message, slice)) {
            continue;
        }
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

// Waits for the daemon's own five-second reconciliation -- a
// GET_SELECTED_VOICE_CHANNEL nobody prompted -- and answers it with `answer`.
// What it buys is a clock: `channel_checked` is reset only there
// (daemon/src/main.cpp), so for five seconds after this returns the daemon
// will not reconcile again, and anything it asks in that span it asked
// because of an event. The two claims below that rest on an event (our own
// voice state going away, the voice connection changing state) send it right
// after this and require the daemon to follow within two seconds: without the
// clock, the reconciliation answered them for it and both passed with the
// event's handling deleted (DESIGN 193, measured by mutation).
bool await_reconcile(int fd, std::string& buffer, const std::string& answer, double until) {
    std::string message;
    while (monotonic() < until && recv_text(fd, buffer, message, until)) {
        if (message.find("\"cmd\":\"GET_SELECTED_VOICE_CHANNEL\"") != std::string::npos) {
            send_text(fd, R"({"cmd":"GET_SELECTED_VOICE_CHANNEL","evt":null,"nonce":"chan","data":)" +
                              answer + "}");
            return true;
        }
    }
    return false;
}

// How long the daemon has, after an event, to follow it on its own: well under
// the five seconds before its next reconciliation can come.
constexpr double kEventSeconds = 2.0;

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

    if (const int gate = vocem_test::ensure_daemon_confinement(); gate >= 0) {
        return gate;
    }

    vocem_test::set_alarm(120, "being dragged between channels");

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

    std::string buffer;
    check(vocem_test::accept_upgrade(fd), "and upgraded");

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

    // The segment names `channel` and has its people in it: the name and the
    // participants are published apart, and the checks after each wait read
    // both -- a wait that ended at the name alone read an empty user list.
    const auto names = [&](const char* channel) {
        return [&reader, &snapshot, channel] {
            return (reader.valid() || reader.open()) && reader.read(snapshot) &&
                   snapshot.in_channel && strcmp(snapshot.channel_name, channel) == 0 &&
                   snapshot.user_count >= 1;
        };
    };
    serve(fd, buffer, first, monotonic() + 8.0, &watches_connection, [&] {
        return watches_connection && names("Chilling")();
    });
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
    check(await_reconcile(fd, buffer, first, monotonic() + 8.0),
          "the daemon reconciled on its own clock (the start of the span the event is timed in)");
    send_text(fd,
              R"({"cmd":"DISPATCH","evt":"VOICE_STATE_DELETE","nonce":null,)"
              R"("data":{"user":{"id":"99","username":"stub"}}})");
    const double moved_at = monotonic();
    const int asked = serve(fd, buffer, second, moved_at + kEventSeconds, nullptr, names("Gaming"));
    const double followed = monotonic() - moved_at;
    printf("     followed the move in %.2f s (the next reconciliation is five away)\n", followed);
    check(asked > 0, "being moved made the daemon ask where it is, rather than assume");
    check(names("Gaming")(),
          "and it names the channel the user is actually in within two seconds, before its "
          "own reconciliation could have done it");
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
        while (monotonic() < until && !(asked_by_id && names("Studio")())) {
            if (!recv_text(fd, buffer, incoming, std::min(until, monotonic() + 0.1))) {
                continue;
            }
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
    check(await_reconcile(fd, buffer, third, monotonic() + 8.0),
          "the daemon reconciled on its own clock again");
    send_text(fd,
              R"({"cmd":"DISPATCH","evt":"VOICE_CONNECTION_STATUS","nonce":null,)"
              R"("data":{"state":"AWAITING_ENDPOINT","pings":[20],"average_ping":20}})");
    send_text(fd,
              R"({"cmd":"DISPATCH","evt":"VOICE_CONNECTION_STATUS","nonce":null,)"
              R"("data":{"state":"VOICE_CONNECTED","pings":[20],"average_ping":20}})");
    const double changed_at = monotonic();
    serve(fd, buffer, fourth, changed_at + kEventSeconds, nullptr, names("Musica"));
    printf("     followed the connection's change in %.2f s\n", monotonic() - changed_at);
    check(names("Musica")(),
          "a voice connection that changed state made the daemon ask, and it followed within "
          "two seconds, before its own reconciliation could have");

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

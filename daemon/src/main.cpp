// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// vocemd -- the only component that talks to Discord.
//
// It holds the RPC connection, tracks who is in the voice channel and who is
// speaking, and publishes that into shared memory for the in-game layer to
// read. Nothing here runs inside a game process, which is the whole point: the
// code that does I/O and parses JSON is kept as far away from the render loop
// as possible.
//
// This file is the loops: the port walk, the connection, the pauses between
// them, and the tick every one of them drives. What the channel is
// (session.h) and what the protocol says (rpc_client.h) are files of their
// own since entry 134; a thousand lines in one anonymous namespace was a
// model nothing could test and a protocol nobody could read in one sitting.
//
// Authorisation is its own: the daemon asks Discord for a token the first time it
// connects and stores it. See auth.h for how, and for what that costs us.

#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <nlohmann/json.hpp>

#include "auth.h"
#include "avatars.h"
#include "flatpak_bridge.h"
#include "log.h"
#include "rpc_client.h"
#include "session.h"
#include "vocem/clock.h"
#include "vocem/display.h"
#include "vocem/journal.h"
#include "vocem/live_config.h"
#include "vocem/shm.h"
#include "websocket.h"

using json = nlohmann::json;

namespace {

constexpr const char* kOrigin = "http://localhost:3000";
// Discord's client takes the first free port in this range, "trying sequentially
// until it can bind to one", and tells clients to check the same way. Only 6463
// was ever tried: with anything else holding it, Discord ran on 6464 and the
// overlay said "Waiting for Discord" for ever, with nothing in the log.
constexpr uint16_t kRpcPortFirst = 6463;
constexpr uint16_t kRpcPortLast = 6472;

// The cadences, named. Each was a bare number in the loop that used it.
constexpr double kReconcileSeconds = 5.0;  // ask Discord where we are
constexpr double kDisplaySeconds = 60.0;   // re-read /sys/class/drm
constexpr double kBridgeSeconds = 1.0;     // sweep the Flatpak sandboxes
constexpr int kBackoffCapSeconds = 30;     // the longest pause between attempts
constexpr int kRecvTimeoutMs = 1000;       // one tick's worth of waiting on the socket

volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int) { g_stop = 1; }

}  // namespace

int main() {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    std::signal(SIGPIPE, SIG_IGN);

    vocem::StateWriter writer;
    if (!writer.open()) {
        LOG("could not create the shared state segment");
        return 1;
    }

    // The daemon's own session journal (vocem/journal.h): the Debug section's
    // log of connection life, and -- exactly as for a game -- a `.running`
    // journal whose process is gone is a daemon that died without unwinding.
    vocem::journal_begin("daemon", "vocemd");

    // The second half of the same publication, for games that are themselves
    // Flatpaks and cannot see the segment above. Hung off the writer so there is
    // one publish path: every state the host sees, a served sandbox sees.
    vocem::FlatpakBridge bridge;
    bridge.start();
    bridge.rescan();
    writer.on_publish_context = &bridge;
    writer.on_publish = [](const vocem::SharedState& state, void* context) {
        static_cast<vocem::FlatpakBridge*>(context)->publish(state);
    };

    vocem::AvatarCache avatars;
    vocem::Session session(writer, avatars);
    // The message's words take the same road as the state, and are removed from
    // it at the same moment they are removed from the segment.
    session.note().on_publish_context = &bridge;
    session.note().on_publish = [](uint64_t serial, const char* body, void* context) {
        static_cast<vocem::FlatpakBridge*>(context)->publish_note(serial, body);
    };
    // The user's settings, reread on the same live mechanism the overlay uses --
    // one stat() every couple of seconds, a reparse only when the file moved. The
    // daemon consumes exactly one key: notification_seconds, the toast's
    // lifetime, which is what the note segment's expiry runs on (the tick
    // below).
    vocem::LiveConfig live_config;
    session.set_display_height(vocem::display_height());
    session.set_connected(false);

    // Everything that has to happen whatever Discord is doing, in one place so
    // that a loop cannot run some of it. Every loop in main() that spends time
    // -- the port walk's pause, the reconnect pause, the wait after a refused
    // authorisation, and the recv loop itself -- calls this and nothing else.
    //
    // That shape is the fix for a defect, not a tidying: the note's expiry used
    // to be called from the recv loop alone, so it ran only while a Discord
    // connection was up. Discord quitting after a message therefore left the
    // words of that message in /dev/shm, readable by every process in the
    // session, for as long as it stayed away -- measured against the packaged
    // 0.1.4-1 daemon as still there forty seconds after the connection closed,
    // which is where the counting stopped and not where the exposure ended.
    // `vocem/note.h` promises the words live no longer than the toast; the toast
    // is timed by this daemon, so the clock has to be this daemon's own.
    double bridge_checked = 0.0;
    double display_checked = 0.0;
    const auto tick = [&] {
        session.expire_note(live_config.current().notification_seconds);

        const double now = vocem::monotonic_seconds();

        // The display, on its own slower clock: a mode switch or a plugged
        // monitor is rare, and four sysfs opens a minute cost nothing. Here and
        // not in the recv loop, where it lived until 0.1.5: there it ran only
        // while a Discord connection was up, so a monitor plugged in while
        // Discord was closed left every running game sized to the old mode
        // until Discord came back.
        if (now - display_checked >= kDisplaySeconds) {
            display_checked = now;
            session.set_display_height(vocem::display_height());
        }

        // The bridge is on the slower clock the sandboxes' needs run on: a
        // game starting is when a new one appears, and config.ini and the
        // avatar files move on a human's timescale. The state itself does not
        // wait for this -- it goes out with every publish. The republish is
        // here so a sandbox that has just been adopted does not sit on an
        // empty state until the next thing Discord says.
        if (now - bridge_checked < kBridgeSeconds) {
            return;
        }
        bridge_checked = now;
        bridge.rescan();
        if (const vocem::SharedState* state = writer.state()) {
            bridge.publish(*state);
            bridge.refresh_files(*state);
        }
    };

    // A pause that keeps ticking. Three loops spelled this by hand.
    const auto pause_ticking = [&](int seconds) {
        for (int i = 0; i < seconds && !g_stop; ++i) {
            sleep(1);
            tick();
        }
    };

    // A note found here was written by a daemon that is gone, and its toast went
    // with it. `~NoteWriter()` retires the words on a clean exit, but the unit
    // carries `Restart=on-failure` and `MemoryMax=128M`, and a SIGKILL runs no
    // destructor -- so an OOM-killed daemon leaves the last message's text
    // behind exactly as a crash does. Nothing can be done inside the process
    // being killed; declining the inheritance is what the next one can do.
    // `clear()` is the whole retirement and not just the unlink: it also fires
    // the `on_publish` hook above, which removes the mirror of those words from
    // every Flatpak sandbox that was being served.
    session.note().clear();

    const std::string path = std::string("/?v=1&client_id=") + vocem::kClientId;
    int backoff_seconds = 1;
    bool warned_about_token = false;

    // Escape hatch for exercising the authorisation flow when a token is already
    // stored, which is otherwise unreachable once authorisation succeeded.
    const bool force_authorise = [] {
        const char* env = std::getenv("VOCEM_FORCE_AUTHORISE");
        return env && env[0] == '1';
    }();

    while (!g_stop) {
        // No token is not an error: the daemon asks Discord for authorisation as
        // soon as it is reachable, and until then it waits.
        const std::string token = force_authorise ? std::string() : vocem::load_token();
        if (token.empty() && !warned_about_token) {
            warned_about_token = true;
            LOG("not authorised yet -- will ask Discord once it is reachable");
        }

        vocem::WebSocket socket;
        uint16_t reached_on = 0;
        for (uint16_t port = kRpcPortFirst; port <= kRpcPortLast && !g_stop; ++port) {
            if (!socket.connect("127.0.0.1", port, path, kOrigin)) {
                continue;
            }
            // Who is on the other end, before a token with messages.read scope is
            // handed to it. Anything that binds the port before Discord does gets
            // this connection; a peer owned by another user, or by a sandbox that
            // can bind loopback but cannot read our state directory, is not
            // Discord and is told so out loud rather than trusted quietly.
            const vocem::PeerIdentity owner = socket.peer_owner();
            if (owner.outcome == vocem::PeerOwner::Found &&
                static_cast<uid_t>(owner.uid) != getuid()) {
                LOG("refusing port %u: the process listening there belongs to uid %ld, not to "
                    "this user -- not sending it the Discord token",
                    port, owner.uid);
                socket.close();
                continue;
            }
            if (owner.outcome != vocem::PeerOwner::Found) {
                // Not a refusal: a container with /proc restricted cannot answer
                // the question, and a daemon that will not work there is worse
                // than one that cannot check. Said out loud, every time, because
                // a check that quietly does not happen is worth nothing.
                LOG("could not establish who owns the listener on port %u; continuing without "
                    "that check", port);
            }
            reached_on = port;
            break;
        }
        // Between ports, because `connect()` above is allowed a whole handshake
        // deadline against a peer that accepts and then says nothing, and ten
        // ports of that would accumulate into a minute and a half in which the
        // note's expiry never came round. One tick per port bounds the wait to
        // one deadline rather than ten.
        tick();
        if (reached_on == 0) {
            DBG("Discord not reachable on ports %u-%u, retrying in %ds", kRpcPortFirst,
                kRpcPortLast, backoff_seconds);
            pause_ticking(backoff_seconds);
            backoff_seconds = backoff_seconds < kBackoffCapSeconds ? backoff_seconds * 2
                                                                   : kBackoffCapSeconds;
            continue;
        }

        LOG("connected to Discord RPC on port %u", reached_on);
        vocem::journal_note("connected to Discord RPC");
        vocem::RpcClient client(socket, session, token, &g_stop);

        double channel_checked = 0.0;
        while (!g_stop && !client.failed() && !client.broken()) {
            std::string raw;
            const auto result = socket.recv(raw, kRecvTimeoutMs);
            // The recv timeout is one of the four places the tick is driven
            // from, and for a long time it was the only one -- which is what
            // left a message's words in /dev/shm whenever Discord went away
            // between the toast and the tick. See `tick` above.
            tick();
            const double tick_now = vocem::monotonic_seconds();
            // Which channel we are actually in, asked rather than remembered.
            // Discord announces a move it performs for you; being moved by
            // somebody else is not the client joining anything, and
            // VOICE_CHANNEL_SELECT is documented as "dispatched when the client
            // joins a voice channel". The panel is meant to describe what is
            // around you right now, so where you are is reconciled on a tick and
            // the events only make it instant. The reply costs nothing when the
            // answer is the channel we already know.
            if (client.authenticated() && tick_now - channel_checked >= kReconcileSeconds) {
                channel_checked = tick_now;
                client.reconcile_channel();
            }
            if (result == vocem::WebSocket::Result::Closed) {
                break;
            }
            if (result == vocem::WebSocket::Result::Timeout) {
                continue;
            }
            json message = json::parse(raw, nullptr, false);
            if (message.is_discarded()) {
                continue;
            }
            client.handle(message);
        }

        const bool authorisation_failed = client.failed();
        session.set_connected(false);
        if (authorisation_failed) {
            // Keep the segment alive with the reason in it: the interface has to be
            // able to explain why nothing is happening, and offer to try again.
            session.set_status(vocem::DaemonStatus::AuthorisationRefused);
            LOG("authorisation did not succeed -- waiting for the interface to retry");
            vocem::journal_note("authorisation refused; waiting for a retry");
            while (!g_stop) {
                pause_ticking(1);
            }
            break;
        }
        // A connection that got as far as authenticating is evidence Discord is
        // really there, and the next attempt should be immediate. One that did
        // not is a peer that took the connection and dropped it -- Discord
        // refusing an origin or a client_id, a client still starting, or
        // something else on the port entirely -- and reconnecting at once turns
        // that into a busy loop. The backoff used to be reset the moment the
        // socket connected, which made "reachable" and "willing to talk" the
        // same question: measured against the packaged 0.1.4-1 daemon, 1914
        // connections in five seconds against a peer that answered the handshake
        // and hung up, where this makes 3 -- and the stub, not the daemon, was
        // what set that ceiling (tests/daemon_reconnect.cpp).
        if (client.authenticated()) {
            backoff_seconds = 1;
        }
        if (!g_stop) {
            LOG("connection lost, reconnecting%s",
                client.authenticated() ? "" : " after a pause: the session never started");
            vocem::journal_note("connection lost, reconnecting");
        }
        if (!client.authenticated()) {
            pause_ticking(backoff_seconds);
            backoff_seconds = backoff_seconds < kBackoffCapSeconds ? backoff_seconds * 2
                                                                   : kBackoffCapSeconds;
        }
    }

    LOG("shutting down");
    // The names first, the worker last. What has to be gone before this
    // process is -- the segment's name, the note's words, every Flatpak
    // mirror -- goes now, while nothing can still delay it. The avatar worker
    // is joined afterwards: a download in flight is aborted from its progress
    // callback the moment stop() is called (avatars.cpp), but a join that ran
    // FIRST, as it did through 0.1.7, put a stalled CDN between SIGTERM and
    // the unlink -- up to the transfer's own fifteen-second timeout against a
    // unit whose TimeoutStopSec is ten, so systemctl stop ended in SIGKILL
    // with the segment still published, the exact leftover entry 81 forbids
    // (entry 134). tests/daemon_stop_unlinks.cpp measures the seconds.
    bridge.stop();
    writer.close();
    vocem::StateWriter::unlink_segment();
    session.note().clear();
    avatars.stop();
    vocem::journal_end();
    return 0;
}

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
// own.
//
// Authorisation is its own: the daemon asks Discord for a token the first time it
// connects and stores it. See auth.h for how, and for what that costs us.

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <nlohmann/json.hpp>

#include "auth.h"
#include "avatars.h"
#include "flatpak_bridge.h"
#include "log.h"
#include "rpc_client.h"
#include "session.h"
#include "text.h"
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
// until it can bind to one", and tells clients to check the same way; with
// anything else holding 6463, Discord runs on a later port (entry 73).
constexpr uint16_t kRpcPortFirst = 6463;
constexpr uint16_t kRpcPortLast = 6472;

// The cadences, named.
constexpr double kReconcileSeconds = 5.0;  // ask Discord where we are
constexpr double kDisplaySeconds = 60.0;   // re-read /sys/class/drm
constexpr double kBridgeSeconds = 1.0;     // sweep the Flatpak sandboxes
constexpr int kBackoffCapSeconds = 30;     // the longest pause between attempts
constexpr int kRecvTimeoutMs = 1000;       // one tick's worth of waiting on the socket

volatile std::sig_atomic_t g_stop = 0;

void handle_signal(int) { g_stop = 1; }

// Whether this is the only daemon of this user. 1: the lock is ours, and stays
// ours until the process ends (the descriptor is never closed, so the kernel
// lets go of it however the process goes). 0: another daemon holds it. -1:
// the question could not be asked, with the reason in `why`.
//
// The segment is opened O_CREAT without O_EXCL and its seqlock assumes one
// writer, so a second vocemd would publish into the first one's segment, and
// whichever stopped first would unlink the name the other was still using
// (tests/daemon_single_instance.cpp).
//
// The lock file sits beside the segment in /dev/shm, not in $XDG_RUNTIME_DIR,
// because /dev/shm is what every daemon test makes private. /dev/shm is
// world-writable, so the file is opened without following a link or waiting
// on a FIFO and is believed only when it is a regular file of this user's: a
// file planted by another user must not keep this daemon from starting.
int take_instance_lock(std::string& why) {
    char segment[64];
    vocem::shm_name(segment, sizeof(segment), static_cast<unsigned>(getuid()));
    const std::string path = std::string("/dev/shm") + segment + ".lock";
    const int fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (fd < 0) {
        why = path + ": " + std::strerror(errno);
        return -1;
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_uid != getuid()) {
        ::close(fd);
        why = path + " is not a regular file of this user's";
        return -1;
    }
    if (::flock(fd, LOCK_EX | LOCK_NB) == 0) {
        return 1;  // held for the life of the process
    }
    const int error = errno;
    ::close(fd);
    if (error == EWOULDBLOCK) {
        why = path;
        return 0;
    }
    why = path + ": " + std::strerror(error);
    return -1;
}

}  // namespace

int main() {
    // sigaction and not std::signal, for one flag: SA_RESTART, which glibc's
    // signal() sets. With it, a handler that only raises g_stop changes
    // nothing for a call that is blocked -- the kernel restarts the call and
    // the flag is never read, so a FIFO planted at an avatar cache name or at
    // config.ini would hold the daemon through SIGTERM until the unit's
    // SIGKILL. Without it a blocked call returns EINTR, and every loop here
    // reads g_stop. The avatar worker blocks both signals (avatars.cpp) so
    // that they land on this thread, the one that has to notice.
    struct sigaction stop_action {};
    stop_action.sa_handler = handle_signal;
    sigemptyset(&stop_action.sa_mask);
    stop_action.sa_flags = 0;
    sigaction(SIGINT, &stop_action, nullptr);
    sigaction(SIGTERM, &stop_action, nullptr);
    std::signal(SIGPIPE, SIG_IGN);

    // Before anything is opened: a second daemon must not touch the segment
    // at all, not even to create it. Exit 0, because there is nothing wrong --
    // the user is served -- and the unit's Restart=on-failure must not answer.
    {
        std::string why;
        const int lock = take_instance_lock(why);
        if (lock == 0) {
            LOG("another vocemd already serves this user (it holds %s); exiting", why.c_str());
            return 0;
        }
        if (lock < 0) {
            // Cannot tell is not "somebody else is running": said, and on.
            LOG("could not take the single-instance lock (%s); continuing without it",
                why.c_str());
        }
    }

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
    // The note's expiry in particular must run while no Discord connection is
    // up: `vocem/note.h` promises the words live no longer than the toast, and
    // Discord quitting after a message must not leave them in /dev/shm
    // (entry 112). The toast is timed by this daemon, so the clock has to be
    // this daemon's own.
    double bridge_checked = 0.0;
    double display_checked = 0.0;
    const auto tick = [&] {
        session.expire_note(live_config.current().notification_seconds);

        const double now = vocem::monotonic_seconds();

        // The display, on its own slower clock: a mode switch or a plugged
        // monitor is rare, and four sysfs opens a minute cost nothing. Here and
        // not in the recv loop, so a monitor plugged in while Discord is closed
        // still resizes every running game.
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

    // A pause that keeps ticking.
    const auto pause_ticking = [&](int seconds) {
        for (int i = 0; i < seconds && !g_stop; ++i) {
            sleep(1);
            tick();
        }
    };

    // A note found here was written by a daemon that is gone, and its toast went
    // with it: a SIGKILL (the unit's MemoryMax, a crash) runs no destructor, so
    // `~NoteWriter()` never retired the words. Declining the inheritance is what
    // the next daemon can do. `clear()` also fires the `on_publish` hook above,
    // which removes the mirror of those words from every Flatpak sandbox.
    session.note().clear();

    const std::string path = std::string("/?v=1&client_id=") + vocem::kClientId;
    int backoff_seconds = 1;
    bool warned_about_token = false;
    // Once per daemon: a kernel without sock_diag, or a unit without
    // AF_NETLINK, answers the same way on every connection.
    bool cgroup_unavailable_said = false;

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
        // Every wait this client makes reads the stop flag on its way through,
        // so a SIGTERM arriving inside the connect or the handshake ends the
        // daemon then rather than after the deadline (websocket.cpp).
        socket.watch_stop(&g_stop);
        uint16_t reached_on = 0;
        for (uint16_t port = kRpcPortFirst; port <= kRpcPortLast && !g_stop; ++port) {
            // Before each attempt, not after the walk: see the tick below.
            tick();
            if (!socket.connect("127.0.0.1", port, path, kOrigin)) {
                continue;
            }
            // Who is on the other end, before a token with messages.read scope is
            // handed to it. Anything that binds the port before Discord does gets
            // this connection; a peer owned by another user, or by a Flatpak
            // sandbox that is not Discord's own (same uid, network shared,
            // $XDG_STATE_HOME out of reach), is not Discord and is told so out
            // loud rather than trusted quietly. peer_identity.h says how each is
            // told apart and what the uid alone could not; peer_cgroup.h says
            // why the socket's cgroup is asked first.
            const vocem::PeerIdentity owner = socket.peer_owner();
            if (owner.outcome == vocem::PeerOwner::Found &&
                static_cast<uid_t>(owner.uid) != getuid()) {
                LOG("refusing port %u: the process listening there belongs to uid %ld, not to "
                    "this user -- not sending it the Discord token",
                    port, owner.uid);
                socket.close();
                continue;
            }
            // First source: the cgroup the listener's socket was made in, which
            // the kernel reports without any access to a process. It is the
            // only one the shipped unit leaves the daemon: its user namespace
            // closes every process of the session to the /proc walk below
            // (entry 285).
            const vocem::PeerCgroup cgroup = socket.peer_cgroup();
            bool cgroup_answered = false;
            if (!cgroup.known) {
                if (!cgroup_unavailable_said) {
                    cgroup_unavailable_said = true;
                    LOG("cannot ask the kernel which cgroup the listener on port %u was made in "
                        "(%s: %s); the Flatpak check falls back to /proc alone, which a "
                        "confined daemon cannot see into",
                        port, cgroup.failed ? cgroup.failed : "?",
                        cgroup.error ? std::strerror(cgroup.error) : "no error given");
                }
            } else if (cgroup.path.empty()) {
                LOG("the listener on port %u was made in cgroup %llu, which is nowhere under "
                    "/sys/fs/cgroup; continuing without the cgroup check",
                    port, static_cast<unsigned long long>(cgroup.id));
            } else {
                cgroup_answered = true;
                const std::string app = vocem::flatpak_of_cgroup(cgroup.path);
                if (!app.empty() && !vocem::is_discord_flatpak(app)) {
                    LOG("refusing port %u: the socket listening there was made in the Flatpak "
                        "sandbox of %s (cgroup %s), not Discord's -- not sending it the Discord "
                        "token",
                        port, vocem::sanitise_text(app).c_str(),
                        vocem::sanitise_text(cgroup.path).c_str());
                    socket.close();
                    continue;
                }
                DBG("port %u is answered from cgroup %s", port, cgroup.path.c_str());
            }
            // Second source: the process holding the socket and the
            // /.flatpak-info at its root, which is what a daemon run from a
            // shell (and the bwrap tests) can still read. Either source
            // naming a Flatpak that is not Discord's refuses.
            if (owner.outcome != vocem::PeerOwner::Found) {
                // Not a refusal: a container with /proc restricted cannot answer
                // the question, and a daemon that will not work there is worse
                // than one that cannot check. Said out loud, every time, because
                // a check that quietly does not happen is worth nothing.
                if (!cgroup_answered) {
                    LOG("could not establish who owns the listener on port %u; continuing "
                        "without that check",
                        port);
                }
            } else {
                const vocem::PeerProcess process = vocem::socket_process(owner.inode);
                if (process.place == vocem::PeerPlace::Flatpak &&
                    !vocem::is_discord_flatpak(process.app_id)) {
                    LOG("refusing port %u: the process listening there (pid %ld) is in the "
                        "Flatpak sandbox of %s, not Discord's -- not sending it the Discord token",
                        port, process.pid, vocem::sanitise_text(process.app_id).c_str());
                    socket.close();
                    continue;
                }
                if (process.place == vocem::PeerPlace::Hidden ||
                    process.place == vocem::PeerPlace::Unknown) {
                    // Cannot tell, which is not hostile: /proc could not be
                    // listed, the holder's root could not be read, or no
                    // process this daemon can look into holds the socket --
                    // always the case from the unit. An undumpable squatter in
                    // a Flatpak is refused above, by its cgroup. Said, every
                    // time, when the cgroup did not answer either.
                    if (!cgroup_answered) {
                        if (process.place == vocem::PeerPlace::Hidden) {
                            LOG("could not establish which process listens on port %u: none "
                                "this daemon can look into holds it, and %d of this user's "
                                "processes are closed to it (pid %s%s); continuing without "
                                "the sandbox check",
                                port, process.hidden, process.hidden_pids.c_str(),
                                process.hidden > 4 ? ", ..." : "");
                        } else {
                            LOG("could not establish which process listens on port %u%s; "
                                "continuing without the sandbox check",
                                port, process.pid > 0 ? " (its root cannot be read)" : "");
                        }
                    }
                } else if (process.place == vocem::PeerPlace::Flatpak) {
                    DBG("port %u is answered from Discord's own Flatpak (%s)", port,
                        process.app_id.c_str());
                }
            }
            reached_on = port;
            break;
        }
        // The tick above runs once per port; this one covers the time the
        // last attempt took, before the pause below starts ticking on its own
        // second, so ten ports that accept and say nothing cannot keep the
        // tick away for ten deadlines. The tick is cheap and idempotent (its
        // two slower halves carry their own clocks).
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
            // from. See `tick` above.
            tick();
            const double tick_now = vocem::monotonic_seconds();
            // Which channel we are actually in, asked rather than remembered:
            // a move by somebody else is announced by no event (see
            // RpcClient::reconcile_channel).
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
            // Bounded before it is parsed, not after: nlohmann limits neither
            // depth nor element count, and an 8 MiB message (kMaxMessage) can
            // cost several times the unit's MemoryMax to parse.
            // vocem::json_within says what each shape costs and where the two
            // ceilings come from.
            //
            // Said once: a peer that does this once will do it again, and the
            // journal is what somebody reads afterwards.
            if (!vocem::json_within(raw, vocem::kJsonDepthCeiling, vocem::kJsonTokenCeiling)) {
                static bool said_shape = false;
                if (!said_shape) {
                    said_shape = true;
                    LOG("a message of %zu bytes nests deeper than %d levels or holds more than "
                        "%zu values; dropping it rather than parsing it",
                        raw.size(), vocem::kJsonDepthCeiling, vocem::kJsonTokenCeiling);
                }
                continue;
            }
            // An allocation that fails arrives as an exception whatever
            // `allow_exceptions = false` says. Not under the unit's MemoryMax,
            // which never makes malloc fail (past it the answer is reclaim,
            // swap or the OOM killer), but under RLIMIT_AS or strict
            // overcommit.
            json message;
            try {
                message = json::parse(raw, nullptr, false);
            } catch (const std::exception& error) {
                static bool said = false;
                if (!said) {
                    said = true;
                    LOG("a message of %zu bytes could not be parsed within this daemon's memory "
                        "(%s); dropping it and staying up",
                        raw.size(), error.what());
                }
                continue;
            }
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
        // really there, so the next attempt waits the shortest pause there is.
        // One that did not is a peer that took the connection and dropped it --
        // Discord refusing an origin or a client_id, a client still starting,
        // or something else on the port entirely -- and reconnecting at once
        // turns that into a busy loop. "Reachable" and "willing to talk" are
        // different questions, so the backoff is not reset on connect alone
        // (tests/daemon_reconnect.cpp).
        if (client.authenticated()) {
            backoff_seconds = 1;
        }
        if (!g_stop) {
            LOG("connection lost, reconnecting after %ds%s", backoff_seconds,
                client.authenticated() ? "" : ": the session never started");
            vocem::journal_note("connection lost, reconnecting");
        }
        // Always a pause, and for the authenticated case always the shortest
        // one: a session that authenticates and then ends -- Discord
        // restarting, a client that accepts AUTHENTICATE and drops -- would
        // otherwise re-enter this loop with nothing sleeping, each turn a token
        // read, a port walk, a handshake, a publish and a fan-out to every
        // mirror, with `connected` flapping under every game's panel. One
        // second is not a wait anybody notices; only the never-authenticated
        // case doubles from here.
        pause_ticking(backoff_seconds);
        if (!client.authenticated()) {
            backoff_seconds = backoff_seconds < kBackoffCapSeconds ? backoff_seconds * 2
                                                                   : kBackoffCapSeconds;
        }
    }

    LOG("shutting down");
    // The names first, the worker last. What has to be gone before this
    // process is -- the segment's name, the note's words, every Flatpak
    // mirror -- goes now, while nothing can still delay it. The avatar worker
    // is joined afterwards: a download in flight is aborted from its progress
    // callback the moment stop() is called (avatars.cpp), but a stalled CDN
    // must not stand between SIGTERM and the unlink, or the unit's
    // TimeoutStopSec ends in SIGKILL with the segment still published
    // (entry 134). tests/daemon_stop_unlinks.cpp measures the seconds.
    // The note's words first of all, and BEFORE bridge.stop(). `clear()` fires
    // the on_publish hook into FlatpakBridge::publish_note(), which unlinks the
    // mirrored copy in every sandbox it is serving -- and stop() empties
    // `mirrors_`, so the other way round the hook would unlink nothing and
    // leave the last message's text in every served sandbox until logout.
    // "Quit means quit" means there is no later daemon to clean it up.
    session.note().clear();
    bridge.stop();
    writer.close();
    vocem::StateWriter::unlink_segment();
    avatars.stop();
    vocem::journal_end();
    return 0;
}

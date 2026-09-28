// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What the daemon does when the port answers and the session does not.
//
// The retry backoff was written for the case where nothing is listening at all:
// connect refused, wait 1s, 2s, 4s, up to 30. The case it did not cover is the
// one that costs something -- a peer that ACCEPTS the connection, completes the
// WebSocket handshake and then drops it. The daemon reset its backoff the
// moment the socket connected, so a session that ended without ever getting
// anywhere was followed by an immediate reconnect, and the immediate reconnect
// by another: measured against the packaged 0.1.3-7 daemon, 2215 connections in
// five seconds where the fixed one makes 3 -- and 2215 is what this stub could
// serve, not what the daemon could ask for, so the real figure is higher. (This
// header and entry 103 used to name 0.1.4-1 and 1914: re-measured on
// 2026-09-07, the packaged 0.1.4-1 already carries the fix and makes 3, so the
// figure had come from a build before the tag. The exemplar is 0.1.3-7.)
//
// It is not a hypothetical peer. Discord's own client closes an RPC connection
// it will not serve -- a rejected origin, a client_id it does not know, a
// client still starting up -- and it does so straight after the handshake,
// which is exactly this shape.
//
// The claim: a peer that takes the connection and drops it is not a reason to
// try again at once. Five seconds of it must cost a handful of connections,
// not thousands. The count is printed either way, because the number is the
// measurement and a threshold is only where it was put.
//
// **And the same question one step further in**, with
// VOCEM_DAEMON_RECONNECT_SCENARIO=authenticated: a peer that completes the
// handshake, sends READY, answers AUTHENTICATE and *then* drops. The fix above
// made the pause conditional on `!authenticated()` -- "a connection that got as
// far as authenticating is evidence Discord is really there, and the next
// attempt should be immediate" -- which left the authenticated path with no
// sleep in it at all. Every turn of that loop is a token read, a port walk, a
// handshake, a publish and a fan-out to every Flatpak mirror on
// set_connected(true), the drop, the same again on false, and two journal
// lines: a spin, with `connected` flapping under every game's panel. Discord
// restarting produces exactly this shape, and so does anything that accepts
// AUTHENTICATE without staying -- `authenticated_` is set by any non-ERROR
// reply. One second is the pause now, for that case alone.
//
// Isolation is daemon_notification's: re-exec under bwrap with a private
// /dev/shm -- the real daemon's segment is never touched -- and a private
// network namespace, so 6463 is free even while Discord is running. Where bwrap
// is missing it reports itself skipped rather than passing without having run.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
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

namespace {

// The stub's clock and listener are tests/discord_stub.h's; this file carried a copy.
using vocem_test::monotonic;

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

// Accept, read the request line, answer 101, close. Nothing else: this peer is
// a door that opens and shuts.
void serve_and_drop(int fd) {
    std::string request;
    char c = 0;
    const double deadline = monotonic() + 2.0;
    while (request.find("\r\n\r\n") == std::string::npos && monotonic() < deadline) {
        pollfd pfd{fd, POLLIN, 0};
        if (poll(&pfd, 1, 200) <= 0) {
            break;
        }
        if (read(fd, &c, 1) != 1) {
            break;
        }
        request.push_back(c);
    }
    const char* reply =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: stub\r\n\r\n";
    ssize_t ignored = write(fd, reply, strlen(reply));
    (void)ignored;
    close(fd);
}

// Accept, upgrade, say READY, answer AUTHENTICATE, drop. A session that starts
// and does not stay -- which is what Discord restarting looks like from here.
void serve_and_authenticate(int fd) {
    if (!vocem_test::accept_upgrade(fd)) {
        close(fd);
        return;
    }
    vocem_test::send_text(fd, R"({"cmd":"DISPATCH","evt":"READY","data":{"v":1},"nonce":null})");
    std::string buffer;
    std::string message;
    const double deadline = monotonic() + 2.0;
    while (vocem_test::recv_text(fd, buffer, message, deadline)) {
        if (message.find("\"cmd\":\"AUTHENTICATE\"") != std::string::npos) {
            vocem_test::send_text(fd,
                                  R"({"cmd":"AUTHENTICATE","evt":null,"nonce":"auth",)"
                                  R"("data":{"user":{"id":"99","username":"stub"}}})");
            break;
        }
    }
    close(fd);
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

    vocem_test::set_alarm(90, "the reconnect backoff");

    const char* env_scenario = getenv("VOCEM_DAEMON_RECONNECT_SCENARIO");
    const std::string scenario = env_scenario ? env_scenario : "";
    const bool authenticate = scenario == "authenticated";
    if (!scenario.empty() && !authenticate) {
        printf("FAIL unknown VOCEM_DAEMON_RECONNECT_SCENARIO '%s'\n", scenario.c_str());
        return 1;
    }
    printf("--  scenario: a peer that %s\n",
           authenticate ? "authenticates the session and then drops it"
                        : "answers the handshake and then drops it");

    char root[] = "/tmp/vocem-reconnect-test-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    const std::string base = root;
    for (const char* leaf :
         {"/state", "/state/vocem", "/config", "/config/vocem", "/cache", "/runtime"}) {
        mkdir((base + leaf).c_str(), 0700);
    }
    // A token, so the daemon goes straight for the connection rather than
    // waiting on an authorisation prompt nobody is there to accept.
    write_file(base + "/state/vocem/token", "test-token\n");

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

    // Five seconds of a peer that will not serve. The backoff doubles from one
    // second, so a daemon that honours it makes about four attempts in that
    // window; one that does not makes as many as the loop can turn.
    const double window = 5.0;
    const double until = monotonic() + window;
    int connections = 0;
    while (monotonic() < until) {
        pollfd pfd{listener, POLLIN, 0};
        const int ready = poll(&pfd, 1, 200);
        if (ready <= 0) {
            continue;
        }
        const int fd = accept(listener, nullptr, nullptr);
        if (fd < 0) {
            continue;
        }
        ++connections;
        if (authenticate) {
            serve_and_authenticate(fd);
        } else {
            serve_and_drop(fd);
        }
    }

    printf("  %d connections in %.0f seconds\n", connections, window);
    check(connections >= 1, "the daemon did reach the port at all");
    // The bound is generous on purpose: what is being refused is a busy loop,
    // not a particular schedule. Measured: 3 with the backoff, 2215 without it
    // (the handshake scenario); and with the session authenticated, where the
    // pause is a flat second, 5 against what the installed daemon does with no
    // pause at all.
    check(connections <= 12, "and a peer that drops the session is backed off, not hammered");

    kill(daemon_pid, SIGTERM);
    int status = 0;
    waitpid(daemon_pid, &status, 0);
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "and SIGTERM still ends it cleanly, from inside the backoff");

    close(listener);
    if (system(("rm -rf " + base).c_str()) != 0) {
        printf("  (could not remove %s)\n", base.c_str());
    }

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

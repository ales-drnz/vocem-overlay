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
// by another: measured against the packaged 0.1.4-1 daemon, 1914 connections in
// five seconds where the fixed one makes 3 -- and 1914 is what this stub could
// serve, not what the daemon could ask for, so the real figure is higher.
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
        execlp("bwrap", "bwrap", "--dev-bind", "/", "/", "--tmpfs", "/dev/shm",
               "--unshare-net", "--die-with-parent", self, nullptr);
        printf("FAIL could not exec bwrap\n");
        return 1;
    }

    alarm(90);

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

    const int listener = socket(AF_INET, SOCK_STREAM, 0);
    const int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(6463);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        listen(listener, 64) != 0) {
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
        serve_and_drop(fd);
    }

    printf("  %d connections in %.0f seconds\n", connections, window);
    check(connections >= 1, "the daemon did reach the port at all");
    // The bound is generous on purpose: what is being refused is a busy loop,
    // not a particular schedule. Measured: 3 with the backoff, 1914 without it.
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

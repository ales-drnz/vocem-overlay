// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What the WebSocket client does when the peer is hostile rather than Discord.
//
// The daemon connects to whatever answers on 127.0.0.1, which is the one place in
// this project that reads bytes off a socket. The frame length was already
// bounded; four things around it were not, and each of them is measured here
// against the real `daemon/src/websocket.cpp` with a stub peer on an ephemeral
// loopback port -- no daemon, no /dev/shm, no port 6463.
//
//   1. A message is the concatenation of its frames, and only the frames were
//      capped. A peer that never sets FIN grew `pending_` without limit: 256 MiB
//      of 64 KiB continuations were swallowed whole before this test existed.
//   2. `recv` re-armed the caller's whole timeout after every control frame, so a
//      peer pinging faster than the timeout kept it inside the loop forever. The
//      daemon's per-tick duties all live after that call: the note segment stops
//      expiring, the channel stops reconciling, and the stop flag is never read
//      again, so SIGTERM ends in SIGKILL and leaves a stale segment.
//   2b. And the absolute deadline that answered 2 was consulted by poll() alone,
//      which only consults it when it has to wait. A peer pinging at the speed of
//      the wire kept the socket readable, so every poll returned at once and the
//      loop ran on the pong's own write deadline instead: recv asked for a second
//      and had not returned after forty-five. The same starvation, through the
//      door the first fix left open.
//   3. RFC 6455 section 5.5 gives control frames at most 125 bytes and requires
//      FIN. Neither was checked, so an 8 MiB "ping" was allocated and echoed back.
//   4. The frame's own read had a timeout and not a deadline: poll() re-armed it
//      on every chunk, so a peer that announced 64 KiB and then sent one byte
//      every two seconds held recv open indefinitely -- the same defect the
//      handshake loop had been fixed for, left in place one function away.
//
// Each case runs in its own forked child so a wedged client cannot hang the run.

#include <arpa/inet.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "websocket.h"

namespace {

constexpr int kFail = 1;

// A listening socket on 127.0.0.1 with a port the kernel picks.
int listener(uint16_t& port) {
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        return -1;
    }
    int one = 1;
    ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (::bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(fd, 1) != 0) {
        ::close(fd);
        return -1;
    }
    socklen_t length = sizeof(addr);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&addr), &length) != 0) {
        ::close(fd);
        return -1;
    }
    port = ntohs(addr.sin_port);
    return fd;
}

// The server half of the upgrade: read to the blank line, answer 101.
bool accept_upgrade(int server) {
    std::string request;
    char c = 0;
    while (request.find("\r\n\r\n") == std::string::npos) {
        if (request.size() > 8192 || ::recv(server, &c, 1, 0) != 1) {
            return false;
        }
        request.push_back(c);
    }
    static const char* reply =
        "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n\r\n";
    return ::send(server, reply, std::strlen(reply), MSG_NOSIGNAL) > 0;
}

// A server-to-client frame: never masked, which is what RFC 6455 requires of a
// server and what Discord sends.
std::vector<uint8_t> frame(bool fin, uint8_t opcode, const std::string& payload) {
    std::vector<uint8_t> out;
    out.push_back(static_cast<uint8_t>((fin ? 0x80 : 0x00) | opcode));
    const size_t length = payload.size();
    if (length < 126) {
        out.push_back(static_cast<uint8_t>(length));
    } else if (length <= 0xFFFF) {
        out.push_back(126);
        out.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
        out.push_back(static_cast<uint8_t>(length & 0xFF));
    } else {
        out.push_back(127);
        for (int shift = 56; shift >= 0; shift -= 8) {
            out.push_back(static_cast<uint8_t>((length >> shift) & 0xFF));
        }
    }
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

bool send_all(int fd, const std::vector<uint8_t>& bytes) {
    size_t sent = 0;
    while (sent < bytes.size()) {
        const ssize_t n = ::send(fd, bytes.data() + sent, bytes.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}

double milliseconds_since(std::chrono::steady_clock::time_point start) {
    const auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(now - start).count();
}

// Waits for the child, killing it after `limit_ms`. Returns its exit status, or
// -1 when it had to be killed -- which is itself a failure the caller reports.
int reap(pid_t child, int limit_ms) {
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        int status = 0;
        const pid_t done = ::waitpid(child, &status, WNOHANG);
        if (done == child) {
            return WIFEXITED(status) ? WEXITSTATUS(status) : kFail;
        }
        if (milliseconds_since(start) > limit_ms) {
            ::kill(child, SIGKILL);
            ::waitpid(child, nullptr, 0);
            return -1;
        }
        ::usleep(20'000);
    }
}

// ---------------------------------------------------------------------------
// 1. A message made of frames that never end.

int unterminated_message() {
    uint16_t port = 0;
    const int listen_fd = listener(port);
    if (listen_fd < 0) {
        std::fprintf(stderr, "no loopback listener\n");
        return kFail;
    }

    const pid_t child = ::fork();
    if (child == 0) {
        ::close(listen_fd);
        vocem::WebSocket socket;
        if (!socket.connect("127.0.0.1", port, "/", "http://localhost")) {
            ::_exit(kFail);
        }
        std::string out;
        socket.recv(out, 2000);
        ::_exit(0);
    }

    const int server = ::accept(listen_fd, nullptr, nullptr);
    ::close(listen_fd);
    if (server < 0 || !accept_upgrade(server)) {
        ::kill(child, SIGKILL);
        ::waitpid(child, nullptr, 0);
        std::fprintf(stderr, "the upgrade did not complete\n");
        return kFail;
    }

    // 64 KiB per frame, well under the 8 MiB per-frame cap, FIN never set.
    const std::string chunk(64 * 1024, 'x');
    size_t sent = 0;
    const size_t ceiling = 256u << 20;
    bool first = true;
    while (sent < ceiling) {
        const std::vector<uint8_t> bytes = frame(false, first ? 0x1 : 0x0, chunk);
        first = false;
        if (!send_all(server, bytes)) {
            break;  // the client hung up: it stopped accepting the message
        }
        sent += chunk.size();
    }
    ::close(server);
    reap(child, 5000);

    // The client must refuse the message long before this much of it has been
    // accepted. The cap the frames already had is 8 MiB; four times that is
    // generous and still two orders below what an unbounded reader swallows.
    const size_t allowed = 32u << 20;
    std::fprintf(stderr, "  unterminated message: peer placed %zu MiB before the client stopped\n",
                 sent >> 20);
    if (sent >= allowed) {
        std::fprintf(stderr,
                     "FAIL a peer that never sets FIN grew the reassembly buffer to %zu MiB; "
                     "the message is not bounded\n",
                     sent >> 20);
        return kFail;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// 2. A peer that pings faster than the caller's timeout.

int ping_flood() {
    uint16_t port = 0;
    const int listen_fd = listener(port);
    if (listen_fd < 0) {
        std::fprintf(stderr, "no loopback listener\n");
        return kFail;
    }

    const pid_t child = ::fork();
    if (child == 0) {
        ::close(listen_fd);
        vocem::WebSocket socket;
        if (!socket.connect("127.0.0.1", port, "/", "http://localhost")) {
            ::_exit(kFail);
        }
        std::string out;
        const auto start = std::chrono::steady_clock::now();
        const vocem::WebSocket::Result result = socket.recv(out, 1000);
        const double elapsed = milliseconds_since(start);
        // The caller asked for a second. Two is slack for the scheduler; the
        // defect this holds is unbounded, not late.
        if (result != vocem::WebSocket::Result::Timeout || elapsed > 2000.0) {
            std::fprintf(stderr, "  recv returned %d after %.0f ms\n",
                         static_cast<int>(result), elapsed);
            ::_exit(kFail);
        }
        ::_exit(0);
    }

    const int server = ::accept(listen_fd, nullptr, nullptr);
    ::close(listen_fd);
    if (server < 0 || !accept_upgrade(server)) {
        ::kill(child, SIGKILL);
        ::waitpid(child, nullptr, 0);
        std::fprintf(stderr, "the upgrade did not complete\n");
        return kFail;
    }

    // One ping every 200 ms: five times the rate of the caller's own timeout.
    const auto start = std::chrono::steady_clock::now();
    while (milliseconds_since(start) < 4000.0) {
        if (!send_all(server, frame(true, 0x9, "ping"))) {
            break;
        }
        ::usleep(200'000);
    }
    ::close(server);

    const int status = reap(child, 1000);
    if (status == -1) {
        std::fprintf(stderr,
                     "FAIL recv never came back under a ping every 200 ms: the caller's timeout "
                     "is re-armed by every control frame, so the daemon's tick starves and the "
                     "stop flag is never read again\n");
        return kFail;
    }
    if (status != 0) {
        std::fprintf(stderr, "FAIL recv did not return Timeout within its own deadline\n");
        return kFail;
    }
    std::fprintf(stderr, "  ping flood: recv honoured its deadline\n");
    return 0;
}

// ---------------------------------------------------------------------------
// 2b. The same flood, at the speed of the wire rather than five a second.
//
// The case above is a peer that pings faster than the caller's timeout. This one
// is a peer that pings faster than the client can answer, and it is a different
// defect: the deadline was absolute, but poll() is the only thing that ever
// looked at it, and poll() only looks when it has to wait. With the socket never
// empty every poll returned at once, every ping was answered with a pong, and the
// pong's own five-second write deadline was what the loop ran on -- the caller's
// second never came round again. Measured against the client that shipped 0.1.4:
// recv() asked for one second and had not returned after forty-five, with the
// daemon's tick -- note expiry, channel reconciliation, the stop flag -- stopped
// for all of it. That is entry 72's starvation, alive, one function further in.
//
// The peer here writes blocking, on purpose: that is what makes both sides wedge
// against each other, and it is the shape a straightforward peer has.

int saturating_ping_flood() {
    uint16_t port = 0;
    const int listen_fd = listener(port);
    if (listen_fd < 0) {
        std::fprintf(stderr, "no loopback listener\n");
        return kFail;
    }

    const pid_t child = ::fork();
    if (child == 0) {
        ::close(listen_fd);
        vocem::WebSocket socket;
        if (!socket.connect("127.0.0.1", port, "/", "http://localhost")) {
            ::_exit(kFail);
        }
        std::string out;
        const auto start = std::chrono::steady_clock::now();
        const vocem::WebSocket::Result result = socket.recv(out, 1000);
        const double elapsed = milliseconds_since(start);
        // Closed is as acceptable an answer as Timeout -- the point is that it
        // comes back at all, and inside the second it was given plus slack.
        if (elapsed > 2000.0) {
            std::fprintf(stderr, "  recv returned %d after %.0f ms\n",
                         static_cast<int>(result), elapsed);
            ::_exit(kFail);
        }
        ::_exit(0);
    }

    const int server = ::accept(listen_fd, nullptr, nullptr);
    ::close(listen_fd);
    if (server < 0 || !accept_upgrade(server)) {
        ::kill(child, SIGKILL);
        ::waitpid(child, nullptr, 0);
        std::fprintf(stderr, "the upgrade did not complete\n");
        return kFail;
    }

    // As fast as the socket takes them, for as long as it takes them. send_all
    // stops on the first refusal, which is what the child going away looks like.
    const std::vector<uint8_t> ping = frame(true, 0x9, "ping");
    const auto start = std::chrono::steady_clock::now();
    while (milliseconds_since(start) < 8000.0) {
        if (!send_all(server, ping)) {
            break;
        }
    }
    ::close(server);

    const int status = reap(child, 1000);
    if (status == -1) {
        std::fprintf(stderr,
                     "FAIL recv never came back under a ping flood at wire speed: the deadline "
                     "bounds the waiting and not the work, so the daemon's tick starves and the "
                     "stop flag is never read again\n");
        return kFail;
    }
    if (status != 0) {
        std::fprintf(stderr, "FAIL recv came back, but well past the deadline it was given\n");
        return kFail;
    }
    std::fprintf(stderr, "  saturating ping flood: recv honoured its deadline\n");
    return 0;
}

// ---------------------------------------------------------------------------
// 3. Control frames that break RFC 6455 section 5.5.

int oversized_control(bool fin, const char* what) {
    uint16_t port = 0;
    const int listen_fd = listener(port);
    if (listen_fd < 0) {
        std::fprintf(stderr, "no loopback listener\n");
        return kFail;
    }

    const pid_t child = ::fork();
    if (child == 0) {
        ::close(listen_fd);
        vocem::WebSocket socket;
        if (!socket.connect("127.0.0.1", port, "/", "http://localhost")) {
            ::_exit(kFail);
        }
        std::string out;
        const vocem::WebSocket::Result result = socket.recv(out, 1500);
        if (result != vocem::WebSocket::Result::Closed) {
            std::fprintf(stderr, "  recv returned %d, not Closed\n", static_cast<int>(result));
            ::_exit(kFail);
        }
        ::_exit(0);
    }

    const int server = ::accept(listen_fd, nullptr, nullptr);
    ::close(listen_fd);
    if (server < 0 || !accept_upgrade(server)) {
        ::kill(child, SIGKILL);
        ::waitpid(child, nullptr, 0);
        std::fprintf(stderr, "the upgrade did not complete\n");
        return kFail;
    }

    // 200 bytes when a ping may carry 125, or a ping with FIN clear.
    send_all(server, frame(fin, 0x9, std::string(fin ? 200 : 4, 'p')));
    const int status = reap(child, 4000);
    ::close(server);

    if (status != 0) {
        std::fprintf(stderr,
                     "FAIL %s was accepted and answered instead of ending the connection\n", what);
        return kFail;
    }
    std::fprintf(stderr, "  %s: connection closed\n", what);
    return 0;
}

// ---------------------------------------------------------------------------
// 4. A frame body that arrives one byte at a time, for ever.

int slow_frame_body() {
    uint16_t port = 0;
    const int listen_fd = listener(port);
    if (listen_fd < 0) {
        std::fprintf(stderr, "no loopback listener\n");
        return kFail;
    }

    const pid_t child = ::fork();
    if (child == 0) {
        ::close(listen_fd);
        vocem::WebSocket socket;
        if (!socket.connect("127.0.0.1", port, "/", "http://localhost")) {
            ::_exit(kFail);
        }
        std::string out;
        const auto start = std::chrono::steady_clock::now();
        const vocem::WebSocket::Result result = socket.recv(out, 1000);
        const double elapsed = milliseconds_since(start);
        // The frame gets its own clock -- five seconds -- but one clock for the
        // whole of it, not five seconds per byte.
        if (result != vocem::WebSocket::Result::Closed || elapsed > 8000.0) {
            std::fprintf(stderr, "  recv returned %d after %.0f ms\n",
                         static_cast<int>(result), elapsed);
            ::_exit(kFail);
        }
        ::_exit(0);
    }

    const int server = ::accept(listen_fd, nullptr, nullptr);
    ::close(listen_fd);
    if (server < 0 || !accept_upgrade(server)) {
        ::kill(child, SIGKILL);
        ::waitpid(child, nullptr, 0);
        std::fprintf(stderr, "the upgrade did not complete\n");
        return kFail;
    }

    // A text frame announcing 65535 bytes, and then a trickle.
    const uint8_t header[4] = {0x81, 126, 0xFF, 0xFF};
    ::send(server, header, sizeof(header), MSG_NOSIGNAL);
    const auto start = std::chrono::steady_clock::now();
    while (milliseconds_since(start) < 20000.0) {
        const char byte = 'x';
        if (::send(server, &byte, 1, MSG_NOSIGNAL) <= 0) {
            break;  // the client gave up, which is the point
        }
        ::usleep(500'000);
    }
    ::close(server);

    const int status = reap(child, 2000);
    if (status != 0) {
        std::fprintf(stderr,
                     "FAIL a frame body arriving one byte every 500 ms has no deadline: the "
                     "poll timeout is re-armed per chunk, so the peer holds recv open for as "
                     "long as it likes\n");
        return kFail;
    }
    std::fprintf(stderr, "  a trickled frame body: the frame's deadline ends it\n");
    return 0;
}

}  // namespace

int main() {
    // A peer that hangs up mid-write must not kill the test.
    ::signal(SIGPIPE, SIG_IGN);

    int failures = 0;
    failures += unterminated_message() != 0;
    failures += ping_flood() != 0;
    failures += saturating_ping_flood() != 0;
    failures += oversized_control(true, "a 200-byte ping") != 0;
    failures += oversized_control(false, "a ping with FIN clear") != 0;
    failures += slow_frame_body() != 0;

    if (failures != 0) {
        std::fprintf(stderr, "%d of 6 hostile-peer cases failed\n", failures);
        return kFail;
    }
    std::fprintf(stderr, "the client bounds the message, honours its deadline, and refuses "
                         "malformed control frames\n");
    return 0;
}

// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A Discord RPC that is not Discord: a WebSocket server speaking just enough
// RFC 6455 for this project's own client, for the tests that drive the real
// vocemd. Frames from the daemon are masked, as the RFC requires of a client;
// replies go out unmasked, as it requires of a server.
//
// daemon_notification, daemon_moved, daemon_note_expiry and daemon_reconnect
// each carry a hand-written copy of this; new tests take it from here, and the
// four should follow when they are next touched (the fifteen-copies lesson of
// entry 124, one file over).

#ifndef VOCEM_TESTS_DISCORD_STUB_H
#define VOCEM_TESTS_DISCORD_STUB_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string>

namespace vocem_test {

inline double monotonic() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

inline bool write_all(int fd, const void* data, size_t length) {
    const char* p = static_cast<const char*>(data);
    while (length > 0) {
        const ssize_t n = ::write(fd, p, length);
        if (n <= 0) {
            return false;
        }
        p += n;
        length -= static_cast<size_t>(n);
    }
    return true;
}

// A listening socket on 127.0.0.1:port. SOCK_CLOEXEC, so a daemon started by
// fork+exec afterwards does not inherit it and keep the port answering after
// the test closes it -- daemon_note_expiry's header tells what that cost.
inline int listen_on(uint16_t port) {
    const int listener = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (listener < 0) {
        return -1;
    }
    const int one = 1;
    ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0 ||
        ::listen(listener, 4) != 0) {
        ::close(listener);
        return -1;
    }
    return listener;
}

// The server half of the upgrade: read to the blank line, answer 101.
inline bool accept_upgrade(int fd) {
    std::string request;
    char c = 0;
    while (request.find("\r\n\r\n") == std::string::npos) {
        if (request.size() > 8192 || ::read(fd, &c, 1) != 1) {
            return false;
        }
        request.push_back(c);
    }
    static const char* reply =
        "HTTP/1.1 101 Switching Protocols\r\n"
        "Upgrade: websocket\r\nConnection: Upgrade\r\n"
        "Sec-WebSocket-Accept: stub\r\n\r\n";
    return write_all(fd, reply, std::strlen(reply));
}

inline bool send_text(int fd, const std::string& payload) {
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

// One text message from the daemon, unmasked into `out`, by `deadline`
// (monotonic seconds). `buffer` carries what was read past the message.
inline bool recv_text(int fd, std::string& buffer, std::string& out, double deadline) {
    for (;;) {
        if (buffer.size() >= 2) {
            const uint8_t b0 = static_cast<uint8_t>(buffer[0]);
            const uint8_t b1 = static_cast<uint8_t>(buffer[1]);
            const uint8_t opcode = b0 & 0x0F;
            const bool masked = (b1 & 0x80) != 0;
            size_t length = b1 & 0x7F;
            size_t offset = 2;
            bool need_more = false;
            if (length == 126) {
                if (buffer.size() < 4) {
                    need_more = true;
                } else {
                    length = (static_cast<size_t>(static_cast<uint8_t>(buffer[2])) << 8) |
                             static_cast<uint8_t>(buffer[3]);
                    offset = 4;
                }
            } else if (length == 127) {
                return false;  // nothing here is remotely that large
            }
            if (!need_more) {
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
                        return false;  // close
                    }
                    continue;
                }
            }
        }
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

// READY, then the AUTHENTICATE reply, then answers GET_SELECTED_VOICE_CHANNEL
// with `channel_reply`; returns once that question was answered, with the
// connection idle. False when the daemon never got that far by the deadline.
inline bool bring_up_session(int fd, std::string& buffer, const std::string& channel_reply,
                             double deadline) {
    if (!send_text(fd, R"({"cmd":"DISPATCH","evt":"READY","data":{"v":1},"nonce":null})")) {
        return false;
    }
    std::string message;
    while (recv_text(fd, buffer, message, deadline)) {
        if (message.find("\"cmd\":\"AUTHENTICATE\"") != std::string::npos) {
            send_text(fd,
                      R"({"cmd":"AUTHENTICATE","evt":null,"nonce":"auth",)"
                      R"("data":{"user":{"id":"99","username":"stub"}}})");
        } else if (message.find("\"cmd\":\"GET_SELECTED_VOICE_CHANNEL\"") != std::string::npos) {
            send_text(fd, channel_reply);
            return true;
        }
    }
    return false;
}

}  // namespace vocem_test

#endif  // VOCEM_TESTS_DISCORD_STUB_H

// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.

#include "websocket.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <random>
#include <vector>

namespace vocem {
namespace {

constexpr uint8_t kOpContinuation = 0x0;
constexpr uint8_t kOpText = 0x1;
constexpr uint8_t kOpClose = 0x8;
constexpr uint8_t kOpPing = 0x9;
constexpr uint8_t kOpPong = 0xA;

// One frame, and one whole message, may be this large. The frame cap was always
// here; the message cap was not, and a peer that never set FIN grew the
// reassembly buffer without limit -- 256 MiB measured in tests/daemon_ws_bounds.
constexpr uint64_t kMaxFrame = 8u << 20;
constexpr size_t kMaxMessage = 8u << 20;

// RFC 6455 section 5.5: a control frame carries at most 125 bytes and is never
// fragmented.
constexpr uint64_t kMaxControlPayload = 125;

// Once a frame's header has been seen, its body is read on its own clock rather
// than on the caller's: a message arriving in the last millisecond of a one-second
// recv must not be mistaken for a broken connection.
constexpr int kFrameTimeoutMs = 5000;

// The handshake as a whole, not per byte. The header loop reads one byte at a
// time, so a per-byte timeout let a silent peer hold the connection for hours.
constexpr int kHandshakeTimeoutMs = 10000;

using Clock = std::chrono::steady_clock;

// Milliseconds left before `deadline`, never negative -- poll() reads a negative
// timeout as "wait forever", which is the opposite of what a deadline means.
int remaining_ms(Clock::time_point deadline) {
    const auto left =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    return left > 0 ? static_cast<int>(left) : 0;
}

std::string base64(const uint8_t* data, size_t length) {
    static const char* table =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((length + 2) / 3) * 4);
    for (size_t i = 0; i < length; i += 3) {
        uint32_t chunk = static_cast<uint32_t>(data[i]) << 16;
        if (i + 1 < length) chunk |= static_cast<uint32_t>(data[i + 1]) << 8;
        if (i + 2 < length) chunk |= data[i + 2];

        out.push_back(table[(chunk >> 18) & 0x3F]);
        out.push_back(table[(chunk >> 12) & 0x3F]);
        out.push_back(i + 1 < length ? table[(chunk >> 6) & 0x3F] : '=');
        out.push_back(i + 2 < length ? table[chunk & 0x3F] : '=');
    }
    return out;
}

std::mt19937& rng() {
    static std::mt19937 engine{std::random_device{}()};
    return engine;
}

}  // namespace

WebSocket::~WebSocket() { close(); }

void WebSocket::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    pending_.clear();
}

bool WebSocket::write_all(const void* data, size_t length) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    size_t written = 0;
    // A peer that stops reading fills our send buffer, and a blocking send then
    // waits in the kernel with no way out: the daemon would sit there through its
    // own SIGTERM. Wait on the descriptor instead, against a deadline.
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(kFrameTimeoutMs);
    while (written < length) {
        struct pollfd pfd{fd_, POLLOUT, 0};
        const int ready = ::poll(&pfd, 1, remaining_ms(deadline));
        if (ready == 0) {
            return false;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        ssize_t n = ::send(fd_, bytes + written, length - written, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
                continue;
            }
            return false;
        }
        written += static_cast<size_t>(n);
    }
    return true;
}

bool WebSocket::read_exact(void* dest, size_t length, Clock::time_point by) {
    auto* bytes = static_cast<uint8_t*>(dest);
    size_t read_total = 0;
    while (read_total < length) {
        struct pollfd pfd{fd_, POLLIN, 0};
        // The deadline is for the whole read, not for each chunk of it. Per chunk,
        // a peer that announced a 64 KiB frame and then sent one byte every two
        // seconds stayed inside a five-second window for ever -- measured: recv
        // had not returned after thirty seconds. That is the same defect the
        // handshake loop below was fixed for, one function away.
        int ready = ::poll(&pfd, 1, remaining_ms(by));
        if (ready == 0) {
            return false;  // timeout mid-frame: treated as failure by the caller
        }
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        ssize_t n = ::recv(fd_, bytes + read_total, length - read_total, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) {
                continue;
            }
            return false;
        }
        read_total += static_cast<size_t>(n);
    }
    return true;
}

bool WebSocket::connect(const char* host, uint16_t port, const std::string& path,
                        const std::string& origin) {
    close();

    fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd_ < 0) {
        return false;
    }

    int one = 1;
    ::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
        close();
        return false;
    }
    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        close();
        return false;
    }

    uint8_t nonce[16];
    for (uint8_t& byte : nonce) {
        byte = static_cast<uint8_t>(rng()() & 0xFF);
    }

    std::string request;
    request += "GET " + path + " HTTP/1.1\r\n";
    request += std::string("Host: ") + host + ":" + std::to_string(port) + "\r\n";
    request += "Upgrade: websocket\r\n";
    request += "Connection: Upgrade\r\n";
    request += "Sec-WebSocket-Key: " + base64(nonce, sizeof(nonce)) + "\r\n";
    request += "Sec-WebSocket-Version: 13\r\n";
    // Discord rejects the handshake without an allowed origin.
    request += "Origin: " + origin + "\r\n";
    request += "\r\n";

    if (!write_all(request.data(), request.size())) {
        close();
        return false;
    }

    // Read headers one byte at a time up to the blank line. Wasteful in general,
    // trivial here, and it guarantees we never consume part of the first frame.
    // The deadline is on the whole handshake: per byte, a peer trickling one
    // character every four seconds held the connection open for hours.
    const Clock::time_point handshake_by =
        Clock::now() + std::chrono::milliseconds(kHandshakeTimeoutMs);
    std::string response;
    char c = 0;
    while (response.find("\r\n\r\n") == std::string::npos) {
        if (response.size() > 8192 || remaining_ms(handshake_by) == 0 ||
            !read_exact(&c, 1, handshake_by)) {
            close();
            return false;
        }
        response.push_back(c);
    }

    if (response.compare(0, 12, "HTTP/1.1 101") != 0) {
        close();
        return false;
    }
    return true;
}

PeerIdentity WebSocket::peer_owner() const {
    if (fd_ < 0) {
        return {};
    }
    sockaddr_in ours{};
    sockaddr_in theirs{};
    socklen_t length = sizeof(ours);
    if (::getsockname(fd_, reinterpret_cast<sockaddr*>(&ours), &length) != 0) {
        return {};
    }
    length = sizeof(theirs);
    if (::getpeername(fd_, reinterpret_cast<sockaddr*>(&theirs), &length) != 0) {
        return {};
    }
    // The peer's row has the endpoints the other way round from ours.
    return socket_owner(theirs.sin_addr.s_addr, ntohs(theirs.sin_port), ours.sin_addr.s_addr,
                        ntohs(ours.sin_port));
}

bool WebSocket::send_frame(uint8_t opcode, const void* data, size_t length) {
    if (fd_ < 0) {
        return false;
    }

    std::vector<uint8_t> header;
    header.push_back(static_cast<uint8_t>(0x80 | opcode));  // FIN + opcode

    // Clients must always mask.
    if (length < 126) {
        header.push_back(static_cast<uint8_t>(0x80 | length));
    } else if (length <= 0xFFFF) {
        header.push_back(0x80 | 126);
        header.push_back(static_cast<uint8_t>((length >> 8) & 0xFF));
        header.push_back(static_cast<uint8_t>(length & 0xFF));
    } else {
        header.push_back(0x80 | 127);
        for (int shift = 56; shift >= 0; shift -= 8) {
            header.push_back(static_cast<uint8_t>((length >> shift) & 0xFF));
        }
    }

    uint8_t mask[4];
    for (uint8_t& byte : mask) {
        byte = static_cast<uint8_t>(rng()() & 0xFF);
    }
    header.insert(header.end(), mask, mask + 4);

    std::vector<uint8_t> payload(length);
    const auto* src = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < length; ++i) {
        payload[i] = static_cast<uint8_t>(src[i] ^ mask[i & 3]);
    }

    return write_all(header.data(), header.size()) &&
           (length == 0 || write_all(payload.data(), payload.size()));
}

bool WebSocket::send_text(const std::string& payload) {
    return send_frame(kOpText, payload.data(), payload.size());
}

WebSocket::Result WebSocket::recv(std::string& out, int timeout_ms) {
    if (fd_ < 0) {
        return Result::Closed;
    }

    // The caller's timeout is a deadline for the whole call, not for each frame.
    // It used to be re-armed after every ping and pong, so a peer pinging faster
    // than the timeout kept this function from ever returning -- and everything
    // the daemon does once per tick lives after this call: expiring the note
    // segment, reconciling the channel, and reading the stop flag.
    const Clock::time_point deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);

    for (;;) {
        struct pollfd pfd{fd_, POLLIN, 0};
        int ready = ::poll(&pfd, 1, remaining_ms(deadline));
        if (ready == 0) {
            return Result::Timeout;
        }
        if (ready < 0) {
            if (errno == EINTR) {
                return Result::Timeout;
            }
            return Result::Closed;
        }

        // One deadline for the whole frame, taken when its first byte is in hand.
        const Clock::time_point frame_by =
            Clock::now() + std::chrono::milliseconds(kFrameTimeoutMs);

        uint8_t head[2];
        if (!read_exact(head, 2, frame_by)) {
            return Result::Closed;
        }

        const bool fin = (head[0] & 0x80) != 0;
        const uint8_t opcode = head[0] & 0x0F;
        const bool masked = (head[1] & 0x80) != 0;
        uint64_t length = head[1] & 0x7F;

        if (length == 126) {
            uint8_t ext[2];
            if (!read_exact(ext, 2, frame_by)) return Result::Closed;
            length = (static_cast<uint64_t>(ext[0]) << 8) | ext[1];
        } else if (length == 127) {
            uint8_t ext[8];
            if (!read_exact(ext, 8, frame_by)) return Result::Closed;
            length = 0;
            for (uint8_t byte : ext) {
                length = (length << 8) | byte;
            }
        }

        // Guard against a hostile or broken peer claiming an enormous frame, and
        // hold control frames to RFC 6455 section 5.5: at most 125 bytes, never
        // fragmented. Checked here, before the payload is allocated, so an 8 MiB
        // "ping" costs nothing and is never echoed back as an 8 MiB pong.
        const bool control = (opcode & 0x08) != 0;
        if (length > kMaxFrame || (control && (!fin || length > kMaxControlPayload))) {
            return Result::Closed;
        }

        // A server must not mask, but handle it rather than desynchronise.
        uint8_t mask[4] = {0, 0, 0, 0};
        if (masked && !read_exact(mask, 4, frame_by)) {
            return Result::Closed;
        }

        std::string payload;
        payload.resize(static_cast<size_t>(length));
        if (length && !read_exact(payload.data(), payload.size(), frame_by)) {
            return Result::Closed;
        }
        if (masked) {
            for (size_t i = 0; i < payload.size(); ++i) {
                payload[i] = static_cast<char>(static_cast<uint8_t>(payload[i]) ^ mask[i & 3]);
            }
        }

        switch (opcode) {
            case kOpPing:
                send_frame(kOpPong, payload.data(), payload.size());
                continue;
            case kOpPong:
                continue;
            case kOpClose:
                send_frame(kOpClose, nullptr, 0);
                return Result::Closed;
            case kOpText:
            case kOpContinuation:
                // A message is the concatenation of its frames, so capping the
                // frame capped nothing: a peer that never sets FIN can send 64 KiB
                // at a time forever. The whole message gets the frame's ceiling.
                if (pending_.size() + payload.size() > kMaxMessage) {
                    return Result::Closed;
                }
                pending_ += payload;
                if (fin) {
                    out = std::move(pending_);
                    pending_.clear();
                    return Result::Message;
                }
                continue;
            default:
                continue;  // binary and reserved opcodes are not used by the RPC
        }
    }
}

}  // namespace vocem

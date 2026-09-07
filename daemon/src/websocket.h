// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A minimal RFC 6455 client, just enough for Discord's local RPC.
//
// Discord exposes its RPC as a plain WebSocket on 127.0.0.1, which means no TLS
// and no proxy handling: a dependency-free implementation is a few hundred lines
// and removes an entire third-party library from the build. It is deliberately
// not a general-purpose client -- it speaks only what this one endpoint needs.

#ifndef VOCEM_WEBSOCKET_H
#define VOCEM_WEBSOCKET_H

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>

#include "peer_identity.h"

namespace vocem {

class WebSocket {
public:
    enum class Result {
        Message,  // a complete text message is in `out`
        Timeout,  // nothing arrived within the deadline
        Closed,   // peer closed, or the connection broke
    };

    ~WebSocket();

    WebSocket() = default;
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    // Connects and performs the HTTP upgrade. `path` includes the query string.
    bool connect(const char* host, uint16_t port, const std::string& path,
                 const std::string& origin);

    bool send_text(const std::string& payload);

    Result recv(std::string& out, int timeout_ms);

    // Who owns the other end. The daemon refuses to authenticate to a peer it can
    // identify as somebody else (see peer_identity.h): the token it would send
    // carries messages.read scope. An answer of Unknown is not a refusal -- see
    // that header for why.
    PeerIdentity peer_owner() const;

    void close();


private:
    // Reads against an absolute deadline. A per-call timeout was not enough:
    // poll() re-armed it on every chunk, so a peer sending one byte every two
    // seconds held a frame open for as long as it liked.
    bool read_exact(void* dest, size_t length, std::chrono::steady_clock::time_point by);
    // Writes against an absolute deadline, for the same reason reads have one:
    // a peer that stops reading fills our send buffer and a blocking send waits
    // in the kernel with no way out. The deadline is passed in rather than
    // started here, because a reply sent from inside recv() may not outlive the
    // deadline recv() was given (see the pong in recv()).
    bool write_all(const void* data, size_t length, std::chrono::steady_clock::time_point by);
    bool send_frame(uint8_t opcode, const void* data, size_t length,
                    std::chrono::steady_clock::time_point by);

    int fd_ = -1;
    std::string pending_;  // accumulates fragmented messages
    // Whether `pending_` is the start of a TEXT message. A binary message's
    // continuations are not appended to it (recv() says why).
    bool pending_is_text_ = false;
};

}  // namespace vocem

#endif  // VOCEM_WEBSOCKET_H

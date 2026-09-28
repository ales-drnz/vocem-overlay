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
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <string>

#include "peer_cgroup.h"
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

    // The cgroup the other end's socket was created in, asked of the kernel
    // (peer_cgroup.h): the one answer about the peer that the daemon's own
    // unit can still get, since no process of the session is open to it.
    PeerCgroup peer_cgroup() const;

    void close();

    // A flag this client reads while it waits: the daemon's SIGTERM handler
    // sets it, the connect's and the handshake's waits are taken in slices, and
    // every read and write loop reads it on each turn, so that a stop is
    // noticed rather than waited out -- by a silent peer or by one trickling a
    // byte at a time (entry 196). recv()'s own wait is bounded by its caller's
    // one second instead. Without it the handshake's own deadline
    // was honoured to the letter -- ten seconds of it, inside a stop the unit
    // gives ten (websocket.cpp, kStopSliceMs). Optional: the bounds tests
    // construct this client without one.
    void watch_stop(const volatile std::sig_atomic_t* stop) { stop_ = stop; }


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

    bool stopping() const;
    // The next poll() timeout: the deadline, in slices while a stop flag is
    // watched.
    int wait_ms(std::chrono::steady_clock::time_point deadline) const;

    int fd_ = -1;
    const volatile std::sig_atomic_t* stop_ = nullptr;
    std::string pending_;  // accumulates fragmented messages
    // Whether `pending_` is the start of a TEXT message. A binary message's
    // continuations are not appended to it (recv() says why).
    bool pending_is_text_ = false;
};

}  // namespace vocem

#endif  // VOCEM_WEBSOCKET_H

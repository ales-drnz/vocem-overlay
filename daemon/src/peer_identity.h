// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Who owns the other end of a loopback connection.
//
// The daemon connects to whatever answers on 127.0.0.1 in Discord's RPC port
// range and, the moment that peer says READY, sends it the stored OAuth token --
// whose scopes include messages.read. Nothing about the peer was ever checked, so
// any process that bound the port before Discord did was handed the credential:
// another user on the machine, or a sandboxed process of the user's own that can
// bind loopback but cannot read $XDG_STATE_HOME. The project's own
// tests/daemon_notification.cpp is a working demonstration -- a stub that does
// nothing but listen gets AUTHENTICATE with the real token.
//
// /proc/net/tcp lists every TCP socket with the uid that owns it. The peer's row
// is the one whose local endpoint is our remote and whose remote endpoint is our
// local, which is exact: a connected pair of endpoints identifies one socket.
// This does not defend against a process running as the same user -- nothing can,
// and Discord's own client is one -- but it closes the cross-user and
// cross-sandbox case.
//
// Two things this deliberately does NOT do, because a daemon that refuses to work
// is worse than one that is not confined:
//
//   * it looks in /proc/net/tcp6 as well. A server socket bound dual-stack
//     accepts our IPv4 connection into a socket the kernel lists there, as
//     ::ffff:127.0.0.1 -- so looking only at /proc/net/tcp would have refused a
//     perfectly ordinary Discord client;
//   * "cannot tell" is not "hostile". Where neither file can be read -- a
//     container with /proc restricted -- the caller is told the answer is unknown
//     and says so out loud, rather than treating every peer as an impostor.

#ifndef VOCEM_PEER_IDENTITY_H
#define VOCEM_PEER_IDENTITY_H

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

namespace vocem {

// What a lookup can conclude.
enum class PeerOwner {
    Unknown,  // neither /proc/net file could be read, or no row matched
    Found,    // `uid` is the owner of the socket at the other end
};

struct PeerIdentity {
    PeerOwner outcome = PeerOwner::Unknown;
    long uid = -1;
};

namespace detail {

// The address column, as /proc prints it: a hexadecimal word per 32 bits, each in
// the host's byte order, so an IPv4 address on x86-64 reads 0100007F for
// 127.0.0.1. An IPv4-mapped address in /proc/net/tcp6 ends with the same eight
// characters, which is all the comparison needs.
inline bool address_matches(const char* column, uint32_t address) {
    char wanted[9];
    std::snprintf(wanted, sizeof(wanted), "%08X", address);
    const size_t length = std::strlen(column);
    if (length < 8) {
        return false;
    }
    return std::strncmp(column + length - 8, wanted, 8) == 0;
}

inline bool scan_one(const char* path, uint32_t local_address, uint16_t local_port,
                     uint32_t remote_address, uint16_t remote_port, PeerIdentity& out) {
    std::FILE* file = std::fopen(path, "r");
    if (!file) {
        return false;  // not readable here: not an answer, and not a refusal
    }
    char line[512];
    if (!std::fgets(line, sizeof(line), file)) {  // the header row
        std::fclose(file);
        return false;
    }
    while (std::fgets(line, sizeof(line), file)) {
        char local[64] = {0};
        char remote[64] = {0};
        unsigned int row_local_port = 0, row_remote_port = 0, state = 0;
        unsigned long tx = 0, rx = 0;
        unsigned int tr = 0;
        unsigned long when = 0;
        unsigned int retransmit = 0;
        long uid = -1;
        // sl: local:port remote:port st tx:rx tr:when retrnsmt uid
        if (std::sscanf(line, "%*d: %63[0-9A-Fa-f]:%4X %63[0-9A-Fa-f]:%4X %2X %8lX:%8lX %2X:%8lX %8X %ld",
                        local, &row_local_port, remote, &row_remote_port, &state, &tx, &rx, &tr,
                        &when, &retransmit, &uid) != 11) {
            continue;
        }
        if (row_local_port == local_port && row_remote_port == remote_port &&
            address_matches(local, local_address) && address_matches(remote, remote_address)) {
            out.outcome = PeerOwner::Found;
            out.uid = uid;
            std::fclose(file);
            return true;
        }
    }
    std::fclose(file);
    return false;
}

}  // namespace detail

// The owner of the socket with these endpoints. Addresses are the 32-bit values
// as they sit in a sockaddr_in, ports in host order.
inline PeerIdentity socket_owner(uint32_t local_address, uint16_t local_port,
                                 uint32_t remote_address, uint16_t remote_port,
                                 const char* tcp_path = "/proc/net/tcp",
                                 const char* tcp6_path = "/proc/net/tcp6") {
    PeerIdentity identity;
    if (detail::scan_one(tcp_path, local_address, local_port, remote_address, remote_port,
                         identity)) {
        return identity;
    }
    if (tcp6_path) {
        detail::scan_one(tcp6_path, local_address, local_port, remote_address, remote_port,
                         identity);
    }
    return identity;
}

}  // namespace vocem

#endif  // VOCEM_PEER_IDENTITY_H

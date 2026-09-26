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
// That closes the cross-user case and nothing more. A sandbox runs as the same
// uid -- measured by the review of 2026-09-26: a listener under
// `bwrap --unshare-user --unshare-pid`, network shared, read outcome=Found
// uid=1000, and this header's claim that the uid "closes the cross-sandbox case"
// was false for as long as it stood.
//
// So the row's inode is taken too, and socket_process() below finds the process
// that holds it (a `socket:[inode]` link under /proc/<pid>/fd) and reads the
// `/.flatpak-info` Flatpak puts at the root of every sandbox, which the
// application inside cannot change. A peer in a Flatpak sandbox that is not one
// of Discord's own application ids is refused by the caller. Measured on this
// machine with yama ptrace_scope=1: /proc/<pid>/fd and /proc/<pid>/root of a
// process under `bwrap --unshare-user --unshare-pid` are readable from the host,
// because both are PTRACE_MODE_READ, which Yama does not restrict, and the uid
// is the same.
//
// This still does not defend against a process running as the same user
// outside any sandbox -- nothing can, and Discord's own client is one.
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
//     and says so out loud, rather than treating every peer as an impostor. The
//     same holds for the process: when no process holding the socket can be
//     found, or its root cannot be read, the caller says so and goes on.

#ifndef VOCEM_PEER_IDENTITY_H
#define VOCEM_PEER_IDENTITY_H

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
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
    // The socket's inode, from the same row; 0 when the row did not carry one.
    unsigned long inode = 0;
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
        long timeout = 0;
        unsigned long inode = 0;
        // sl: local:port remote:port st tx:rx tr:when retrnsmt uid timeout inode
        const int fields = std::sscanf(
            line, "%*d: %63[0-9A-Fa-f]:%4X %63[0-9A-Fa-f]:%4X %2X %8lX:%8lX %2X:%8lX %8X %ld %ld %lu",
            local, &row_local_port, remote, &row_remote_port, &state, &tx, &rx, &tr, &when,
            &retransmit, &uid, &timeout, &inode);
        if (fields < 11) {
            continue;
        }
        if (row_local_port == local_port && row_remote_port == remote_port &&
            address_matches(local, local_address) && address_matches(remote, remote_address)) {
            out.outcome = PeerOwner::Found;
            out.uid = uid;
            out.inode = fields >= 13 ? inode : 0;
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

// Discord's own Flatpak application ids: the Flathub build and the Canary one
// Flathub's beta repository carries (`flatpak search` on this machine lists
// com.discordapp.Discord and no PTB). A client built on arRPC (Vesktop and the
// like) answers presence on this port and cannot authorise, so it has no use
// for the token either.
inline bool is_discord_flatpak(const std::string& id) {
    return id == "com.discordapp.Discord" || id == "com.discordapp.DiscordCanary";
}

// What holds the other end of the connection.
enum class PeerPlace {
    Unknown,  // no process holding the socket could be found, or read
    Host,     // a process with no /.flatpak-info at its root
    Flatpak,  // a process in a Flatpak sandbox; `app_id` says whose
};

struct PeerProcess {
    PeerPlace place = PeerPlace::Unknown;
    long pid = -1;
    std::string app_id;
};

namespace detail {

// The `[Application] name=` of the /.flatpak-info at this process's root. 1 with
// the id, 0 when there is no such file (not a Flatpak), -1 when it cannot be
// read. The root is the sandbox's own, so the open takes nothing on trust: no
// following a link (an absolute one would resolve against OUR root, and a
// sandbox could point it at an installed Discord's metadata), no waiting on a
// FIFO, regular files of a size such a file has.
inline int flatpak_info_name(const std::string& proc, long pid, std::string& id) {
    const std::string path = proc + "/" + std::to_string(pid) + "/root/.flatpak-info";
    const int fd = ::open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return errno == ENOENT ? 0 : -1;
    }
    struct stat info {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size > 64 * 1024) {
        ::close(fd);
        return -1;
    }
    std::FILE* file = ::fdopen(fd, "r");
    if (!file) {
        ::close(fd);
        return -1;
    }
    char line[512];
    bool in_application = false;
    id.clear();
    while (std::fgets(line, sizeof(line), file)) {
        if (line[0] == '[') {
            in_application = std::strncmp(line, "[Application]", 13) == 0;
            continue;
        }
        if (in_application && std::strncmp(line, "name=", 5) == 0) {
            id.assign(line + 5, std::strcspn(line + 5, "\r\n"));
            break;
        }
    }
    std::fclose(file);
    return 1;
}

inline bool all_digits(const char* name) {
    if (!*name) {
        return false;
    }
    for (const char* c = name; *c; ++c) {
        if (*c < '0' || *c > '9') {
            return false;
        }
    }
    return true;
}

}  // namespace detail

// The process that holds the socket with this inode, and where it runs.
//
// Every process's descriptors are walked once -- a readlink per descriptor,
// only for processes whose /proc entry belongs to this user (anybody else's
// fd directory is closed to us anyway) -- and the socket may be held by more
// than one of them after a fork. The answer is the most suspicious holder: a
// Flatpak that is not Discord's over a Discord one, and either over a host
// process, because the question is whether the token may go there. Once per
// connection, never per message.
inline PeerProcess socket_process(unsigned long inode, const char* proc = "/proc") {
    PeerProcess found;
    if (inode == 0) {
        return found;
    }
    char wanted[64];
    std::snprintf(wanted, sizeof(wanted), "socket:[%lu]", inode);
    const size_t wanted_length = std::strlen(wanted);
    const std::string root = proc;
    const uid_t self_uid = ::getuid();
    const long self_pid = static_cast<long>(::getpid());

    DIR* processes = ::opendir(proc);
    if (!processes) {
        return found;
    }
    int rank = -1;  // 0 host, 1 unreadable, 2 Discord's Flatpak, 3 any other Flatpak
    while (const dirent* entry = ::readdir(processes)) {
        if (!detail::all_digits(entry->d_name)) {
            continue;
        }
        const long pid = std::strtol(entry->d_name, nullptr, 10);
        if (pid == self_pid) {
            continue;
        }
        const std::string base = root + "/" + entry->d_name;
        struct stat owner {};
        if (::stat(base.c_str(), &owner) != 0 || owner.st_uid != self_uid) {
            continue;
        }
        const std::string fd_path = base + "/fd";
        DIR* descriptors = ::opendir(fd_path.c_str());
        if (!descriptors) {
            continue;
        }
        const int fd_dir = ::dirfd(descriptors);
        bool holds = false;
        while (const dirent* fd_entry = ::readdir(descriptors)) {
            if (fd_entry->d_name[0] == '.') {
                continue;
            }
            char target[64];
            const ssize_t length = ::readlinkat(fd_dir, fd_entry->d_name, target, sizeof(target));
            if (length == static_cast<ssize_t>(wanted_length) &&
                std::memcmp(target, wanted, wanted_length) == 0) {
                holds = true;
                break;
            }
        }
        ::closedir(descriptors);
        if (!holds) {
            continue;
        }
        std::string id;
        const int info = detail::flatpak_info_name(root, pid, id);
        const int this_rank = info == 0 ? 0 : info < 0 ? 1 : is_discord_flatpak(id) ? 2 : 3;
        if (this_rank > rank) {
            rank = this_rank;
            found.pid = pid;
            found.place = info == 0   ? PeerPlace::Host
                          : info < 0  ? PeerPlace::Unknown
                                      : PeerPlace::Flatpak;
            found.app_id = info > 0 ? id : std::string();
        }
    }
    ::closedir(processes);
    return found;
}

}  // namespace vocem

#endif  // VOCEM_PEER_IDENTITY_H

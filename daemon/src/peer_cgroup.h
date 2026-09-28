// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Which cgroup the other end of a loopback connection was created in, asked of
// the kernel's socket table and not of any process.
//
// peer_identity.h finds the process that holds the peer's socket by walking
// /proc/<pid>/fd, and that walk is a ptrace-mode read of every process. From
// the unit the daemon ships in it reads nothing of the host's: the unit's
// ProtectClock/ProtectHostname/ProtectKernel*/ProtectControlGroups each give a
// user unit a user namespace of its own, and a process in a child user
// namespace holds no CAP_SYS_PTRACE over a process in the parent one, so every
// descriptor directory of the session is closed to it -- Discord's renderer,
// which holds the RPC socket, among them. Measured on 0.1.11-1: the daemon
// refused the owner's real Discord every 30 s, while the same code run from a
// shell found the holder (NEW-hotfix-1).
//
// A socket, though, carries the cgroup of the process that created it, and
// NETLINK_SOCK_DIAG reports it (INET_DIAG_CGROUP_ID) to anyone in the socket's
// network namespace, with no access to any process: `ss -tlne` inside
// `systemd-run --user -p ProtectKernelTunables=yes` still names Discord's
// `app-discord-<n>.scope` (measured). The id is the inode number of that
// cgroup's directory under /sys/fs/cgroup, which the unit can read. And a
// Flatpak's processes live in `app-flatpak-<app-id>-<n>.scope`, which the
// sandbox cannot leave: /sys/fs/cgroup is read-only inside it (measured with
// com.rtosta.zapzap). A Flatpak holding the whole session bus could ask the
// user's systemd for a scope of any name -- and could ask Flatpak itself to
// run anything on the host, so it is no longer in a sandbox to be told
// apart from.
//
// The row the daemon asks about is the peer's end of its own connection, the
// socket accept() hands the listener, which the kernel clones from the
// listening one. Measured against the live Discord from inside the unit's
// confinement: the accepted row and the listening row both name the same
// `app-discord-<n>.scope`. tests/daemon_unit_peer.cpp holds the rest: the
// real daemon under the unit's own properties, and a listener in a scope named
// like Discord's (sent the token) and like a Flatpak's (refused).

#ifndef VOCEM_PEER_CGROUP_H
#define VOCEM_PEER_CGROUP_H

#include <dirent.h>
#include <fcntl.h>
#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <linux/sock_diag.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "flatpak_process.h"

namespace vocem {

struct PeerCgroup {
    // True when the kernel named the socket's cgroup; `id` is that cgroup's id.
    bool known = false;
    uint64_t id = 0;
    // The cgroup's path below the cgroup2 root ("/user.slice/..."), empty
    // when no directory under it carries the id -- a cgroup removed since, or
    // a tree this process cannot walk.
    std::string path;
    // When !known: which step failed and its errno, for the log.
    const char* failed = nullptr;
    int error = 0;
};

namespace detail {

// One SOCK_DIAG_BY_FAMILY exact lookup. 1 with the id, 0 when the kernel
// answered that there is no such socket, -1 on any other failure (`failed`
// and `error` say which).
inline int sock_diag_cgroup(int family, const inet_diag_sockid& id, PeerCgroup& out) {
    const int fd = ::socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_SOCK_DIAG);
    if (fd < 0) {
        out.failed = "socket(AF_NETLINK, NETLINK_SOCK_DIAG)";
        out.error = errno;
        return -1;
    }
    // The kernel answers at once or not at all; a second is generous and
    // keeps a wedged answer from holding the connect loop.
    timeval timeout{1, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    struct {
        nlmsghdr header;
        inet_diag_req_v2 request;
    } message{};
    message.header.nlmsg_len = sizeof(message);
    message.header.nlmsg_type = SOCK_DIAG_BY_FAMILY;
    message.header.nlmsg_flags = NLM_F_REQUEST;
    message.header.nlmsg_seq = 1;
    message.request.sdiag_family = static_cast<uint8_t>(family);
    message.request.sdiag_protocol = IPPROTO_TCP;
    message.request.idiag_states = ~0u;
    message.request.id = id;
    message.request.id.idiag_cookie[0] = INET_DIAG_NOCOOKIE;
    message.request.id.idiag_cookie[1] = INET_DIAG_NOCOOKIE;

    sockaddr_nl kernel{};
    kernel.nl_family = AF_NETLINK;
    if (::sendto(fd, &message, sizeof(message), 0, reinterpret_cast<sockaddr*>(&kernel),
                 sizeof(kernel)) < 0) {
        out.failed = "sendto(sock_diag)";
        out.error = errno;
        ::close(fd);
        return -1;
    }
    alignas(nlmsghdr) char buffer[8192];
    const ssize_t got = ::recv(fd, buffer, sizeof(buffer), 0);
    ::close(fd);
    if (got < 0) {
        out.failed = "recv(sock_diag)";
        out.error = errno;
        return -1;
    }
    int length = static_cast<int>(got);
    for (nlmsghdr* header = reinterpret_cast<nlmsghdr*>(buffer); NLMSG_OK(header, length);
         header = NLMSG_NEXT(header, length)) {
        if (header->nlmsg_type == NLMSG_ERROR) {
            const nlmsgerr* error = static_cast<const nlmsgerr*>(NLMSG_DATA(header));
            if (header->nlmsg_len < NLMSG_LENGTH(sizeof(nlmsgerr))) {
                break;
            }
            if (error->error == -ENOENT) {
                return 0;
            }
            out.failed = "sock_diag lookup";
            out.error = -error->error;
            return -1;
        }
        if (header->nlmsg_type != SOCK_DIAG_BY_FAMILY ||
            header->nlmsg_len < NLMSG_LENGTH(sizeof(inet_diag_msg))) {
            continue;
        }
        const inet_diag_msg* row = static_cast<const inet_diag_msg*>(NLMSG_DATA(header));
        int attributes = static_cast<int>(header->nlmsg_len - NLMSG_LENGTH(sizeof(*row)));
        for (const rtattr* attribute = reinterpret_cast<const rtattr*>(row + 1);
             RTA_OK(attribute, attributes); attribute = RTA_NEXT(attribute, attributes)) {
            if (attribute->rta_type == INET_DIAG_CGROUP_ID &&
                RTA_PAYLOAD(attribute) >= sizeof(uint64_t)) {
                std::memcpy(&out.id, RTA_DATA(attribute), sizeof(uint64_t));
                out.known = true;
                return 1;
            }
        }
        // A row without the attribute: a kernel older than 5.7, or one built
        // without cgroup socket data. Not an answer.
        out.failed = "sock_diag row without INET_DIAG_CGROUP_ID";
        out.error = 0;
        return -1;
    }
    out.failed = "sock_diag answer";
    out.error = EPROTO;
    return -1;
}

// The directory under `root` whose inode number is `id` -- a cgroup2 id is its
// kernfs node's, which is the directory's inode number on a 64-bit kernel --
// looked for below `start` (relative to `root`, "" for the root itself),
// breadth first, `budget` directories at most in all. The d_ino a readdir
// reports is enough: no stat per entry.
inline bool find_cgroup(const std::string& root, const std::string& start, uint64_t id,
                        int& budget, std::string& path) {
    std::vector<std::string> queue{start};
    for (size_t next = 0; next < queue.size() && budget > 0; ++next) {
        const std::string here = queue[next];
        DIR* directory = ::opendir((root + here).c_str());
        if (!directory) {
            continue;
        }
        --budget;
        while (const dirent* entry = ::readdir(directory)) {
            if (entry->d_type != DT_DIR || entry->d_name[0] == '.') {
                continue;
            }
            const std::string child = here + "/" + entry->d_name;
            if (static_cast<uint64_t>(entry->d_ino) == id) {
                ::closedir(directory);
                path = child;
                return true;
            }
            queue.push_back(child);
        }
        ::closedir(directory);
    }
    return false;
}

}  // namespace detail

// The cgroup of the TCP socket whose local endpoint is (local_address,
// local_port) and whose remote endpoint is (remote_address, remote_port):
// addresses as they sit in a sockaddr_in, ports in host order, the same
// convention as socket_owner(). The peer's socket, for the daemon, is the one
// whose local endpoint is the daemon's remote.
inline PeerCgroup socket_cgroup(uint32_t local_address, uint16_t local_port,
                                uint32_t remote_address, uint16_t remote_port,
                                const char* cgroup_root = "/sys/fs/cgroup") {
    PeerCgroup out;
    inet_diag_sockid id{};
    id.idiag_sport = htons(local_port);
    id.idiag_dport = htons(remote_port);
    id.idiag_src[0] = local_address;
    id.idiag_dst[0] = remote_address;
    int answer = detail::sock_diag_cgroup(AF_INET, id, out);
    if (answer == 0) {
        // A socket bound dual-stack holds the connection as an AF_INET6 one
        // with v4-mapped addresses (peer_identity.h reads /proc/net/tcp6 for
        // the same reason). The kernel resolves a mapped pair through the
        // IPv4 table either way; asked for explicitly all the same.
        inet_diag_sockid mapped{};
        mapped.idiag_sport = id.idiag_sport;
        mapped.idiag_dport = id.idiag_dport;
        mapped.idiag_src[2] = htonl(0xffff);
        mapped.idiag_src[3] = local_address;
        mapped.idiag_dst[2] = htonl(0xffff);
        mapped.idiag_dst[3] = remote_address;
        answer = detail::sock_diag_cgroup(AF_INET6, mapped, out);
    }
    if (answer == 0) {
        out.failed = "sock_diag lookup (no such socket)";
        out.error = ENOENT;
    }
    if (answer <= 0) {
        return out;
    }
    // This user's own tree first -- where every process of the session is --
    // then the rest of the hierarchy, 20000 directories in all at most (this
    // machine has about 300).
    int budget = 20000;
    const std::string root = cgroup_root;
    char user_tree[96];
    std::snprintf(user_tree, sizeof(user_tree), "/user.slice/user-%u.slice",
                  static_cast<unsigned>(::getuid()));
    if (!detail::find_cgroup(root, user_tree, out.id, budget, out.path)) {
        detail::find_cgroup(root, "", out.id, budget, out.path);
    }
    return out;
}

// The Flatpak application of a socket's cgroup path: the id in its
// `app-flatpak-<id>-<n>.scope` component, empty when it has none.
inline std::string flatpak_of_cgroup(const std::string& path) {
    return detail::flatpak_of_cgroup_path(path);
}

}  // namespace vocem

#endif  // VOCEM_PEER_CGROUP_H

// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Which Flatpak application a process of this user's is running in, asked of
// the process and not of anything the application can write.
//
// Flatpak puts `/.flatpak-info` at the root of every sandbox it builds, bound
// read-only, and its `[Application] name=` is the application id. A process
// inside cannot change it, and cannot make a process whose root carries a
// different one: Flatpak's seccomp filter refuses the new user namespace a
// second bwrap needs, and `flatpak-spawn --sandbox` builds its sub-sandbox
// under the same id. A process on the HOST can build one with any name (bwrap
// is unprivileged), which is the case nothing here defends against: such a
// process runs as the user outside any sandbox and can read everything this
// daemon holds anyway.
//
// Two askers: the peer check, which wants the sandbox of the one process
// holding a socket (peer_identity.h), and the Flatpak bridge, which wants the
// ids that have any process running at all before it believes a directory
// under $XDG_RUNTIME_DIR/app is the application its name says
// (flatpak_bridge.cpp).
//
// /proc/<pid>/root of a process of this user's is readable from the host --
// PTRACE_MODE_READ, which yama ptrace_scope=1 does not restrict -- including
// under `bwrap --unshare-user --unshare-pid` (measured). A process that is
// not dumpable, or that holds capabilities, is closed to it.

#ifndef VOCEM_DAEMON_FLATPAK_PROCESS_H
#define VOCEM_DAEMON_FLATPAK_PROCESS_H

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

namespace vocem {
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

// The application ids of every Flatpak sandbox with a process of this user's
// running in it now, as far as this process can see: one stat and one open per
// process of the user's, about a millisecond for the 147 of 489 on this
// machine. False when /proc cannot be listed at all, which leaves `ids` empty.
// A sandbox whose processes are all closed to the daemon (not dumpable) is not
// in the answer, and a caller must treat that as "not seen running".
inline bool running_flatpak_ids(std::set<std::string>& ids, const char* proc = "/proc") {
    ids.clear();
    DIR* processes = ::opendir(proc);
    if (!processes) {
        return false;
    }
    const std::string root = proc;
    const uid_t self_uid = ::getuid();
    while (const dirent* entry = ::readdir(processes)) {
        if (!detail::all_digits(entry->d_name)) {
            continue;
        }
        struct stat owner {};
        if (::stat((root + "/" + entry->d_name).c_str(), &owner) != 0 ||
            owner.st_uid != self_uid) {
            continue;
        }
        std::string id;
        if (detail::flatpak_info_name(root, std::strtol(entry->d_name, nullptr, 10), id) > 0 &&
            !id.empty()) {
            ids.insert(id);
        }
    }
    ::closedir(processes);
    return true;
}

}  // namespace vocem

#endif  // VOCEM_DAEMON_FLATPAK_PROCESS_H

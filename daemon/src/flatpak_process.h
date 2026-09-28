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
// (flatpak_policy.cpp, check_running).
//
// /proc/<pid>/root of a process of this user's is readable from a shell --
// PTRACE_MODE_READ, which yama ptrace_scope=1 does not restrict -- including
// under `bwrap --unshare-user --unshare-pid`. A process that is not dumpable,
// or that holds capabilities, is closed to it. And so is EVERY process of the
// session from the daemon's own unit: its ProtectClock, ProtectKernel* and
// ProtectControlGroups give a user unit a user namespace of its own, and from
// a child user namespace a ptrace-mode read of a process in the parent one is
// refused (commoncap: not the same namespace, no CAP_SYS_PTRACE in the
// target's), so from there no root can be read (entry 285).
//
// So the process's cgroup is asked first. Flatpak starts every sandbox in a
// systemd scope named `app-flatpak-<app-id>-<n>.scope`, the sandbox cannot
// leave it (/sys/fs/cgroup is read-only inside), and /proc/<pid>/cgroup is
// readable without any ptrace access, from the unit too. The /.flatpak-info
// stays as the second source, for a Flatpak started where no scope is made.

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

// The Flatpak application a cgroup path belongs to: the id in the first
// `app-flatpak-<id>-<n>.scope` component. Empty when the path has none.
inline std::string flatpak_of_cgroup_path(const std::string& path) {
    static const char prefix[] = "app-flatpak-";
    static const char suffix[] = ".scope";
    const size_t prefix_length = sizeof(prefix) - 1;
    const size_t suffix_length = sizeof(suffix) - 1;
    size_t at = 0;
    while (at < path.size()) {
        size_t end = path.find('/', at);
        if (end == std::string::npos) {
            end = path.size();
        }
        const std::string component = path.substr(at, end - at);
        at = end + 1;
        if (component.size() <= prefix_length + suffix_length ||
            component.compare(0, prefix_length, prefix) != 0 ||
            component.compare(component.size() - suffix_length, suffix_length, suffix) != 0) {
            continue;
        }
        const std::string middle =
            component.substr(prefix_length, component.size() - prefix_length - suffix_length);
        // "<id>-<n>": the number is Flatpak's, the id is everything before it.
        const size_t dash = middle.rfind('-');
        if (dash == std::string::npos || dash == 0 || dash + 1 == middle.size()) {
            continue;
        }
        bool digits = true;
        for (size_t i = dash + 1; i < middle.size(); ++i) {
            digits = digits && middle[i] >= '0' && middle[i] <= '9';
        }
        if (digits) {
            return middle.substr(0, dash);
        }
    }
    return {};
}

// The Flatpak application of this process's cgroup (/proc/<pid>/cgroup, the
// unified hierarchy's `0::` line). 1 with the id, 0 when its cgroup is not a
// Flatpak's scope, -1 when the file cannot be read.
inline int flatpak_cgroup_name(const std::string& proc, long pid, std::string& id) {
    const std::string path = proc + "/" + std::to_string(pid) + "/cgroup";
    std::FILE* file = std::fopen(path.c_str(), "re");
    if (!file) {
        return -1;
    }
    char line[1024];
    int answer = 0;
    while (std::fgets(line, sizeof(line), file)) {
        if (std::strncmp(line, "0::", 3) == 0) {
            id = flatpak_of_cgroup_path(std::string(line + 3, std::strcspn(line + 3, "\r\n")));
            answer = id.empty() ? 0 : 1;
            break;
        }
    }
    std::fclose(file);
    return answer;
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
// running in it now, as far as this process can see: one stat and one or two
// opens per process of the user's, about a millisecond for a desktop
// session's. False when /proc cannot be listed at all, which leaves `ids`
// empty. The cgroup is asked first and the /.flatpak-info second (see the
// header); a sandbox that answers neither is not in the answer, and a caller
// must treat that as "not seen running".
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
        const long pid = std::strtol(entry->d_name, nullptr, 10);
        std::string id;
        if ((detail::flatpak_cgroup_name(root, pid, id) > 0 ||
             detail::flatpak_info_name(root, pid, id) > 0) &&
            !id.empty()) {
            ids.insert(id);
        }
    }
    ::closedir(processes);
    return true;
}

}  // namespace vocem

#endif  // VOCEM_DAEMON_FLATPAK_PROCESS_H

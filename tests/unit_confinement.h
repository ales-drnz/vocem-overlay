// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The daemon measured where it ships: under the [Service] properties of its
// own unit file (entry 285 -- a check that reads other processes is measured
// from inside the confinement it ships in).
//
// Two things live here.
//
//   * The pieces daemon_unit_peer builds its transient units from: the unit
//     file's [Service] lines (ExecStart, Type and the restart policy aside), a
//     command run without a shell, and the privacy measurement taken inside a
//     unit before anything is started in it.
//
//   * ensure_daemon_confinement(), the entry of every end-to-end daemon test.
//     Without VOCEM_UNIT_FILE it is ensure_private_shm(true): bwrap, private
//     /dev/shm and network. With it the test re-executes ITSELF as a transient
//     unit of the user's systemd carrying every property of that file plus
//     PrivateNetwork and a TemporaryFileSystem on /dev/shm and on the live
//     $XDG_RUNTIME_DIR/app, measures inside that all three are private, and
//     makes itself undumpable. The daemon the test then forks inherits the
//     unit's user namespace, seccomp filter, address families and memory cap,
//     exactly as the packaged vocemd.service gives them.
//
// Why undumpable: from inside the unit the real Discord is a process the
// daemon cannot look into (its user namespace closes the parent's), and that
// is what 0.1.11-1 refused. A stub the daemon CAN look into would be the
// wrong witness (entry 38). An undumpable process of the same user reads the
// same from the daemon's side -- /proc/<pid>/fd closed -- so the test's own
// stub on 6463 is seen the way Discord is.
//
// What differs from the shipped unit: MemoryMax covers the test and the
// daemon together (the test's own share is a few tens of MB at most), and the
// unit is started by systemd-run, not at login. RestrictNamespaces applies to
// the test too, so a test that builds a bwrap of its own cannot run this way.

#ifndef VOCEM_TESTS_UNIT_CONFINEMENT_H
#define VOCEM_TESTS_UNIT_CONFINEMENT_H

#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "private_shm.h"

extern char** environ;

namespace vocem_test {

inline std::string self_path() {
    char self[4096];
    const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) {
        return {};
    }
    self[n] = '\0';
    return self;
}

inline std::string read_whole_file(const std::string& path) {
    FILE* file = fopen(path.c_str(), "rb");
    if (!file) {
        return {};
    }
    std::string out;
    char buffer[4096];
    size_t got = 0;
    while ((got = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        out.append(buffer, got);
    }
    fclose(file);
    return out;
}

// A command's standard output and its exit status, run with execvp from a
// fork -- no shell, so no quoting of the unit's own lines.
inline int run_command(const std::vector<std::string>& argv, std::string* out = nullptr) {
    int pipe_fds[2];
    if (pipe(pipe_fds) != 0) {
        return -1;
    }
    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        close(pipe_fds[0]);
        dup2(pipe_fds[1], 1);
        close(pipe_fds[1]);
        std::vector<char*> args;
        for (const std::string& arg : argv) {
            args.push_back(const_cast<char*>(arg.c_str()));
        }
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    close(pipe_fds[1]);
    std::string got;
    char buffer[1024];
    ssize_t n = 0;
    while ((n = read(pipe_fds[0], buffer, sizeof(buffer))) > 0) {
        got.append(buffer, static_cast<size_t>(n));
    }
    close(pipe_fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (out) {
        *out = got;
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128;
}

// Every [Service] line of the unit file but the ones that say what to run and
// when to restart it: the confinement, as the package installs it.
inline std::vector<std::string> unit_properties(const std::string& path) {
    std::vector<std::string> properties;
    FILE* file = fopen(path.c_str(), "r");
    if (!file) {
        return properties;
    }
    char line[1024];
    bool service = false;
    while (fgets(line, sizeof(line), file)) {
        std::string text(line, strcspn(line, "\r\n"));
        while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
            text.pop_back();
        }
        if (text.empty() || text[0] == '#' || text[0] == ';') {
            continue;
        }
        if (text[0] == '[') {
            service = text == "[Service]";
            continue;
        }
        if (!service) {
            continue;
        }
        const std::string key = text.substr(0, text.find('='));
        if (key == "ExecStart" || key == "Type" || key == "Restart" || key == "RestartSec") {
            continue;
        }
        properties.push_back(text);
    }
    fclose(file);
    return properties;
}

// The file's confinement really was read: enough lines, and the one that
// restricts address families among them.
inline bool properties_look_read(const std::vector<std::string>& properties) {
    bool restricts_families = false;
    for (const std::string& property : properties) {
        restricts_families |= property.rfind("RestrictAddressFamilies=", 0) == 0;
    }
    return properties.size() >= 10 && restricts_families;
}

// The live session's $XDG_RUNTIME_DIR/app when it exists, else empty.
inline std::string live_app_directory() {
    const char* runtime = getenv("XDG_RUNTIME_DIR");
    if (!runtime || !runtime[0]) {
        return {};
    }
    const std::string app = std::string(runtime) + "/app";
    struct stat info {};
    return stat(app.c_str(), &info) == 0 && S_ISDIR(info.st_mode) ? app : std::string();
}

// The test's own confinement on top of the unit's: a network of its own (the
// live Discord on 6463 is never reached), and empty /dev/shm and
// $XDG_RUNTIME_DIR/app (the live daemon's segment and the live sandboxes are
// never touched).
inline std::vector<std::string> privacy_properties(const std::string& live_app) {
    std::vector<std::string> privacy{"PrivateNetwork=yes", "TemporaryFileSystem=/dev/shm"};
    if (!live_app.empty()) {
        privacy.push_back("TemporaryFileSystem=" + live_app);
    }
    return privacy;
}

// Measured inside the unit, before anything is started there.
struct Privacy {
    int shm_entries = 0;  // -1: /dev/shm cannot be listed
    int app_entries = 0;  // -1: the directory cannot be listed
    bool refused = false; // nothing listens on 6463 in this network
    bool holds() const { return shm_entries == 0 && app_entries == 0 && refused; }
};

inline int count_entries(const char* path) {
    DIR* directory = opendir(path);
    if (!directory) {
        return -1;
    }
    int entries = 0;
    while (const dirent* entry = readdir(directory)) {
        entries += entry->d_name[0] != '.';
    }
    closedir(directory);
    return entries;
}

inline Privacy measure_privacy(const char* live_app) {
    Privacy privacy;
    privacy.shm_entries = count_entries("/dev/shm");
    if (live_app && live_app[0]) {
        privacy.app_entries = count_entries(live_app);
    }
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(6463);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    privacy.refused =
        connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 &&
        errno == ECONNREFUSED;
    close(fd);
    return privacy;
}

// The systemd-run command that re-executes `self` as a transient unit under
// `properties` and `privacy`, with the test's environment. Every variable is
// named, --setenv=NAME, and systemd-run takes its value from its own
// environment: a value on the command line is readable by every local user
// for as long as the test runs (/proc/<pid>/cmdline), and the session's
// tokens are among them (tests/unit_command_env.cpp). The unit's own three
// variables are set in this process for the same reason.
inline std::vector<std::string> unit_command(const std::string& self, const char* unit_file,
                                             const std::string& live_app,
                                             const std::vector<std::string>& properties,
                                             const std::vector<std::string>& privacy) {
    std::string name = self.substr(self.rfind('/') + 1);
    std::vector<std::string> argv{"systemd-run", "--user", "--quiet", "--collect", "--pipe",
                                  "--wait", "--same-dir",
                                  "--unit=vocem-test-" + name + "-" + std::to_string(getpid())};
    for (const std::string& property : properties) {
        argv.push_back("--property=" + property);
    }
    for (const std::string& property : privacy) {
        argv.push_back("--property=" + property);
    }
    // A client killed by ctest's timeout does not stop the unit; this does.
    argv.push_back("--property=RuntimeMaxSec=300");
    // The test's environment, as bwrap would have kept it. Names systemd
    // refuses (a shell's exported functions) stay behind.
    for (char** entry = environ; *entry; ++entry) {
        const char* equals = strchr(*entry, '=');
        const size_t length = equals ? static_cast<size_t>(equals - *entry) : 0;
        const bool plain_name =
            length > 0 && strspn(*entry, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"
                                         "0123456789_") == length;
        if (plain_name && strncmp(*entry, "VOCEM_UNIT_", 11) != 0) {
            argv.push_back(std::string("--setenv=") + std::string(*entry, length));
        }
    }
    setenv("VOCEM_UNIT_FILE", unit_file, 1);
    setenv("VOCEM_UNIT_CONFINED", "1", 1);
    setenv("VOCEM_UNIT_LIVE_APP", live_app.c_str(), 1);
    argv.push_back("--setenv=VOCEM_UNIT_FILE");
    argv.push_back("--setenv=VOCEM_UNIT_CONFINED");
    argv.push_back("--setenv=VOCEM_UNIT_LIVE_APP");
    argv.push_back(self);
    return argv;
}

// Returns -1 to proceed, or the exit code to return: 77 when the machine
// cannot build the confinement, 1 when it was built and did not hold.
inline int ensure_daemon_confinement() {
    const char* unit_file = getenv("VOCEM_UNIT_FILE");
    if (!unit_file || !unit_file[0]) {
        return ensure_private_shm(true);
    }
    setvbuf(stdout, nullptr, _IOLBF, 0);

    // Second half: inside the unit. The sentinel only tells this half from
    // the first; the isolation is measured.
    if (getenv("VOCEM_UNIT_CONFINED")) {
        const char* live_app = getenv("VOCEM_UNIT_LIVE_APP");
        const Privacy privacy = measure_privacy(live_app);
        char found[256] = {0};
        if (!privacy.holds() || !shm_is_private(found, sizeof(found))) {
            printf("FAIL inside the unit /dev/shm holds %d entries, %s holds %d, and 6463 %s: "
                   "the daemon is not started where it could reach the live session\n",
                   privacy.shm_entries, live_app ? live_app : "(no app directory)",
                   privacy.app_entries, privacy.refused ? "is free" : "answers");
            return 1;
        }
        if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) {
            printf("FAIL cannot make the test undumpable: %s\n", strerror(errno));
            return 1;
        }
        printf("--  under %s's confinement: /dev/shm, the app directory and 6463 are "
               "private, and this process is closed to the daemon as Discord is\n",
               unit_file);
        return -1;
    }

    if (access(unit_file, R_OK) != 0) {
        printf("skip VOCEM_UNIT_FILE %s is unreadable: no unit to take the confinement from\n",
               unit_file);
        return 77;
    }
    if (run_command({"systemd-run", "--user", "--quiet", "--wait", "--collect", "true"}) != 0) {
        printf("skip no user systemd manager reachable (systemd-run --user failed)\n");
        return 77;
    }
    const std::vector<std::string> properties = unit_properties(unit_file);
    if (!properties_look_read(properties)) {
        printf("FAIL %zu [Service] properties read from %s, and none restricts address "
               "families: that is not the daemon's unit\n",
               properties.size(), unit_file);
        return 1;
    }
    const std::string self = self_path();
    if (self.empty()) {
        printf("FAIL cannot find my own binary\n");
        return 1;
    }
    const std::string live_app = live_app_directory();
    const std::vector<std::string> argv =
        unit_command(self, unit_file, live_app, properties, privacy_properties(live_app));
    printf("--  re-executed under the %zu [Service] properties of %s\n", properties.size(),
           unit_file);
    fflush(stdout);
    std::vector<char*> args;
    for (const std::string& arg : argv) {
        args.push_back(const_cast<char*>(arg.c_str()));
    }
    args.push_back(nullptr);
    execvp(args[0], args.data());
    printf("FAIL could not exec systemd-run: %s\n", strerror(errno));
    return 1;
}

}  // namespace vocem_test

#endif  // VOCEM_TESTS_UNIT_CONFINEMENT_H

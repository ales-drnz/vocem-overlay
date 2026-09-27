// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A port squatter in a Flatpak sandbox is not Discord.
//
// The daemon sends the stored OAuth token -- messages.read among its scopes --
// to whatever answers on 127.0.0.1 in Discord's port range. peer_identity.h's
// uid check refuses a listener owned by another user, and its header said that
// also closed "the cross-sandbox case". It did not: a Flatpak with network
// access runs as the same uid, and the review measured a listener under
// `bwrap --unshare-user --unshare-pid` (network shared) reading outcome=Found
// uid=1000 -- the check's own "this is us". Such a listener received
// AUTHENTICATE with the token.
//
// The sandbox is told apart by the process, not by the socket: the socket's
// inode (from /proc/net/tcp) names the process holding it (a `socket:[inode]`
// link under /proc/<pid>/fd), and that process's root carries the
// `/.flatpak-info` Flatpak puts in every sandbox and the application cannot
// change. The daemon refuses a peer whose `[Application] name=` is not one of
// Discord's own Flatpak ids.
//
// Held here with the real vocemd and a stub listener that is really in a
// sandbox of its own: a nested bwrap with a new user and pid namespace, the
// network shared with the daemon, and a /.flatpak-info naming an application.
// Named org.evil.Squatter it must not be sent AUTHENTICATE; named
// com.discordapp.Discord it must. The whole test runs inside
// ensure_private_shm(true), so the port and the segment are private.
//
// And once more as org.evil.Squatter after prctl(PR_SET_DUMPABLE, 0): the
// refutation of the first fix showed that one call hid the holder from the
// scan (its /proc/<pid> becomes root's), the daemon "continued without the
// sandbox check", and the token went out. A socket of this user's that no
// visible process holds, while some of this user's processes are hidden, is
// refused now.

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

#include "discord_stub.h"
#include "private_shm.h"
#include "probe_alarm.h"

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what.c_str());
    if (!condition) {
        ++failures;
    }
}

void write_file(const std::string& path, const std::string& body) {
    FILE* file = fopen(path.c_str(), "wb");
    if (!file) {
        return;
    }
    fwrite(body.data(), 1, body.size(), file);
    fclose(file);
}

std::string read_file(const std::string& path) {
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

// The stub, inside its sandbox. One connection: upgrade, READY, and then
// whatever the daemon sends for eight seconds. Prints one verdict line.
int listener() {
    alarm(30);
    // The one call the review's refutation added: a process that is not
    // dumpable has a /proc/<pid> owned by root -- global root, which a
    // Flatpak's user namespace does not map -- so its descriptors and its
    // root are closed to the daemon, and the scan that looks for the holder
    // of the socket does not find one. Chromium does this in its own children.
    if (getenv("VOCEM_PEER_NODUMP") && prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) {
        printf("PRCTL-FAILED\n");
        return 1;
    }
    const int server = vocem_test::listen_on(6463);
    if (server < 0) {
        printf("LISTEN-FAILED\n");
        return 1;
    }
    printf("LISTENING\n");
    fflush(stdout);
    const int fd = accept(server, nullptr, nullptr);
    if (fd < 0 || !vocem_test::accept_upgrade(fd)) {
        printf("NO-CONNECTION\n");
        return 1;
    }
    vocem_test::send_text(fd, R"({"cmd":"DISPATCH","evt":"READY","data":{"v":1},"nonce":null})");
    std::string buffer;
    std::string message;
    const double deadline = vocem_test::monotonic() + 8.0;
    while (vocem_test::recv_text(fd, buffer, message, deadline)) {
        if (message.find("\"cmd\":\"AUTHENTICATE\"") != std::string::npos) {
            printf("AUTHENTICATE %s\n",
                   message.find("test-token") != std::string::npos ? "with-token" : "");
            fflush(stdout);
            return 0;
        }
    }
    printf("NOTHING\n");
    fflush(stdout);
    return 0;
}

// The listener under its own bwrap: a tmpfs root holding only what a program
// needs, the test's own directory, and a /.flatpak-info naming `app_id`. The
// network is NOT unshared: it is the daemon's, as a Flatpak's is the host's.
pid_t start_listener(const std::string& base, const char* label, const char* app_id, bool nodump,
                     int* out_fd) {
    const std::string info = base + "/flatpak-info-" + label;
    write_file(info, std::string("[Application]\nname=") + app_id + "\nruntime=runtime/x/y/z\n");
    char self[4096];
    const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) {
        return -1;
    }
    self[n] = '\0';
    std::string directory = self;
    directory.erase(directory.rfind('/'));
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
        // bwrap passes the environment through to the listener.
        if (nodump) {
            setenv("VOCEM_PEER_NODUMP", "1", 1);
        }
        execlp("bwrap", "bwrap", "--unshare-user", "--unshare-pid", "--die-with-parent",
               "--tmpfs", "/", "--ro-bind", "/usr", "/usr", "--symlink", "usr/lib", "/lib",
               "--symlink", "usr/lib", "/lib64", "--symlink", "usr/bin", "/bin", "--ro-bind",
               "/etc", "/etc", "--proc", "/proc", "--dev", "/dev", "--ro-bind", directory.c_str(),
               directory.c_str(), "--ro-bind", info.c_str(), "/.flatpak-info", "--unsetenv",
               "LD_PRELOAD", "--setenv", "VOCEM_PEER_LISTENER", "1", self, (char*)nullptr);
        _exit(127);
    }
    close(pipe_fds[1]);
    *out_fd = pipe_fds[0];
    return pid;
}

// One line from the listener, by the deadline.
std::string line_from(int fd, double seconds) {
    std::string line;
    const double deadline = vocem_test::monotonic() + seconds;
    while (vocem_test::monotonic() < deadline) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(fd, &set);
        timeval tv{0, 100 * 1000};
        if (select(fd + 1, &set, nullptr, nullptr, &tv) <= 0) {
            continue;
        }
        char c = 0;
        if (read(fd, &c, 1) != 1) {
            break;
        }
        if (c == '\n') {
            return line;
        }
        line.push_back(c);
    }
    return line;
}

pid_t start_daemon(const char* daemon_path, const std::string& base, const std::string& log) {
    const pid_t pid = fork();
    if (pid == 0) {
        setenv("XDG_STATE_HOME", (base + "/state").c_str(), 1);
        setenv("XDG_CONFIG_HOME", (base + "/config").c_str(), 1);
        setenv("XDG_CACHE_HOME", (base + "/cache").c_str(), 1);
        setenv("XDG_RUNTIME_DIR", (base + "/runtime").c_str(), 1);
        const int fd = open(log.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        dup2(fd, 2);
        execl(daemon_path, daemon_path, nullptr);
        _exit(127);
    }
    return pid;
}

void stop(pid_t pid) {
    if (pid <= 0) {
        return;
    }
    kill(pid, SIGTERM);
    for (int i = 0; i < 100; ++i) {
        if (waitpid(pid, nullptr, WNOHANG) == pid) {
            return;
        }
        usleep(50 * 1000);
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
}

// One listener, one daemon; what the listener was sent. `label` names the
// scenario's files; `nodump` makes the listener undumpable.
std::string scenario(const char* daemon_path, const std::string& base, const char* label,
                     const char* app_id, bool nodump, std::string* daemon_log) {
    int from_listener = -1;
    const pid_t listener_pid = start_listener(base, label, app_id, nodump, &from_listener);
    if (listener_pid < 0) {
        return "no listener";
    }
    const std::string ready = line_from(from_listener, 10.0);
    if (ready != "LISTENING") {
        stop(listener_pid);
        return "listener did not start: '" + ready + "'";
    }
    const std::string log = base + "/daemon-" + label + ".log";
    const pid_t daemon_pid = start_daemon(daemon_path, base, log);
    const std::string verdict = line_from(from_listener, 15.0);
    stop(daemon_pid);
    stop(listener_pid);
    close(from_listener);
    *daemon_log = read_file(log);
    // The daemon's own words about the port, for the record.
    size_t at = 0;
    while (at < daemon_log->size()) {
        size_t end = daemon_log->find('\n', at);
        if (end == std::string::npos) {
            end = daemon_log->size();
        }
        const std::string line = daemon_log->substr(at, end - at);
        if (line.find("port 6463") != std::string::npos) {
            printf("--  daemon: %s\n", line.c_str());
        }
        at = end + 1;
    }
    return verdict;
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    if (getenv("VOCEM_PEER_LISTENER")) {
        return listener();
    }
    const char* daemon_path = getenv("VOCEM_DAEMON");
    if (!daemon_path || !daemon_path[0]) {
        printf("skip VOCEM_DAEMON not set: no daemon binary to drive\n");
        return 77;
    }
    if (const int gate = vocem_test::ensure_private_shm(true); gate >= 0) {
        return gate;
    }
    vocem_test::set_alarm(90, "a port squatter in a sandbox");

    char root[] = "/tmp/vocem-peer-sandbox-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    const std::string base = root;
    for (const char* leaf :
         {"/state", "/state/vocem", "/config", "/config/vocem", "/cache", "/runtime"}) {
        mkdir((base + leaf).c_str(), 0700);
    }
    write_file(base + "/state/vocem/token", "test-token\n");
    write_file(base + "/config/vocem/config.ini", "");

    std::string log;
    const std::string squatter = scenario(daemon_path, base, "squatter", "org.evil.Squatter", false, &log);
    printf("--  a listener in the sandbox of org.evil.Squatter was sent: %s\n", squatter.c_str());
    check(squatter == "NOTHING" || squatter == "NO-CONNECTION",
          "a listener in a non-Discord Flatpak sandbox is not sent AUTHENTICATE");
    check(log.find("org.evil.Squatter") != std::string::npos,
          "and the daemon's log names the sandbox it refused");

    const std::string discord = scenario(daemon_path, base, "discord", "com.discordapp.Discord", false, &log);
    printf("--  a listener in the sandbox of com.discordapp.Discord was sent: %s\n",
           discord.c_str());
    check(discord == "AUTHENTICATE with-token",
          "the same listener in Discord's own Flatpak is sent AUTHENTICATE with the token");

    // The squatter again, after prctl(PR_SET_DUMPABLE, 0). Its /proc entry is
    // root's now, so no process the daemon can look into holds the socket
    // whose row says it is this user's -- which a user's own /proc never
    // hides, so unseen is made unseeable, and that is refused.
    const std::string hidden = scenario(daemon_path, base, "nodump", "org.evil.Squatter", true, &log);
    printf("--  an undumpable listener in the sandbox of org.evil.Squatter was sent: %s\n",
           hidden.c_str());
    check(hidden == "NOTHING" || hidden == "NO-CONNECTION",
          "a listener that made itself undumpable is not sent AUTHENTICATE");
    check(log.find("refusing port 6463") != std::string::npos,
          "and the daemon's log says it refused the port");

    (void)!system(("rm -rf '" + base + "'").c_str());
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

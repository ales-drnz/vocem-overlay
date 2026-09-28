// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The peer check, measured where the daemon actually runs: inside its unit.
//
// 0.1.11-1 refused the owner's real Discord every 30 s. The second fix round
// had made "no process this daemon can look into holds the socket" a refusal,
// and both that fix and the pass that refuted it measured Discord from a
// shell, where its renderer is an ordinary process the /proc walk finds. The
// unit's ProtectClock/ProtectHostname/ProtectKernel*/ProtectControlGroups give
// a user unit a user namespace of its own, and from there no process of the
// session can be looked into at all -- so every listener, Discord's included,
// was refused (entry 285). Every daemon test ran the daemon from a shell,
// or under bwrap: none under the unit. This one does.
//
// The real vocemd runs as a transient unit of the user's own systemd with
// every [Service] property of the unit file the package installs, read from
// that file ($VOCEM_UNIT_FILE; ExecStart, Type and the restart policy aside),
// plus four of the test's own, each measured before the daemon starts:
// PrivateNetwork (the live Discord on 6463 is never reached), a
// TemporaryFileSystem on /dev/shm and on $XDG_RUNTIME_DIR/app (the live
// daemon's segment and the live sandboxes are not touched), and a log file.
//
// The listener is shaped like Discord's renderer, and worse: it is started in
// a systemd scope of the name under test, its socket is made there inside the
// daemon's own network namespace (a child joins the unit's user and network
// namespaces, makes it, and hands it over), and the process that holds and
// accepts it then moves to a user namespace of its own and makes itself
// undumpable -- the squatter of entry 266's refutation, which nothing looking
// into processes can see from anywhere.
//
//   * in `app-discord-<n>.scope`, as the live Discord is: sent AUTHENTICATE
//     with the token. 0.1.11-1 refused it: this fails against that binary;
//   * in `app-flatpak-org.evil.Squatter-<n>.scope`: refused, by the cgroup
//     its socket was made in -- the one thing about it the kernel reports
//     without looking into any process.
//
// And the Flatpak bridge, which asked the same closed /proc whether a Flatpak
// game was running before serving it the voice channel: a `sleep` in
// `app-flatpak-org.example.Game-<n>.scope`, with that id consented through
// flatpak_apps, must be seen running. 0.1.11-1 never saw one from the unit.
//
// What this does not cover: a unit started at login by the package (the
// properties are the file's, the manager is the user's own, the start is
// systemd-run's), and a real Flatpak's listener -- a scope of the same name
// is what Flatpak makes, and what the check reads.

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <signal.h>
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

#include "discord_stub.h"
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

std::string self_path() {
    char self[4096];
    const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n <= 0) {
        return {};
    }
    self[n] = '\0';
    return self;
}

// A command's standard output and its exit status, the command run with
// execvp from a fork -- no shell, so no quoting of the unit's own lines.
int run(const std::vector<std::string>& argv, std::string* out = nullptr) {
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

// Every [Service] line of the unit file but the ones that say what to run
// and when to restart it: the confinement, as the package installs it.
std::vector<std::string> unit_properties(const std::string& path) {
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

// ---- inside the unit: is it private? ----------------------------------------

int privacy_probe() {
    int entries = 0;
    if (DIR* shm = opendir("/dev/shm")) {
        while (const dirent* entry = readdir(shm)) {
            entries += entry->d_name[0] != '.';
        }
        closedir(shm);
    } else {
        entries = -1;
    }
    int app_entries = 0;
    if (const char* app = getenv("VOCEM_UNIT_PEER_APP")) {
        if (DIR* directory = opendir(app)) {
            while (const dirent* entry = readdir(directory)) {
                app_entries += entry->d_name[0] != '.';
            }
            closedir(directory);
        } else {
            app_entries = -1;
        }
    }
    // A bare connect and close: in the unit's own network nothing listens.
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(6463);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    const bool refused =
        connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 &&
        errno == ECONNREFUSED;
    close(fd);
    printf("shm=%d app=%d refused=%d\n", entries, app_entries, refused ? 1 : 0);
    return 0;
}

// ---- the listener -----------------------------------------------------------

// After the move to a user namespace of its own: undumpable, then the stub.
int listener_holder() {
    alarm(40);
    const int server = atoi(getenv("VOCEM_UNIT_PEER_FD"));
    if (prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) {
        printf("PRCTL-FAILED\n");
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

// In the scope under test. A child joins the daemon's user and network
// namespaces -- this user owns the unit's user namespace, so it may -- and
// makes the listening socket there, in this scope's cgroup, and hands it
// over. Then this process leaves for a user namespace of its own and execs,
// so the socket's holder is out of reach of anything looking into processes.
int listener_in_scope() {
    alarm(40);
    const char* target = getenv("VOCEM_UNIT_PEER_TARGET");
    if (!target) {
        printf("NO-TARGET\n");
        return 1;
    }
    int pair[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0, pair) != 0) {
        printf("SOCKETPAIR-FAILED\n");
        return 1;
    }
    const pid_t maker = fork();
    if (maker == 0) {
        const std::string base = std::string("/proc/") + target + "/ns/";
        const int user = open((base + "user").c_str(), O_RDONLY | O_CLOEXEC);
        const int net = open((base + "net").c_str(), O_RDONLY | O_CLOEXEC);
        if (user < 0 || net < 0 || setns(user, CLONE_NEWUSER) != 0 ||
            setns(net, CLONE_NEWNET) != 0) {
            printf("SETNS-FAILED %s\n", strerror(errno));
            fflush(stdout);
            _exit(1);
        }
        const int server = vocem_test::listen_on(6463);
        if (server < 0) {
            printf("LISTEN-FAILED %s\n", strerror(errno));
            fflush(stdout);
            _exit(1);
        }
        char byte = 0;
        iovec part{&byte, 1};
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
        msghdr message{};
        message.msg_iov = &part;
        message.msg_iovlen = 1;
        message.msg_control = control;
        message.msg_controllen = sizeof(control);
        cmsghdr* header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET;
        header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(header), &server, sizeof(int));
        _exit(sendmsg(pair[1], &message, 0) == 1 ? 0 : 1);
    }
    int status = 0;
    waitpid(maker, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return 1;  // the child said why
    }
    char byte = 0;
    iovec part{&byte, 1};
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int))] = {};
    msghdr message{};
    message.msg_iov = &part;
    message.msg_iovlen = 1;
    message.msg_control = control;
    message.msg_controllen = sizeof(control);
    int server = -1;
    if (recvmsg(pair[0], &message, 0) == 1) {
        if (cmsghdr* header = CMSG_FIRSTHDR(&message)) {
            memcpy(&server, CMSG_DATA(header), sizeof(int));
        }
    }
    if (server < 0) {
        printf("NO-SOCKET\n");
        return 1;
    }
    if (unshare(CLONE_NEWUSER) != 0) {
        printf("UNSHARE-FAILED %s\n", strerror(errno));
        return 1;
    }
    // Kept across the exec on purpose: it is the socket the daemon will reach.
    fcntl(server, F_SETFD, 0);
    setenv("VOCEM_UNIT_PEER_ROLE", "holder", 1);
    setenv("VOCEM_UNIT_PEER_FD", std::to_string(server).c_str(), 1);
    const std::string self = self_path();
    execl(self.c_str(), self.c_str(), (char*)nullptr);
    printf("EXEC-FAILED %s\n", strerror(errno));
    return 1;
}

// ---- the harness ------------------------------------------------------------

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

struct Harness {
    std::string self;
    std::string daemon;
    std::string base;
    std::vector<std::string> properties;  // the unit's
    std::vector<std::string> privacy;     // the test's own
    int counter = 0;

    std::string unit_name(const char* label) {
        return "vocem-test-unit-peer-" + std::to_string(getpid()) + "-" + label + ".service";
    }
    int number() { return getpid() * 10 + ++counter; }

    std::vector<std::string> transient(const std::string& unit, const std::string& runtime,
                                       const std::string& log) const {
        std::vector<std::string> argv{"systemd-run", "--user", "--quiet", "--collect",
                                      "--unit=" + unit};
        for (const std::string& property : properties) {
            argv.push_back("--property=" + property);
        }
        for (const std::string& property : privacy) {
            argv.push_back("--property=" + property);
        }
        argv.push_back("--property=StandardError=file:" + log);
        argv.push_back("--setenv=XDG_STATE_HOME=" + base + "/state");
        argv.push_back("--setenv=XDG_CONFIG_HOME=" + base + "/config");
        argv.push_back("--setenv=XDG_CACHE_HOME=" + base + "/cache");
        argv.push_back("--setenv=XDG_RUNTIME_DIR=" + runtime);
        argv.push_back("--setenv=VOCEM_DEBUG=1");
        return argv;
    }

    long start_daemon(const std::string& unit, const std::string& runtime,
                      const std::string& log) const {
        std::vector<std::string> argv = transient(unit, runtime, log);
        argv.push_back(daemon);
        if (run(argv) != 0) {
            return -1;
        }
        // The main pid is systemd's executor until it has set the unit's
        // namespaces up and exec'd the daemon, and the executor is not
        // dumpable, so its namespaces cannot be joined (measured: EPERM).
        // The pid is the daemon's once its comm is the daemon's name.
        std::string name = daemon.substr(daemon.rfind('/') + 1);
        name = name.substr(0, 15);
        for (int i = 0; i < 100; ++i) {
            std::string pid;
            run({"systemctl", "--user", "show", "--property=MainPID", "--value", unit}, &pid);
            const long value = atol(pid.c_str());
            if (value > 0) {
                std::string comm = read_file("/proc/" + std::to_string(value) + "/comm");
                comm = comm.substr(0, comm.find('\n'));
                if (comm == name) {
                    return value;
                }
            }
            usleep(100 * 1000);
        }
        return -1;
    }

    void stop_unit(const std::string& unit) const {
        run({"systemctl", "--user", "stop", unit});
    }

    // A process in a scope of this name, started by systemd-run --scope from
    // a fork of ours, its standard output on `*out_fd`.
    pid_t start_in_scope(const std::string& scope, const std::vector<std::string>& command,
                         const std::vector<std::pair<std::string, std::string>>& env,
                         int* out_fd) const {
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
            for (const auto& [key, value] : env) {
                setenv(key.c_str(), value.c_str(), 1);
            }
            unsetenv("LD_PRELOAD");
            std::vector<std::string> argv{"systemd-run", "--user",  "--quiet",
                                          "--scope",     "--collect", "--slice=app.slice",
                                          "--unit=" + scope};
            argv.insert(argv.end(), command.begin(), command.end());
            std::vector<char*> args;
            for (const std::string& arg : argv) {
                args.push_back(const_cast<char*>(arg.c_str()));
            }
            args.push_back(nullptr);
            execvp(args[0], args.data());
            _exit(127);
        }
        close(pipe_fds[1]);
        *out_fd = pipe_fds[0];
        return pid;
    }
};

void stop_process(pid_t pid) {
    if (pid <= 0) {
        return;
    }
    kill(pid, SIGKILL);
    waitpid(pid, nullptr, 0);
}

bool wait_for_log(const std::string& log, const std::string& text, double seconds) {
    const double deadline = vocem_test::monotonic() + seconds;
    while (vocem_test::monotonic() < deadline) {
        if (read_file(log).find(text) != std::string::npos) {
            return true;
        }
        usleep(200 * 1000);
    }
    return false;
}

void print_port_lines(const std::string& log) {
    size_t at = 0;
    while (at < log.size()) {
        size_t end = log.find('\n', at);
        if (end == std::string::npos) {
            end = log.size();
        }
        const std::string line = log.substr(at, end - at);
        if (line.find("port 6463") != std::string::npos ||
            line.find("cgroup") != std::string::npos) {
            printf("--  daemon: %s\n", line.c_str());
        }
        at = end + 1;
    }
}

// One daemon under the unit, one listener in `scope`; what the listener was
// sent.
std::string peer_scenario(Harness& harness, const char* label, const std::string& scope,
                          std::string* log_text) {
    const std::string unit = harness.unit_name(label);
    const std::string log = harness.base + "/daemon-" + label + ".log";
    const long daemon_pid = harness.start_daemon(unit, harness.base + "/runtime", log);
    if (daemon_pid <= 0) {
        harness.stop_unit(unit);
        return "daemon unit did not start";
    }
    int from_listener = -1;
    const pid_t listener = harness.start_in_scope(
        scope, {harness.self},
        {{"VOCEM_UNIT_PEER_ROLE", "listener"},
         {"VOCEM_UNIT_PEER_TARGET", std::to_string(daemon_pid)}},
        &from_listener);
    const std::string ready = line_from(from_listener, 10.0);
    std::string verdict;
    if (ready != "LISTENING") {
        verdict = "listener did not start: '" + ready + "'";
    } else {
        // Where the holder really is, as the kernel says.
        const std::string cgroup = read_file("/proc/" + std::to_string(listener) + "/cgroup");
        printf("--  the listener's cgroup: %s", cgroup.c_str());
        check(cgroup.find("/" + scope) != std::string::npos,
              "the listener runs in the scope " + scope);
        verdict = line_from(from_listener, 25.0);
    }
    harness.stop_unit(unit);
    stop_process(listener);
    close(from_listener);
    *log_text = read_file(log);
    print_port_lines(*log_text);
    return verdict;
}

}  // namespace

int main() {
    setvbuf(stdout, nullptr, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    if (const char* role = getenv("VOCEM_UNIT_PEER_ROLE")) {
        if (strcmp(role, "probe") == 0) {
            return privacy_probe();
        }
        if (strcmp(role, "listener") == 0) {
            return listener_in_scope();
        }
        if (strcmp(role, "holder") == 0) {
            return listener_holder();
        }
        return 2;
    }
    const char* daemon = getenv("VOCEM_DAEMON");
    if (!daemon || !daemon[0]) {
        printf("skip VOCEM_DAEMON not set: no daemon binary to drive\n");
        return 77;
    }
    const char* unit_file = getenv("VOCEM_UNIT_FILE");
    if (!unit_file || access(unit_file, R_OK) != 0) {
        printf("skip VOCEM_UNIT_FILE not set or unreadable: no unit to take the confinement from\n");
        return 77;
    }
    if (run({"systemd-run", "--user", "--quiet", "--wait", "--collect", "true"}) != 0) {
        printf("skip no user systemd manager reachable (systemd-run --user failed)\n");
        return 77;
    }
    vocem_test::set_alarm(170, "the daemon under its unit's confinement");

    Harness harness;
    harness.self = self_path();
    harness.daemon = daemon;
    harness.properties = unit_properties(unit_file);
    printf("--  %zu properties from %s\n", harness.properties.size(), unit_file);
    bool restricts_families = false;
    for (const std::string& property : harness.properties) {
        restricts_families |= property.rfind("RestrictAddressFamilies=", 0) == 0;
    }
    check(harness.properties.size() >= 10 && restricts_families,
          "the unit file's confinement was read (it restricts address families)");

    char root[] = "/tmp/vocem-unit-peer-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    harness.base = root;
    for (const char* leaf : {"/state", "/state/vocem", "/config", "/config/vocem", "/cache",
                             "/runtime", "/runtime-bridge", "/runtime-bridge/app"}) {
        mkdir((harness.base + leaf).c_str(), 0700);
    }
    write_file(harness.base + "/state/vocem/token", "test-token\n");
    write_file(harness.base + "/config/vocem/config.ini",
               "[behaviour]\nflatpak_apps = org.example.Game\n");

    // The test's own confinement on top of the unit's, and the proof that it
    // holds before the daemon is started inside it.
    harness.privacy = {"PrivateNetwork=yes", "TemporaryFileSystem=/dev/shm"};
    std::string live_app;
    if (const char* runtime = getenv("XDG_RUNTIME_DIR")) {
        live_app = std::string(runtime) + "/app";
        struct stat info {};
        if (stat(live_app.c_str(), &info) == 0 && S_ISDIR(info.st_mode)) {
            harness.privacy.push_back("TemporaryFileSystem=" + live_app);
        } else {
            live_app.clear();
        }
    }
    {
        std::vector<std::string> argv = harness.transient(
            harness.unit_name("probe"), harness.base + "/runtime", harness.base + "/probe.log");
        argv.insert(argv.begin() + 3, "--wait");
        argv.insert(argv.begin() + 3, "--pipe");
        argv.push_back("--setenv=VOCEM_UNIT_PEER_ROLE=probe");
        if (!live_app.empty()) {
            argv.push_back("--setenv=VOCEM_UNIT_PEER_APP=" + live_app);
        }
        argv.push_back(harness.self);
        std::string said;
        const int status = run(argv, &said);
        printf("--  inside the unit: %s", said.c_str());
        const bool private_enough =
            status == 0 && said.find("shm=0 ") != std::string::npos &&
            said.find("app=0 ") != std::string::npos && said.find("refused=1") != std::string::npos;
        check(private_enough,
              "the unit's /dev/shm and $XDG_RUNTIME_DIR/app are empty and nothing listens on "
              "6463 in its network");
        if (!private_enough) {
            printf("the daemon is not started where it could reach the live session\n");
            (void)!system(("rm -rf '" + harness.base + "'").c_str());
            return 1;
        }
    }

    // 1. Shaped like the live Discord: its renderer's scope.
    std::string log;
    const std::string discord_scope = "app-discord-" + std::to_string(harness.number()) + ".scope";
    const std::string discord = peer_scenario(harness, "discord", discord_scope, &log);
    printf("--  a listener in %s was sent: %s\n", discord_scope.c_str(), discord.c_str());
    check(discord == "AUTHENTICATE with-token",
          "a listener in a scope named like Discord's, undumpable in a user namespace of its "
          "own, is sent AUTHENTICATE with the token by the daemon under its unit");

    // 2. The same listener in a Flatpak's scope that is not Discord's.
    const std::string squatter_scope =
        "app-flatpak-org.evil.Squatter-" + std::to_string(harness.number()) + ".scope";
    const std::string squatter = peer_scenario(harness, "squatter", squatter_scope, &log);
    printf("--  a listener in %s was sent: %s\n", squatter_scope.c_str(), squatter.c_str());
    check(squatter == "NOTHING" || squatter == "NO-CONNECTION",
          "the same listener in a non-Discord Flatpak's scope is not sent AUTHENTICATE");
    check(log.find("refusing port 6463") != std::string::npos &&
              log.find("org.evil.Squatter") != std::string::npos,
          "and the daemon's log refuses the port and names the sandbox");

    // 3. The Flatpak bridge: is a consented Flatpak game seen running?
    {
        const std::string runtime = harness.base + "/runtime-bridge";
        const std::string bridge = runtime + "/app/org.example.Game/vocem";
        mkdir((runtime + "/app/org.example.Game").c_str(), 0700);
        mkdir(bridge.c_str(), 0700);
        write_file(bridge + "/request", "pid=1\ndrawing=1\n");
        const std::string unit = harness.unit_name("bridge");
        const std::string bridge_log = harness.base + "/daemon-bridge.log";
        const long daemon_pid = harness.start_daemon(unit, runtime, bridge_log);
        check(daemon_pid > 0, "the daemon unit for the bridge started");
        const bool absent = wait_for_log(
            bridge_log, "the voice channel to org.example.Game: no process of this user's", 10.0);
        check(absent, "with no process of org.example.Game running, the bridge says so");
        int from_game = -1;
        const std::string game_scope =
            "app-flatpak-org.example.Game-" + std::to_string(harness.number()) + ".scope";
        const pid_t game = harness.start_in_scope(game_scope, {"/usr/bin/sleep", "30"}, {},
                                                  &from_game);
        const bool seen = wait_for_log(
            bridge_log, "serving the voice channel to org.example.Game: a process of it is running",
            12.0);
        check(seen,
              "a process in " + game_scope + " is seen running from inside the unit, and the "
              "game is served the voice channel");
        harness.stop_unit(unit);
        stop_process(game);
        close(from_game);
        const std::string text = read_file(bridge_log);
        size_t at = 0;
        while (at < text.size()) {
            size_t end = text.find('\n', at);
            if (end == std::string::npos) {
                end = text.size();
            }
            const std::string line = text.substr(at, end - at);
            if (line.find("org.example.Game") != std::string::npos) {
                printf("--  daemon: %s\n", line.c_str());
            }
            at = end + 1;
        }
    }

    (void)!system(("rm -rf '" + harness.base + "'").c_str());
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The injected code's log writes whole lines, keeps its file to itself, and
// says when it cannot open one.
//
// overlay_log() used to spend three stdio calls per line -- a prefix, the
// message, a newline -- with nothing between two threads, and stderr is
// unbuffered, so each of those was a write(2) of its own: the review's
// log_interleave probe, two threads of 100000 lines, found 56-58 thousand of
// the 200000 malformed. The file was fopen(path, "a"), whose descriptor
// rides into everything the game execs (entry 98 gave the journal O_CLOEXEC
// for the same reason), and a VOCEM_LOG_FILE that could not be opened was
// silence -- exactly the case the variable exists for (entry 38).
//
// Held here: two threads, 100000 lines each, into both sinks at once; every
// line in each sink is one whole line, and there are 200000 of them. The log
// file's descriptor carries FD_CLOEXEC. A child told to log into a path that
// cannot be opened says so on stderr.

#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#include "probe_alarm.h"
#include "vocem/overlay_log.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

constexpr int kLines = 100000;

// Whole lines of the two writers, and nothing else; returns how many lines
// were not.
long malformed(const std::string& path, long& whole, bool with_pid) {
    whole = 0;
    long bad = 0;
    std::FILE* file = std::fopen(path.c_str(), "r");
    if (!file) {
        return -1;
    }
    char line[512];
    char expect_prefix[64];
    if (with_pid) {
        std::snprintf(expect_prefix, sizeof(expect_prefix), "[vocem/test %d] ",
                      static_cast<int>(getpid()));
    } else {
        std::snprintf(expect_prefix, sizeof(expect_prefix), "[vocem/test] ");
    }
    const size_t prefix_length = std::strlen(expect_prefix);
    while (std::fgets(line, sizeof(line), file)) {
        const size_t length = std::strlen(line);
        char who[16] = {0};
        int number = -1;
        char tail = 0;
        if (length > 0 && line[length - 1] == '\n' &&
            std::strncmp(line, expect_prefix, prefix_length) == 0 &&
            std::sscanf(line + prefix_length, "%15s line %d%c", who, &number, &tail) == 3 &&
            tail == '\n' && (!std::strcmp(who, "game") || !std::strcmp(who, "atlas")) &&
            number >= 0 && number < kLines) {
            ++whole;
        } else {
            ++bad;
        }
    }
    std::fclose(file);
    return bad;
}

bool log_fd_has_cloexec(const std::string& path, bool& found) {
    found = false;
    for (int fd = 3; fd < 1024; ++fd) {
        char link[64];
        char target[1024];
        std::snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        const ssize_t n = readlink(link, target, sizeof(target) - 1);
        if (n <= 0) {
            continue;
        }
        target[n] = '\0';
        if (path == target) {
            found = true;
            return (fcntl(fd, F_GETFD) & FD_CLOEXEC) != 0;
        }
    }
    return false;
}

std::string read_all(const std::string& path) {
    std::string text;
    if (std::FILE* file = std::fopen(path.c_str(), "r")) {
        char buffer[4096];
        size_t got;
        while ((got = std::fread(buffer, 1, sizeof(buffer), file)) > 0) {
            text.append(buffer, got);
        }
        std::fclose(file);
    }
    return text;
}

}  // namespace

int main() {
    vocem_test::set_alarm(120, "logging from two threads");
    char scratch_template[] = "/tmp/vocem-overlay-log-XXXXXX";
    const char* scratch = mkdtemp(scratch_template);
    if (!scratch) {
        std::printf("FAIL no scratch directory\n");
        return 1;
    }
    const std::string log_path = std::string(scratch) + "/log";
    const std::string err_path = std::string(scratch) + "/stderr";
    const std::string refused_err = std::string(scratch) + "/refused-stderr";

    // A child first, before this process has read either variable: a log
    // file that cannot be opened must be said.
    const pid_t child = fork();
    if (child == 0) {
        const int err = open(refused_err.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        dup2(err, 2);
        unsetenv("VOCEM_DEBUG");
        setenv("VOCEM_LOG_FILE", (std::string(scratch) + "/no/such/dir/log").c_str(), 1);
        VOCEM_OVERLAY_LOG("vocem/test", "a line with nowhere to go");
        _exit(0);
    }
    int status = 0;
    waitpid(child, &status, 0);
    const std::string refusal = read_all(refused_err);
    std::printf("     the child's stderr: %s", refusal.empty() ? "(nothing)\n" : refusal.c_str());
    check(refusal.find("VOCEM_LOG_FILE") != std::string::npos,
          "a VOCEM_LOG_FILE that cannot be opened is said on stderr");

    // Both sinks at once, from two threads.
    setenv("VOCEM_DEBUG", "1", 1);
    setenv("VOCEM_LOG_FILE", log_path.c_str(), 1);
    std::fflush(stdout);
    const int saved_stderr = dup(2);
    const int err = open(err_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    dup2(err, 2);
    close(err);
    auto work = [](const char* who) {
        for (int i = 0; i < kLines; ++i) {
            VOCEM_OVERLAY_LOG("vocem/test", "%s line %d", who, i);
        }
    };
    std::thread a(work, "game");
    std::thread b(work, "atlas");
    a.join();
    b.join();
    dup2(saved_stderr, 2);
    close(saved_stderr);

    long whole = 0;
    const long bad_file = malformed(log_path, whole, true);
    std::printf("     VOCEM_LOG_FILE: %ld whole lines, %ld malformed\n", whole, bad_file);
    check(bad_file == 0 && whole == 2L * kLines, "every line in the log file is whole");
    const long bad_err = malformed(err_path, whole, false);
    std::printf("     stderr: %ld whole lines, %ld malformed\n", whole, bad_err);
    check(bad_err == 0 && whole == 2L * kLines, "every line on stderr is whole");

    bool found = false;
    const bool cloexec = log_fd_has_cloexec(log_path, found);
    check(found, "the log file is held open by this process");
    check(cloexec, "and its descriptor does not ride into an exec");

    std::string cleanup = std::string("rm -rf '") + scratch + "'";
    if (std::system(cleanup.c_str()) != 0) {
        std::printf("     (could not remove %s)\n", scratch);
    }
    std::printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

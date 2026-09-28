// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// `vocem --watch` across a daemon's death and rebirth.
//
// The CLI is "the cheapest way to tell a daemon problem apart from a rendering
// problem" (its own header), and it is what somebody runs while restarting
// the daemon. A mapping outlives the segment's name (vocem/shm.h), and the
// watch loop read its mapping and nothing else: after `systemctl restart
// vocemd` it printed the cleared state the old daemon left, for ever, and
// never saw the new daemon's segment -- exactly the moment it was being
// watched for (entry 136). The first test the CLI has ever had.
//
// A private /dev/shm, a segment published by this process, the real `vocem`
// binary on a pipe, and three readings: the channel, then "not running" once
// the segment is unlinked, then the NEW channel once a second segment is
// published.

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <string>

#include "private_shm.h"
#include "probe_alarm.h"
#include "vocem/shm.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

double monotonic() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + static_cast<double>(ts.tv_nsec) / 1e9;
}

// Reads the CLI's output for up to `seconds` or until `needle` appears in what
// has arrived since the last call. What arrived is appended to `seen`.
bool appears(int fd, std::string& seen, const char* needle, double seconds) {
    const double deadline = monotonic() + seconds;
    seen.clear();
    for (;;) {
        if (seen.find(needle) != std::string::npos) {
            return true;
        }
        const double remaining = deadline - monotonic();
        if (remaining <= 0) {
            return false;
        }
        pollfd pfd{fd, POLLIN, 0};
        if (poll(&pfd, 1, static_cast<int>(remaining * 1000)) <= 0) {
            continue;
        }
        char chunk[4096];
        const ssize_t got = read(fd, chunk, sizeof(chunk));
        if (got <= 0) {
            return false;
        }
        seen.append(chunk, static_cast<size_t>(got));
    }
}

void publish(vocem::StateWriter& writer, const char* channel) {
    writer.publish([channel](vocem::SharedState& state) {
        state.connected = 1;
        state.in_channel = 1;
        state.status = 2;  // Connected
        snprintf(state.channel_name, sizeof(state.channel_name), "%s", channel);
        state.user_count = 1;
        state.users[0].id = 1;
        snprintf(state.users[0].name, sizeof(state.users[0].name), "Watcher");
    });
}

}  // namespace

int main() {
    const char* cli = getenv("VOCEM_CLI");
    if (!cli || !cli[0]) {
        printf("skip VOCEM_CLI not set: no vocem binary to drive\n");
        return 77;
    }
    if (const int gate = vocem_test::ensure_private_shm(false); gate >= 0) {
        return gate;
    }
    vocem_test::set_alarm(60, "the CLI following a daemon");
    signal(SIGPIPE, SIG_IGN);

    // The first daemon's segment.
    vocem::StateWriter* writer = new vocem::StateWriter;
    check(writer->open(), "the private state segment opens");
    publish(*writer, "first-life");

    int pipe_fds[2] = {-1, -1};
    if (pipe(pipe_fds) != 0) {
        printf("FAIL pipe\n");
        return 1;
    }
    const pid_t child = fork();
    if (child == 0) {
        dup2(pipe_fds[1], 1);
        close(pipe_fds[0]);
        close(pipe_fds[1]);
        execl(cli, cli, "--watch", nullptr);
        _exit(127);
    }
    close(pipe_fds[1]);

    std::string seen;
    check(appears(pipe_fds[0], seen, "first-life", 5.0), "the watch shows the channel");

    // The daemon stops: the name goes, the pages stay mapped in the CLI.
    writer->close();
    vocem::StateWriter::unlink_segment();
    check(appears(pipe_fds[0], seen, "not running", 8.0),
          "after the segment is unlinked the watch says the daemon is not running");

    // A new daemon: a new segment under the same name.
    delete writer;
    writer = new vocem::StateWriter;
    check(writer->open(), "a second segment opens under the same name");
    publish(*writer, "second-life");
    check(appears(pipe_fds[0], seen, "second-life", 8.0),
          "and the watch finds it: the new daemon's channel, not the old mapping's history");

    kill(child, SIGTERM);
    int status = 0;
    waitpid(child, &status, 0);
    close(pipe_fds[0]);
    writer->close();
    vocem::StateWriter::unlink_segment();
    delete writer;

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

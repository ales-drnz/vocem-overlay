// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A segment the reader refuses is not "no daemon", and the CLI says which.
//
// The first fix round made every reader refuse an object at the segment's name
// that is not this user's 0600 regular file (segment_trust_problem, vocem/shm.h),
// and kept the reason in StateReader::refusal(). The CLI never asked for it:
// with a 64 KiB /dev/shm/vocem-<uid> at 0644 it printed "vocemd is not running
// (no shared state segment)" -- false twice over, since a segment was there and
// a daemon may well be running (the second round's refutation, measured with
// exactly this object). Entries 38 and 55 are what a refusal that reads like
// silence costs.
//
// A private /dev/shm (private_shm.h), one refused object planted in it, and the
// real `vocem` run on it once and with --watch.

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <string>

#include "private_shm.h"
#include "probe_alarm.h"
#include "vocem/shm.h"

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what.c_str());
    if (!condition) {
        ++failures;
    }
}

// Everything the command wrote, stdout and stderr together, and its status.
std::string run(const std::string& command, int* status) {
    std::string out;
    FILE* pipe = popen((command + " 2>&1").c_str(), "r");
    if (!pipe) {
        *status = -1;
        return out;
    }
    char buffer[4096];
    size_t got = 0;
    while ((got = fread(buffer, 1, sizeof(buffer), pipe)) > 0) {
        out.append(buffer, got);
    }
    *status = pclose(pipe);
    return out;
}

std::string first_line(const std::string& text) {
    return text.substr(0, text.find('\n'));
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
    vocem_test::set_alarm(30, "the CLI and a refused segment");

    char name[64];
    vocem::shm_name(name, sizeof(name), getuid());
    const int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        printf("FAIL could not create %s in the private /dev/shm\n", name);
        return 1;
    }
    // Wider than the daemon ever makes it: others may open it.
    const bool planted = fchmod(fd, 0644) == 0 && ftruncate(fd, 64 * 1024) == 0;
    close(fd);
    check(planted, "a 64 KiB segment at 0644 is planted at the daemon's name");

    int status = 0;
    const std::string once = run(std::string("'") + cli + "'", &status);
    printf("--  vocem said: %s\n", first_line(once).c_str());
    check(once.find("not running") == std::string::npos,
          "the CLI does not say the daemon is not running");
    check(once.find("refused") != std::string::npos &&
              once.find("other users") != std::string::npos,
          "it says the segment was refused, and why");
    check(WIFEXITED(status) && WEXITSTATUS(status) == 1, "and exits 1");

    // --watch runs until it is stopped; one second of it is several ticks.
    const std::string watched =
        run(std::string("timeout -s INT 1 '") + cli + "' --watch", &status);
    printf("--  vocem --watch said: %s\n", first_line(watched).c_str());
    check(watched.find("not running") == std::string::npos &&
              watched.find("refused") != std::string::npos,
          "--watch says the same, not that the daemon is not running");

    shm_unlink(name);
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

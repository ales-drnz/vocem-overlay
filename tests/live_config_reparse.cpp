// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Two writes inside one second must both reach the overlay.
//
// LiveConfig reloads when the file's modification time moved, and the stamp it
// compared was whole seconds (st_mtime). Two Applies landing in the same
// wall-clock second with a poll between them therefore left the poller holding
// the second write's stamp without ever having read it, and the second edit
// stayed invisible until the file moved again -- a missing update with nothing
// logged, which is entry 38's shape applied to the settings. The stamp is
// nanoseconds now (st_mtim, in a long long so 32-bit games keep all of it),
// and this walks the exact sequence: write, poll, write within the same
// second, poll again. Against the whole-second Config::mtime() the second
// poll returns the stale value and this fails.

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "vocem/live_config.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

void write_config(const char* path, float row_spacing) {
    std::FILE* file = std::fopen(path, "w");
    if (!file) {
        std::printf("FAIL cannot write %s\n", path);
        ++failures;
        return;
    }
    std::fprintf(file, "row_spacing = %.1f\n", static_cast<double>(row_spacing));
    std::fclose(file);
}

// Pins the file's modification time to an exact second-and-nanosecond pair, so
// the same-second case is the case, deterministically, rather than a race this
// test would usually lose.
void pin_mtime(const char* path, time_t seconds, long nanoseconds) {
    struct timespec times[2];
    times[0].tv_sec = seconds;
    times[0].tv_nsec = nanoseconds;
    times[1] = times[0];
    if (utimensat(AT_FDCWD, path, times, 0) != 0) {
        std::printf("FAIL utimensat\n");
        ++failures;
    }
}

}  // namespace

int main() {
    char root[] = "/tmp/vocem-live-config-XXXXXX";
    if (!mkdtemp(root)) {
        std::printf("FAIL mkdtemp\n");
        return 1;
    }
    char dir[600];
    std::snprintf(dir, sizeof(dir), "%s/vocem", root);
    mkdir(dir, 0700);
    setenv("XDG_CONFIG_HOME", root, 1);
    char path[600];
    std::snprintf(path, sizeof(path), "%s/vocem/config.ini", root);

    const time_t second = ::time(nullptr);

    write_config(path, 4.0f);
    pin_mtime(path, second, 100);

    vocem::LiveConfig live;
    check(live.current(0.0).row_spacing == 4.0f, "the first write is read");

    // The same wall-clock second, a later nanosecond: the case the whole-second
    // stamp could not see.
    write_config(path, 9.0f);
    pin_mtime(path, second, 200);
    check(live.current(0.0).row_spacing == 9.0f,
          "a second write inside the same second is read too");

    // And the ordinary case still works.
    write_config(path, 14.0f);
    pin_mtime(path, second + 2, 0);
    check(live.current(0.0).row_spacing == 14.0f, "a later write is read as before");

    char cleanup[700];
    std::snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    if (std::system(cleanup) != 0) {
        std::printf("     (scratch directory not removed)\n");
    }

    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

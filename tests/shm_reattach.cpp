// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A reader that survived the daemon must be able to find the next daemon.
//
// Unlinking a shared memory object removes its name, not its pages: a game that
// mapped the segment keeps reading its orphaned copy forever. The daemon's exit
// publishes a cleared state first, so the overlay disappears -- but when a new
// daemon starts it creates a *new* object under the old name, and a reader that
// never looks at the name again stays on the dead pages: quit-and-reopen would
// work for every game except the one being played. StateReader::still_current()
// is the look at the name -- one shm_open, two fstats, an inode comparison --
// and the injected paths ask it on the same cadence as their reopen retries.
//
// Held here across two writer lives in one process, in a private /dev/shm so
// the real daemon's segment is never involved.

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "vocem/shm.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

void publish_channel(vocem::StateWriter& writer, const char* name) {
    writer.publish([&](vocem::SharedState& state) {
        state.connected = 1;
        state.in_channel = 1;
        state.user_count = 1;
        snprintf(state.channel_name, sizeof(state.channel_name), "%s", name);
    });
}

}  // namespace

int main() {
    if (!getenv("VOCEM_SANDBOXED")) {
        if (system("command -v bwrap >/dev/null 2>&1") != 0) {
            printf("skip bwrap is not installed, so the private /dev/shm cannot be built\n");
            return 77;
        }
        char self[4096];
        const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n <= 0) {
            printf("FAIL cannot find my own binary\n");
            return 1;
        }
        self[n] = '\0';
        setenv("VOCEM_SANDBOXED", "1", 1);
        execlp("bwrap", "bwrap", "--dev-bind", "/", "/", "--tmpfs", "/dev/shm",
               "--die-with-parent", self, nullptr);
        printf("FAIL could not exec bwrap\n");
        return 1;
    }
    alarm(30);

    // First life: a daemon publishes, a game attaches and reads.
    auto* first = new vocem::StateWriter;
    check(first->open(), "the first daemon's segment opens");
    publish_channel(*first, "first-life");

    vocem::StateReader reader;
    vocem::Snapshot snapshot{};
    check(reader.open(), "the game attaches");
    check(reader.read(snapshot) && strcmp(snapshot.channel_name, "first-life") == 0,
          "and reads the first daemon's state");
    check(reader.still_current(), "while the daemon lives, the mapping is the name's object");

    // The daemon stops: clears, closes, unlinks -- the order vocemd exits in.
    first->publish([](vocem::SharedState& state) {
        state.connected = 0;
        state.in_channel = 0;
        state.user_count = 0;
    });
    first->close();
    vocem::StateWriter::unlink_segment();
    delete first;

    check(reader.read(snapshot) && !snapshot.in_channel,
          "the orphaned mapping still reads, and holds the cleared state");
    check(!reader.still_current(), "and the name no longer stands behind it");

    // Second life. Without the identity check the reader would never see this.
    vocem::StateWriter second;
    check(second.open(), "a second daemon's segment opens under the same name");
    publish_channel(second, "second-life");

    check(!reader.still_current(), "the old mapping is not the new object either");
    reader.close();
    check(reader.open(), "so the reader reattaches");
    check(reader.read(snapshot) && strcmp(snapshot.channel_name, "second-life") == 0,
          "and reads the second daemon's state, not history");
    check(reader.still_current(), "which the name now stands behind");

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

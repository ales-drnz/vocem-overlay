// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Whether it is safe for a test to publish a channel -- verified, not declared.
//
// This exists because of an incident, and the incident is the whole argument.
// Every probe that publishes state re-execs itself under `bwrap --tmpfs /dev/shm`
// so its fake channel lands in a private segment, and each decided whether it was
// already inside that sandbox by reading a sentinel environment variable it had
// set itself. A sentinel is a promise, and it was forged: a diagnostic invocation
// set `VOCEM_SANDBOXED=1` by hand to skip the re-exec (it wanted the probe's own
// pid, which `exec bwrap` takes away) and the probe ran on the session's real
// /dev/shm. `StateWriter::open()` opens the segment with `O_CREAT` and no
// `O_EXCL`, so it did not create a second one: it wrote its three fake
// participants **into the running daemon's own segment**, and on the way out
// `unlink_segment()` removed the name the daemon publishes under. Both of the
// owner's games showed `Local 1 / Local 2 / Local 3` -- a channel that does not
// exist, with nobody's face -- while he was playing.
//
// So the question "am I isolated?" is answered by looking. The first version of
// this looked for an *empty* /dev/shm -- bwrap's tmpfs is empty and a live session
// never is -- and that was too strict by exactly one measurement: Steam's
// `gameoverlayrenderer.so`, preloaded beside ours in the coexistence tests,
// creates a segment of its own from a library constructor, before `main` runs. A
// private tmpfs is not empty once somebody's constructor has been in it.
//
// What is asked instead are the two things that mean harm, and the answer says
// which:
//
//   * **the daemon's own segment already exists** (`/vocem-<uid>`). That is the
//     object the accident wrote into. Its presence is decisive: publishing means
//     overwriting somebody's live channel.
//   * **/dev/shm carries the hallmarks of a live desktop session** -- pulseaudio's
//     or pipewire's segments, the compositor's. A private tmpfs never has those,
//     because nothing inside the sandbox creates them; a session always does.
//
// The blind spot is named rather than left to be discovered: a machine with no
// pulse/pipewire segments AND no daemon running would pass this check on its real
// /dev/shm. What a probe would do there is create the segment and unlink it on the
// way out -- harmless with no daemon, except to a game still running from a
// previous one, which would reattach to the fake channel (`still_current`).
// Narrow, and the reason the re-exec under bwrap stays the *first* thing every
// probe does rather than a fallback.
//
// The sentinel stays, for one job only: telling a second attempt from a first, so
// a failure to sandbox is a loud error instead of an exec loop.

#ifndef VOCEM_TESTS_PRIVATE_SHM_H
#define VOCEM_TESTS_PRIVATE_SHM_H

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

namespace vocem_test {

// True when publishing a fake channel here cannot reach anybody: no daemon
// segment, and none of the segments a live session always carries. `found`
// receives the name of whatever decided it, for a message that says which.
inline bool shm_is_private(char* found, size_t capacity) {
    if (found && capacity) {
        found[0] = '\0';
    }
    char daemon_segment[64];
    snprintf(daemon_segment, sizeof(daemon_segment), "vocem-%u", (unsigned)getuid());

    DIR* dir = opendir("/dev/shm");
    if (!dir) {
        // No /dev/shm to corrupt. Publishing will fail on its own terms.
        return true;
    }
    bool safe = true;
    while (const struct dirent* entry = readdir(dir)) {
        const char* name = entry->d_name;
        if (!strcmp(name, ".") || !strcmp(name, "..")) {
            continue;
        }
        const bool is_daemon = !strcmp(name, daemon_segment);
        const bool is_session = !strncmp(name, "pulse-", 6) || !strncmp(name, "pipewire", 8) ||
                                !strncmp(name, "wayland", 7);
        if (is_daemon || is_session) {
            safe = false;
            if (found && capacity) {
                snprintf(found, capacity, "%s", name);
            }
            break;
        }
    }
    closedir(dir);
    return safe;
}

// The line a probe prints when it refuses. Kept here so all of them say the same
// thing, and so the reason is the mechanism rather than "sandbox missing".
inline void shm_explain_refusal(const char* found) {
    printf("FAIL /dev/shm is not private (it holds '%s'), so publishing a fake\n"
           "     channel here would write into the running daemon's own segment\n"
           "     and unlink the name it publishes under -- both games in the\n"
           "     owner's session once showed 'Local 1/2/3' because of exactly\n"
           "     that. Run this under bwrap --tmpfs /dev/shm, which the probe\n"
           "     does for itself when it is not told otherwise.\n",
           found && found[0] ? found : "?");
}

}  // namespace vocem_test

#endif  // VOCEM_TESTS_PRIVATE_SHM_H

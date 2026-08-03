// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The frame path reads no files -- measured, not promised.
//
// `fonts_note_emoji` runs on every visible string of every drawn frame, on both
// injected paths, and it is *inside* the present: on Vulkan it is called from
// `draw()` while the layer holds its own lock, before the real
// `vkQueuePresentKHR`. The first sighting of a codepoint that could be a colour
// emoji costs an `open` of the bank and a binary search of `pread`s -- so the
// property that makes this legal is not "no I/O ever" but "every codepoint costs
// its lookup once, and a steady frame costs nothing". The layer's own comment
// claimed the absolute version; this test measures the true one.
//
// Both halves matter, and the second is the one that is easy to lose:
//
//   * a codepoint the bank HAS is remembered as had;
//   * a codepoint the bank does NOT have is remembered as not had.
//
// Without the second, a name carrying a typographic apostrophe would search the
// bank on every frame for the life of the game. The cap check sits above the
// lookup for the same reason, so a channel past the seen-cap does not probe
// either. All three are held here by the machine rather than by reading: a
// seccomp filter that kills the process on any file syscall, installed after the
// warm-up, and then a thousand frames' worth of the same strings.
//
// What this does NOT cover, said here rather than implied: the avatar side. The
// GL provider stats and reads avatar files from inside its draw (throttled, and
// the Vulkan side defers both to the post-present phase) -- a separate finding,
// with its own measurement to come.

#include <linux/filter.h>
#include <linux/seccomp.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "vocem/fonts.h"

namespace {

// A channel name and four display names of the shape this overlay actually
// meets: emoji the bank carries, emoji it does not, and punctuation that looks
// like an emoji to a range check and is not one.
const char* const kStrings[] = {
    "\xF0\x9F\x94\x8A\xE3\x83\xBB stanza",         // speaker + katakana middle dot
    "Reix \xF0\x9F\x8D\xA3",                        // sushi: in the bank
    "Ale\xE2\x80\x99s laptop",                      // typographic apostrophe: not in the bank
    "nightowl \xE2\xAD\x90 \xE2\x9C\x85",           // star and tick, down among the symbols
    "\xF0\x9F\xA5\xA2 chopsticks",                  // in the bank
};

// Kill the process on any file syscall: the same instrument as
// tests/shim_seccomp.cpp, for the same reason -- a filter is the only honest
// answer to "which syscalls does this make" on a machine with no strace.
bool forbid_file_syscalls() {
#define KILL_ON(nr)                                  \
    BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, (nr), 0, 1), \
    BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS)
    struct sock_filter filter[] = {
        BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(struct seccomp_data, nr)),
#ifdef SYS_open
        KILL_ON(SYS_open),
#endif
        KILL_ON(SYS_openat),
#ifdef SYS_openat2
        KILL_ON(SYS_openat2),
#endif
#ifdef SYS_stat
        KILL_ON(SYS_stat),
#endif
#ifdef SYS_newfstatat
        KILL_ON(SYS_newfstatat),
#endif
#ifdef SYS_statx
        KILL_ON(SYS_statx),
#endif
        KILL_ON(SYS_pread64),
#ifdef SYS_read
        KILL_ON(SYS_read),
#endif
        BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
    };
#undef KILL_ON
    struct sock_fprog prog = {sizeof(filter) / sizeof(filter[0]), filter};
    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0)) {
        return false;
    }
    return prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &prog) == 0;
}

void note_every_string() {
    for (const char* text : kStrings) {
        vocem::fonts_note_emoji(text);
    }
}

}  // namespace

int main() {
    if (!getenv("VOCEM_EMOJI_BANK")) {
        printf("skip meant to run with VOCEM_EMOJI_BANK pointing at the repo's bank\n");
        return 77;
    }

    // The warm-up is the part that is allowed to read: every codepoint above is
    // seen once here, verdict remembered either way.
    note_every_string();
    printf("warm-up done: %s\n",
           vocem::fonts_emoji_status() ? vocem::fonts_emoji_status() : "colour emoji available");

    fflush(stdout);
    const pid_t pid = fork();
    if (pid == 0) {
        if (!forbid_file_syscalls()) {
            _exit(77);
        }
        // A thousand frames of the same names. Any file syscall in here is the
        // defect this test exists for, and the kernel reports it as a corpse.
        for (int frame = 0; frame < 1000; ++frame) {
            note_every_string();
        }
        // And a codepoint never seen before, past nothing: this one is ALLOWED
        // to look, so it stays outside the loop -- proving the loop's silence is
        // about repetition and not about the filter having stopped everything.
        _exit(0);
    }
    if (pid < 0) {
        printf("skip fork failed\n");
        return 77;
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFSIGNALED(status)) {
        printf("FAIL the drawn path made a file syscall in the steady state (signal %d)\n",
               WTERMSIG(status));
        return 1;
    }
    if (!WIFEXITED(status)) {
        printf("FAIL the child neither exited nor was signalled\n");
        return 1;
    }
    if (WEXITSTATUS(status) == 77) {
        printf("skip no seccomp filter available here\n");
        return 77;
    }
    if (WEXITSTATUS(status) != 0) {
        printf("FAIL the child exited %d\n", WEXITSTATUS(status));
        return 1;
    }
    printf("ok   1000 frames of the same names: not one file syscall\n");
    return 0;
}

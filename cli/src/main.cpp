// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// vocem -- inspect what the daemon is publishing.
//
// This is the same read path the layer uses, so if the CLI shows the right thing
// the in-game side is looking at the right thing too. That makes it the cheapest
// way to tell a daemon problem apart from a rendering problem.

#include <unistd.h>

#include <cstdio>
#include <cstring>

#include "vocem/shared_state.h"
#include "vocem/shm.h"

namespace {

void print_snapshot(const vocem::Snapshot& snapshot) {
    switch (snapshot.status) {
        case vocem::DaemonStatus::WaitingForDiscord:
            std::printf("Waiting for Discord\n");
            return;
        case vocem::DaemonStatus::Authorising:
            std::printf("Authorising: accept the prompt in Discord\n");
            return;
        case vocem::DaemonStatus::AuthorisationRefused:
            std::printf("Authorisation refused: reconnect from the settings window\n");
            return;
        case vocem::DaemonStatus::Connected:
            break;
    }
    if (!snapshot.in_channel) {
        std::printf("Discord: connected, not in a voice channel\n");
        return;
    }

    std::printf("Channel: %s (%u participant%s)\n", snapshot.channel_name, snapshot.user_count,
                snapshot.user_count == 1 ? "" : "s");
    // The state sits in a fixed cell *before* the name, not padded after it:
    // printf's %-40s counts bytes, and a name with an emoji in it is more bytes
    // than columns, so a column after it would drift. Three ASCII marks, one
    // per state, readable in any terminal with no colour involved; the legend
    // prints once, only when a mark did.
    bool any_marks = false;
    for (uint32_t i = 0; i < snapshot.user_count; ++i) {
        const vocem::User& user = snapshot.users[i];
        const char speaking = (user.flags & vocem::kFlagSpeaking) ? '>' : ' ';
        const char muted = (user.flags & vocem::kFlagMuted) ? 'm' : ' ';
        const char deafened = (user.flags & vocem::kFlagDeafened) ? 'd' : ' ';
        any_marks = any_marks || speaking != ' ' || muted != ' ' || deafened != ' ';
        std::printf("  %c%c%c  %s%s\n", speaking, muted, deafened, user.name,
                    (user.flags & vocem::kFlagSelf) ? "  (you)" : "");
    }
    if (any_marks) {
        std::printf("  (> speaking, m muted, d deafened)\n");
    }
}

// Why there is no snapshot to print. A reader that met an object at the
// segment's name and refused it (not this user's, not a regular file, or open
// to others: segment_trust_problem in vocem/shm.h) has not met "no daemon",
// and says which refusal it was: a refusal must not read like silence.
void say_nothing_to_read(std::FILE* out, const vocem::StateReader& reader) {
    if (const char* refusal = reader.refusal()) {
        std::fprintf(out, "the shared state segment was refused: %s\n", refusal);
    } else {
        std::fprintf(out, "vocemd is not running (no shared state segment)\n");
    }
}

}  // namespace

int main(int argc, char** argv) {
    const bool watch = argc > 1 && (std::strcmp(argv[1], "--watch") == 0 ||
                                    std::strcmp(argv[1], "-w") == 0);
    // Anything else on the command line is refused rather than shrugged off:
    // this is the tool somebody runs while diagnosing, and `vocem --help`,
    // `vocem -h` or a typo of --watch silently printing one snapshot and
    // exiting 0 is the wrong failure for exactly that person. The usage text
    // doubles as the help.
    if (argc > 1 && !watch) {
        const bool asked = std::strcmp(argv[1], "--help") == 0 || std::strcmp(argv[1], "-h") == 0;
        std::fprintf(asked ? stdout : stderr,
                     "usage: vocem [--watch|-w]\n"
                     "Prints the voice state vocemd is publishing; --watch follows it.\n");
        return asked ? 0 : 2;
    }

    vocem::StateReader reader;
    if (!reader.open()) {
        say_nothing_to_read(stderr, reader);
        return 1;
    }

    // Only a terminal is cleared: piped into a file, the escape sequences would
    // be lines of their own.
    const bool clear_screen = watch && isatty(1);

    do {
        // A mapping outlives the segment's name (vocem/shm.h): without asking,
        // --watch would go on printing the state an old daemon left and never
        // see the one a restart made. Asked on every tick, which is what the
        // reader's own note asks of its callers.
        if (reader.valid() && !reader.still_current()) {
            reader.close();
        }
        if (!reader.valid() && !reader.open()) {
            if (!watch) {
                say_nothing_to_read(stderr, reader);
                return 1;
            }
            if (clear_screen) {
                std::printf("\033[2J\033[H");
            }
            say_nothing_to_read(stdout, reader);
            std::fflush(stdout);
            usleep(250 * 1000);
            continue;
        }
        vocem::Snapshot snapshot;
        if (!reader.read(snapshot)) {
            std::fprintf(stderr, "could not read a consistent snapshot\n");
            return 1;
        }
        if (clear_screen) {
            std::printf("\033[2J\033[H");  // clear, home
        }
        print_snapshot(snapshot);
        if (watch) {
            std::fflush(stdout);
            usleep(250 * 1000);
        }
    } while (watch);

    return 0;
}

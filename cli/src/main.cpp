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
    // than columns, so the old marks column drifted on exactly the names Discord
    // is full of. Three ASCII marks, one per state, readable in any terminal
    // with no colour involved; the legend prints once, only when a mark did.
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
        std::fprintf(stderr, "vocemd is not running (no shared state segment)\n");
        return 1;
    }

    do {
        vocem::Snapshot snapshot;
        if (!reader.read(snapshot)) {
            std::fprintf(stderr, "could not read a consistent snapshot\n");
            return 1;
        }
        if (watch) {
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

// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A channel that is not there, published into the shared segment.
//
// The overlay draws whatever the daemon is publishing, so looking at it with twenty
// people in a channel meant being in a channel with twenty people. This writes a
// synthetic one and waits, so the in-game drawing can be seen at any number of them
// with Discord closed. The configuration window is not a consumer of it: its
// previews draw a fixed roster of four and never the live channel.
//
// A development aid, not part of the product: it is never installed, and it should
// be run against a private /dev/shm -- `bwrap --dev-bind / / --tmpfs /dev/shm` --
// so that it cannot be mistaken for the daemon by a game that is running.

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <ctime>

#include <unistd.h>

#include "vocem/display.h"
#include "vocem/note.h"
#include "private_shm.h"
#include "vocem/shm.h"

namespace {

// The same clock the daemon stamps a notification with and the layer measures its
// age against. Stamped now, so the toast is drawn rather than treated as one that
// arrived before the machine was switched on.
double monotonic_seconds() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) * 1e-9;
}

}  // namespace

int main(int argc, char** argv) {
    // This tool publishes a channel that does not exist, so it may only ever run
    // in a private /dev/shm. Verified rather than assumed: run on a live session
    // it would write its fake participants into the running daemon's own segment
    // and unlink the name the daemon publishes under -- which is precisely what
    // happened once, in two of the owner's games at the same time
    // (tests/private_shm.h carries the post-mortem).
    {
        char shm_holds[256] = {0};
        if (!vocem_test::shm_is_private(shm_holds, sizeof(shm_holds))) {
            vocem_test::shm_explain_refusal(shm_holds);
            std::fprintf(stderr,
                         "vocem_fake_state: refusing to publish outside a private /dev/shm\n");
            return 1;
        }
    }
    const uint32_t asked = argc > 1 ? static_cast<uint32_t>(std::atoi(argv[1])) : 3;
    // A second argument names the channel, so that what the overlay does with a
    // name full of emoji and punctuation can be looked at in a real game rather
    // than reasoned about.
    const char* channel = argc > 2 ? argv[2] : "Preview";

    vocem::StateWriter writer;
    if (!writer.open()) {
        std::fprintf(stderr, "fake_state: no shared segment\n");
        return 1;
    }

    writer.publish([&](vocem::SharedState& state) {
        state.connected = 1;
        state.in_channel = 1;
        // As the daemon would: the overlay sizes itself by the display, and a
        // preview fed a zero here would fall back to sizing by the window --
        // the rubber-band this field exists to end.
        state.display_height = vocem::display_height();
        state.status = static_cast<uint32_t>(vocem::DaemonStatus::Connected);
        std::snprintf(state.channel_name, sizeof(state.channel_name), "%s", channel);
        state.user_count = asked > vocem::kMaxUsers ? vocem::kMaxUsers : asked;

        // Names with something other than letters in them, because that is what
        // was broken and what is worth looking at: an emoji, a symbol that is not
        // an emoji, and the punctuation a channel name is full of. Beyond the
        // three, one of each state.
        // Invented names, not anybody's: this fixture is drawn into the frame the
        // README publishes, and a real display name has no business there.
        static const char* const known[] = {"Player One 🎮", "Player Two ⭐",
                                            "Player Three ・ｘ"};
        for (uint32_t i = 0; i < state.user_count; ++i) {
            state.users[i].id = 100 + i;
            // A hash, so the overlay looks for a real file rather than for one of
            // Discord's default pictures: what is being exercised is the wait for a
            // download that has not finished.
            // Hex, because that is what the overlay accepts as a hash -- anything
            // else is read as "this account has no picture" and sends it looking
            // for one of Discord's defaults instead.
            std::snprintf(state.users[i].avatar_hash, sizeof(state.users[i].avatar_hash),
                          "deadbeef%04u", i);
            if (i < 3) {
                std::snprintf(state.users[i].name, sizeof(state.users[i].name), "%s", known[i]);
                if (i == 0) state.users[i].flags |= vocem::kFlagSpeaking;
                if (i == 2) state.users[i].flags |= vocem::kFlagMuted;
                // The first participant is the user. Without this the fake
                // channel could exercise exactly one of the tray icon's five
                // states -- the fallback -- and the whole point of this tool
                // is looking at the thing while it runs.
                if (i == 0) state.users[i].flags |= vocem::kFlagSelf;
            } else {
                std::snprintf(state.users[i].name, sizeof(state.users[i].name), "Participant %u",
                              i + 1);
                if (i % 4 == 3) state.users[i].flags |= vocem::kFlagDeafened;
            }
        }

        state.notification.serial = 1;
        state.notification.user_id = 100;
        state.notification.received = monotonic_seconds();
        std::snprintf(state.notification.title, sizeof(state.notification.title), "Someone");
        // The body field in the segment is always empty by design (entry 64): the
        // words travel in a segment of their own, published below, so a toast
        // drawn from this fixture has to carry them the same way the daemon does
        // -- otherwise what one looks at is a box with no message in it.
    });

    vocem::NoteWriter note;
    note.publish(1, "wrote you a message while you were playing");

    // The segment lives as long as this process does -- and the channel talks.
    // The word moves to the next participant every couple of seconds, with a gap
    // of silence in between, so the ring's rise, hold and fall can be watched in
    // a real game instead of reasoned about. Two seconds of speech, because the
    // hold is 120 ms and a toggle faster than it would show only the hold.
    uint32_t turn = 0;
    for (;;) {
        ::usleep(2'000'000);
        writer.publish([&](vocem::SharedState& state) {
            for (uint32_t i = 0; i < state.user_count; ++i) {
                state.users[i].flags &= ~vocem::kFlagSpeaking;
            }
        });
        ::usleep(400'000);
        writer.publish([&](vocem::SharedState& state) {
            if (state.user_count > 0) {
                turn = (turn + 1) % state.user_count;
                state.users[turn].flags |= vocem::kFlagSpeaking;
            }
        });
    }
    return 0;
}

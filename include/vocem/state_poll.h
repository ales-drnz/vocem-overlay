// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Attach to the daemon's state segment on a cadence, and notice when it dies.
// The one copy of this loop for both injected paths (tests/shm_reattach.cpp).
//
// Injected-code rules hold here: no allocation, no locks, no file syscalls
// except the deliberate ones on the cadence below. The log is a plain function
// pointer so each path keeps its own sink without this header knowing either.

#ifndef VOCEM_STATE_POLL_H
#define VOCEM_STATE_POLL_H

#include <cstdio>

#include "vocem/clock.h"
#include "vocem/shm.h"

namespace vocem {

class StatePoll {
public:
    // How often the segment's NAME is asked about: to find a daemon that started
    // after the game and to notice one that stopped. Paced by the clock, not by
    // a count of presents, so how long a game holds resources for a gone daemon
    // does not depend on its frame rate. One syscall a second; the clock is the
    // vDSO's.
    static constexpr double kCadenceSeconds = 1.0;

    // How long a daemon has to stay gone before the caller is told to hand back
    // what it holds. Drawing stops at the first poll that finds nothing; the atlas
    // is dropped only after two more looks, because a `systemctl --user restart
    // vocemd` is caught as Replaced only when the new object is already there,
    // and rasterising the atlas again is expensive. A Quit costs two seconds of
    // memory nobody is looking at.
    static constexpr double kReleaseAfterSeconds = 2.0;

    using LogFn = void (*)(const char* line);
    explicit StatePoll(LogFn log) : log_(log) {}

    // The snapshot is this process's own copy, so the pointer is mutable on
    // purpose: the GL path fills the toast's body into it from the note segment.
    //
    // Two copies: a read lands in the scratch half and becomes current only when
    // the seqlock says it is whole; a read that met a publish in progress
    // (StateReader::Read::Busy) hands back the last whole snapshot instead of
    // nothing (tests/state_poll_contention.cpp).
    Snapshot* poll() {
        const double now = monotonic_seconds();
        if (!reader_.valid()) {
            if (now < next_ask_) {
                return nullptr;
            }
            next_ask_ = now + kCadenceSeconds;
            if (!reader_.open()) {
                // Something at the name that is not the daemon's: said once,
                // not every second, and said again only after an attachment.
                if (const char* refusal = reader_.refusal(); refusal && !said_refusal_) {
                    said_refusal_ = true;
                    char line[192];
                    std::snprintf(line, sizeof(line), "refusing the object at the state "
                                  "segment's name: %s", refusal);
                    say(line);
                }
                // Still nothing at the name. Hand back what this process holds
                // once a daemon has been gone long enough to mean it.
                if (gone_since_ > 0.0 && now - gone_since_ >= kReleaseAfterSeconds) {
                    gone_since_ = 0.0;
                    left_ = true;
                }
                return nullptr;
            }
            gone_since_ = 0.0;
            said_refusal_ = false;
            attached_afresh();
            say("attached to the vocemd state segment");
        } else if (now >= next_ask_) {
            // The same cadence, pointed the other way: a mapping outlives the
            // segment's name, so ask the name what it means now.
            next_ask_ = now + kCadenceSeconds;
            switch (reader_.segment_state()) {
                case StateReader::Segment::Current:
                    break;
                case StateReader::Segment::Replaced:
                    // A daemon came back: only the mapping moves, here on this
                    // present, so the caller never concludes the daemon is gone
                    // and the font atlas is kept.
                    reader_.close();
                    attached_afresh();
                    if (reader_.open()) {
                        say("the daemon was replaced: attached to the new segment");
                        break;
                    }
                    // Unlinked between the two calls: fall through to the same
                    // waiting the Gone case does.
                    say("the daemon was replaced and the new segment went with it: detaching");
                    gone_since_ = now;
                    return nullptr;
                case StateReader::Segment::Gone:
                    say("the daemon's state segment is gone: detaching");
                    reader_.close();
                    attached_afresh();
                    gone_since_ = now;
                    return nullptr;
            }
        }
        Snapshot& scratch = snapshots_[current_ ^ 1];
        switch (reader_.read_state(scratch)) {
            case StateReader::Read::Ok:
                current_ ^= 1;
                have_good_ = true;
                return &snapshots_[current_];
            case StateReader::Read::Busy:
                // A publish outlasted the read's wait (a writer descheduled or
                // dead mid-publish): the previous state beats no overlay. Said
                // once per segment, so a writer stuck odd shows in the log.
                if (!said_busy_) {
                    said_busy_ = true;
                    say("a publish outlasted the read: showing the previous state");
                }
                return have_good_ ? &snapshots_[current_] : nullptr;
            case StateReader::Read::ForeignAbi: {
                // Refused and SAID, on a flag of its own so contention cannot
                // spend its line; re-armed with every attachment.
                have_good_ = false;
                if (!said_abi_) {
                    said_abi_ = true;
                    char line[192];
                    std::snprintf(line, sizeof(line),
                                  "the state segment speaks ABI version %u and this library "
                                  "speaks %u: refusing it (the daemon and this library come "
                                  "from different releases)",
                                  reader_.abi_version(), kAbiVersion);
                    say(line);
                }
                return nullptr;
            }
            case StateReader::Read::NotAttached:
                break;
        }
        return nullptr;
    }

    // Whether a daemon's segment is attached right now. The Vulkan layer asks
    // before building its renderer, whose `needs_init` otherwise depends only on
    // the swapchain and the verdict, so it would build the whole backend with no
    // daemon to draw.
    bool attached() const { return reader_.valid(); }

    // True exactly once after a daemon this reader HAD has stayed away: the
    // caller's moment to hand back what it holds on that daemon's behalf (the
    // renderer's objects, the font atlas, a texture per face) (entry 146). A
    // process that never found a daemon never sees it; a *replaced* daemon never
    // sets it, because the atlas would only be rasterised again a second later.
    bool daemon_left() {
        const bool left = left_;
        left_ = false;
        return left;
    }

private:
    // A new segment, or none: nothing read from the last one is carried over,
    // and both refusals are news again.
    void attached_afresh() {
        have_good_ = false;
        said_abi_ = false;
        said_busy_ = false;
    }

    void say(const char* line) {
        if (log_) {
            log_(line);
        }
    }

    StateReader reader_;
    // The last whole read and the scratch the next one lands in; `current_`
    // says which is which.
    Snapshot snapshots_[2];
    int current_ = 0;
    bool have_good_ = false;
    // Zero, so the first present asks rather than waiting out a cadence.
    double next_ask_ = 0.0;
    // When the name stopped meaning anything, or 0 while it does; a process
    // that never attached never releases what it never built.
    double gone_since_ = 0.0;
    bool left_ = false;
    bool said_abi_ = false;
    bool said_busy_ = false;
    bool said_refusal_ = false;
    LogFn log_;
};

}  // namespace vocem

#endif  // VOCEM_STATE_POLL_H

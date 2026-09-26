// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Attach to the daemon's state segment on a cadence, and notice when it dies.
//
// One spelling. This loop existed twice -- GlOverlay::poll_state and the
// layer's StateSource::poll, identical to the character apart from the log
// macro -- and the cadence is exactly the kind of number somebody tunes on one
// side (entry 33's shape). tests/shm_reattach.cpp exercises the
// still_current() half and its header speaks of "the injected paths" in the
// plural; this is the one place both of them now are.
//
// Injected-code rules hold here: no allocation, no locks, no file syscalls
// except the deliberate ones on the cadence below. The log is a plain function
// pointer so each path keeps its own macro (pid-stamped file logging on GL,
// stderr on Vulkan) without this header knowing either.

#ifndef VOCEM_STATE_POLL_H
#define VOCEM_STATE_POLL_H

#include <cstdio>

#include "vocem/clock.h"
#include "vocem/shm.h"

namespace vocem {

class StatePoll {
public:
    // How often the segment's NAME is asked about: to find a daemon that
    // started after the game, and to notice one that stopped. A syscall per
    // frame is exactly the cost this design exists to avoid, so it is paced --
    // and paced by the clock rather than by a count of presents, because what
    // this now decides is how long a game goes on holding an atlas and a
    // texture per face for a daemon that has gone. The count was 300 presents,
    // which is 2 s at 144 frames and 10 s at thirty: the same Quit felt
    // different depending on what the game was doing. One syscall a second in a
    // process the overlay draws in; the clock itself is the vDSO's and not a
    // syscall.
    static constexpr double kCadenceSeconds = 1.0;

    // How long a daemon has to stay gone before the caller is told to hand back
    // what it holds. The drawing stops at the first poll that finds nothing --
    // the promise entry 146 makes, "within a second", is about the screen and is
    // unaffected -- but throwing the font atlas away costs 133 ms to rasterise
    // again, so it is worth being sure first.
    //
    // A `systemctl --user restart vocemd` unlinks, exits, and is started again;
    // segment_state() catches that as Replaced whenever the new object is
    // already there when we look, which is most of the time but not a property
    // anybody can promise -- it depends on where the once-a-second look lands in
    // a gap of a couple of hundred milliseconds. This covers the rest: two more
    // looks before anything is thrown away. A daemon that was told to quit is
    // still gone two seconds later, so what it costs the Quit case is two
    // seconds of memory nobody is looking at.
    static constexpr double kReleaseAfterSeconds = 2.0;

    using LogFn = void (*)(const char* line);
    explicit StatePoll(LogFn log) : log_(log) {}

    // The snapshot is this process's own copy, so the pointer is mutable on
    // purpose: the GL path fills the toast's body into it from the note
    // segment, which reaches nobody else.
    //
    // Two copies, not one. A read lands in the scratch half and becomes the
    // current one only when the seqlock says it is whole; a read that met a
    // publish still in progress (StateReader::Read::Busy) hands back the last
    // whole snapshot instead of nothing. The single copy this used to be was
    // both the target of the read and the thing returned, so a read that lost
    // to a publish left it half overwritten and returned nullptr -- a blank
    // frame, measured by tests/state_poll_contention.cpp at 39704 in a million
    // polls beside a writer publishing flat out.
    Snapshot* poll() {
        const double now = monotonic_seconds();
        if (!reader_.valid()) {
            if (now < next_ask_) {
                return nullptr;
            }
            next_ask_ = now + kCadenceSeconds;
            if (!reader_.open()) {
                // Still nothing at the name. Hand back what this process holds
                // once a daemon has been gone long enough to mean it.
                if (gone_since_ > 0.0 && now - gone_since_ >= kReleaseAfterSeconds) {
                    gone_since_ = 0.0;
                    left_ = true;
                }
                return nullptr;
            }
            gone_since_ = 0.0;
            attached_afresh();
            say("attached to the vocemd state segment");
        } else if (now >= next_ask_) {
            // The same cadence, pointed the other way: a mapping outlives the
            // segment's name, so a daemon that stopped -- or stopped and came
            // back -- leaves this reader on orphaned pages it would trust
            // forever. Ask the name what it means now.
            next_ask_ = now + kCadenceSeconds;
            switch (reader_.segment_state()) {
                case StateReader::Segment::Current:
                    break;
                case StateReader::Segment::Replaced:
                    // A daemon came back while this game was running. The pages
                    // are stale and the mapping has to move; nothing else does.
                    // The font atlas is this process's, not the daemon's, and
                    // rasterising it again is the 133 ms entry 145 exists to
                    // stop paying -- so the reattachment is made here, on this
                    // present, rather than by dropping to the branch above and
                    // letting a caller conclude the daemon is gone.
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
                // A publish outlasted the read's wait (a daemon descheduled
                // mid-publish, or one that died there): the previous frame's
                // state is a better answer than no overlay. Said once per
                // segment, so a writer stuck odd is visible in the log.
                if (!said_busy_) {
                    said_busy_ = true;
                    say("a publish outlasted the read: showing the previous state");
                }
                return have_good_ ? &snapshots_[current_] : nullptr;
            case StateReader::Read::ForeignAbi: {
                // Refused and SAID, on a flag of its own. It shared one flag
                // with contention, so the first busy read spent the only line
                // and a foreign ABI met later was silent -- a reader refusing
                // looks exactly like "no daemon" from outside (entry 55).
                // Re-armed with every attachment: a new segment is news.
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

    // Whether a daemon's segment is attached right now.
    //
    // The Vulkan layer asks before building its renderer, and has to: its
    // `needs_init` is decided from the swapchain and the application verdict
    // alone, so a game the overlay is allowed in built the whole backend --
    // 133 ms of atlas and 80 MB of pixels -- with no daemon running at all and
    // nothing to put in it. The OpenGL path never had that door: its draw()
    // returns at the poll, above ensure_backend().
    bool attached() const { return reader_.valid(); }

    // True exactly once after a daemon this reader HAD has stayed away: the
    // caller's moment to hand back everything it is holding on that daemon's
    // behalf -- the renderer's objects, the font atlas, a texture per face. Not
    // drawing is not enough, and measured before this existed a game held all of
    // it for the rest of its life (entry 146).
    //
    // Here rather than in each path because both need exactly this transition,
    // and two copies of the bookkeeping is the shape entry 33 is about. A
    // process that never found a daemon has nothing to hand back and never sees
    // it; a daemon that was *replaced* never sets it, because the atlas it would
    // throw away is the one the next second would rasterise again.
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
    // When the name stopped meaning anything, or 0 while it does. A process
    // that never attached leaves this at 0 and so never releases what it never
    // built.
    double gone_since_ = 0.0;
    bool left_ = false;
    bool said_abi_ = false;
    bool said_busy_ = false;
    LogFn log_;
};

}  // namespace vocem

#endif  // VOCEM_STATE_POLL_H

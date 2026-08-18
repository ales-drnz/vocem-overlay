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

#include "vocem/shm.h"

namespace vocem {

class StatePoll {
public:
    // The daemon may start after the game, so a failed open is retried -- but
    // only every so many presents, because a syscall per frame is exactly the
    // kind of cost this design exists to avoid. Five seconds at sixty.
    static constexpr int kCadencePresents = 300;

    using LogFn = void (*)(const char* line);
    explicit StatePoll(LogFn log) : log_(log) {}

    // The snapshot is this process's own copy, so the pointer is mutable on
    // purpose: the GL path fills the toast's body into it from the note
    // segment, which reaches nobody else.
    Snapshot* poll() {
        if (!reader_.valid()) {
            if (retry_countdown_ > 0) {
                --retry_countdown_;
                return nullptr;
            }
            retry_countdown_ = kCadencePresents;
            if (!reader_.open()) {
                return nullptr;
            }
            say("attached to the vocemd state segment");
        } else if (--retry_countdown_ <= 0) {
            // The same cadence, pointed the other way: a mapping outlives the
            // segment's name, so a daemon that stopped -- or stopped and came
            // back -- leaves this reader on orphaned pages it would trust
            // forever. Ask the name whether it still means our mapping; if
            // not, drop it and let the branch above find the living one.
            retry_countdown_ = kCadencePresents;
            if (!reader_.still_current()) {
                say("state segment replaced or gone: detaching");
                reader_.close();
                return nullptr;
            }
        }
        if (!reader_.read(snapshot_)) {
            // Once, not per frame: a reader refusing a foreign ABI looks
            // exactly like "no daemon" from outside (entry 55's silence), so
            // the refusal has to reach the log -- and it used to on one path
            // only.
            if (!said_read_failure_) {
                said_read_failure_ = true;
                say("state read failed (abi mismatch or writer contention)");
            }
            return nullptr;
        }
        return &snapshot_;
    }

private:
    void say(const char* line) {
        if (log_) {
            log_(line);
        }
    }

    StateReader reader_;
    Snapshot snapshot_;
    int retry_countdown_ = 0;
    bool said_read_failure_ = false;
    LogFn log_;
};

}  // namespace vocem

#endif  // VOCEM_STATE_POLL_H

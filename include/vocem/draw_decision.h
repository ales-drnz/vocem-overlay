// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Whether the overlay belongs in this process, according to the configuration
// as it stands -- asked every frame, cheaply, by both injection paths.
//
// looks_like_game() is decided once, because a process does not become a game
// halfway through. What can change while the process runs is the user's two
// lists, and they are only walked again when they differ from the ones already
// answered for; the steady-state cost is two string comparisons per frame.
//
// This object is the one spelling of that policy. The OpenGL side had it as a
// private method; the Vulkan side had a function-local static decided at the
// first present and never again -- so the per-application switch in the window
// acted live in one path and only at the next game start in the other, while
// the Applications page promised "within a couple of seconds" for both. Two
// spellings of one policy is the entry-33 shape, and this is where they had
// already diverged.

#ifndef VOCEM_DRAW_DECISION_H
#define VOCEM_DRAW_DECISION_H

#include <string>

#include "vocem/apps.h"
#include "vocem/config.h"

namespace vocem {

class DrawDecision {
public:
    // Recomputes only on the first call or when either list changed. Returns
    // true when there is something to say: the first computation, and every
    // later one that came out differently -- the caller's moment to log the
    // evidence, the verdict and its reason, in its own voice.
    //
    // It used to return `first` alone, so a verdict that *changed* was silent.
    // Somebody ticks a running game off the Applications page, the overlay
    // leaves within two seconds exactly as the page promises, and nothing is
    // written to the log, to the journal, or anywhere `vocem-why` reads -- in a
    // project whose rule is that a component which decides not to act says why.
    // The recomputation is already rare (only when a list was edited), so
    // answering honestly costs one comparison and at most one line per edit.
    bool refresh(const Config& config) {
        if (decided_ && config.hidden_apps == hidden_ && config.shown_apps == shown_) {
            return false;
        }
        const bool first = !decided_;
        const bool was = allowed_;
        decided_ = true;
        hidden_ = config.hidden_apps;
        shown_ = config.shown_apps;
        allowed_ = draw_here(config.hidden_apps, config.shown_apps);
        return first || allowed_ != was;
    }

    bool allowed() const { return allowed_; }

private:
    std::string hidden_;
    std::string shown_;
    bool allowed_ = false;
    bool decided_ = false;
};

}  // namespace vocem

#endif  // VOCEM_DRAW_DECISION_H

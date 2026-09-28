// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What a drawing process keeps track of the same way on both injected paths:
// the Flatpak bridge, the per-frame decision, the journal, the Debug
// counters, the "said once" font reasons, a toast's words and ImGui's
// DeltaTime.
//
// It does NOT hold the sequencing: the layer does no file work inside
// vkQueuePresentKHR and defers it to its post-present phase (rules 8 and 10),
// while the GL hook spends a bounded inline budget. Each function says which
// kind of work it does; where in a frame that is allowed is the caller's
// knowledge.

#ifndef VOCEM_OVERLAY_SESSION_H
#define VOCEM_OVERLAY_SESSION_H

#include <cstdint>

#include "vocem/config.h"
#include "vocem/draw_decision.h"
#include "vocem/note.h"

namespace vocem {

class OverlaySession {
public:
    // `api` is the word the record and the journal carry ("opengl", "vulkan");
    // `tag` is the log's ("vocem/gl", "vocem").
    OverlaySession(const char* api, const char* tag) : api_(api), tag_(tag) {}

    // Once per process, before the first thing that derives a path: inside a
    // Flatpak game the segment, settings and avatar cache are across the
    // sandbox (vocem/flatpak.h). Logs either failure, which is otherwise
    // invisible. File syscalls, once.
    void enter_flatpak_bridge_once();

    // Whether this frame should carry the overlay: the lists and the verdict
    // (vocem/draw_decision.h) AND the master switch. Callers ask it whole, never
    // `enabled && decide()`: a short-circuit skips telling the bridge.
    // Logs the evidence whenever the verdict is computed or changes, and tells
    // the daemon across the bridge whether this sandbox is drawing. No file work
    // unless the bridge answer changed (one open and one write then).
    bool decide(const Config& config);

    // The session's journal (vocem/journal.h), opened at the first frame this
    // process draws and never again; the verdict's reason is its first line.
    // File work.
    void journal_begin_once();

    // The Debug section's counters: presents the overlay was willing to draw
    // in, and frames it painted. frame_seen() rewrites the stat file at most
    // once every five seconds (two file syscalls and a rename).
    void frame_seen();
    void frame_drawn() { ++frames_drawn_; }

    // Why there are no colour emoji, and why the text is in the built-in font
    // rather than the one the settings name: each said once per change, never
    // per frame (entry 38).
    void say_font_statuses();

    // The words of the toast with this serial, from the note segment
    // (vocem/note.h): opened at most once per message, closed before
    // returning. Empty when nothing was published, which is logged once per
    // message: a toast without words otherwise looks like success.
    const char* note_words(uint64_t serial);
    // The toast is over: so are the words, out of this process's memory.
    void note_forget() { note_.forget(); }

    // ImGui's DeltaTime for a frame presented at `now` (the frame's one clock):
    // the measured time since the previous frame, 1/60 for the first, and never
    // a zero or negative step for a stalled game.
    float delta_time(double now);
    // The next frame has no predecessor (after a release, a device change), so
    // the absence is not one animation step.
    void reset_clock() { last_frame_seconds_ = 0.0; }

private:
    const char* api_;
    const char* tag_;
    bool bridge_asked_ = false;
    bool journal_open_ = false;
    DrawDecision decision_;
    long frames_seen_ = 0;
    long frames_drawn_ = 0;
    double last_stat_seconds_ = 0.0;
    // Literals' addresses: the comparison is the "say it once" and a change
    // says it again.
    const char* emoji_status_said_ = nullptr;
    const char* font_status_said_ = nullptr;
    NoteReader note_;
    uint64_t said_empty_note_ = 0;
    double last_frame_seconds_ = 0.0;
};

}  // namespace vocem

#endif  // VOCEM_OVERLAY_SESSION_H

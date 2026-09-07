// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What a drawing process keeps track of the same way on both injected paths.
//
// vocem/state_poll.h and vocem/draw_decision.h were the first two pieces the
// OpenGL library and the Vulkan layer stopped spelling twice. This is the
// rest of what they had in common, identical to the character or nearly, and
// free to drift the way the state loop had (entry 127: one side said why a
// read failed and the other did not):
//
//   * entering the Flatpak bridge once, and saying so either way it can fail;
//   * the per-frame decision -- refresh, the evidence logged once, the daemon
//     told across the bridge whether this sandbox is drawing;
//   * the journal opened at the first drawn frame, with the verdict's reason;
//   * the Debug section's frame counters and their five-second stat file;
//   * the "said once" reasons for no colour emoji and for the built-in font;
//   * the words of a toast, fetched once per message, with the one log line
//     for a message that arrived without them;
//   * ImGui's DeltaTime from the frame's clock, forgotten across a release.
//
// What this deliberately does NOT hold is the sequencing. The two paths call
// these pieces in different phases because their rules differ: the layer may
// do no file work inside vkQueuePresentKHR and keeps the stat write, the note
// read and the journal for its post-present phase (rules 8 and 10), while the
// GL hook has no post-present phase and spends a bounded inline budget (entry
// 52). Each function says which kind of work it does; where in a frame that is
// allowed is the caller's knowledge, as it always was.
//
// The consolidation found one place where the two had already parted: the GL
// side told the daemon `enabled && allowed`, the Vulkan side `allowed` alone,
// so a Flatpak game with the master switch off went on receiving the channel
// and every face across the bridge from a daemon that believed it was drawing
// (entry 138). decide() is that sentence's one spelling now.

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
    // Flatpak game the segment, the settings and the avatar cache are all on
    // the far side of the sandbox (vocem/flatpak.h). Says so either way it can
    // fail, because the failure is invisible otherwise: a game whose overlay
    // never found the bridge behaves exactly like one the overlay was never
    // asked to draw in. File syscalls, once.
    void enter_flatpak_bridge_once();

    // Whether the overlay is allowed in this process by the lists and the
    // verdict (vocem/draw_decision.h: the lists re-walked only when edited).
    // Logs the evidence the first time it is computed -- a game that is missed
    // has to be a case somebody can read off one line -- and tells the daemon
    // across the bridge whether this sandbox is DRAWING, which is the answer
    // and the master switch together: a sandbox that is not drawing is served
    // its settings and a cleared state, nothing else. No file work unless the
    // bridge answer changed (one open and one write then).
    bool decide(const Config& config);

    // The session's journal (vocem/journal.h), opened at the first frame this
    // process draws and never again; the verdict's reason is its first line.
    // File work.
    void journal_begin_once();

    // The Debug section's counters: presents the overlay was willing to draw
    // in, and frames it painted. frame_seen() rewrites the stat file at most
    // once every five seconds -- two file syscalls and a rename then, two
    // integers otherwise.
    void frame_seen();
    void frame_drawn() { ++frames_drawn_; }

    // Why there are no colour emoji, and why the text is in the built-in font
    // rather than the one the settings name: each said once per change, never
    // per frame. A feature that quietly does not happen reads exactly like one
    // nobody asked for (entry 38).
    void say_font_statuses();

    // The words of the toast with this serial, from the note segment
    // (vocem/note.h): opened at most once per message, closed before
    // returning. Empty when nothing was published, said once per message,
    // because a toast with a name and a face and no words is the one failure
    // this path has that looks exactly like success. File syscalls, once per
    // message.
    const char* note_words(uint64_t serial);
    // The toast is over: so are the words, out of this process's memory.
    void note_forget() { note_.forget(); }

    // ImGui's DeltaTime for a frame presented at `now` (the frame's one clock):
    // the measured time since the previous frame, 1/60 for the first, and never
    // a zero or negative step for a stalled game.
    float delta_time(double now);
    // The next frame has no predecessor: after a release, a device change, so
    // the frame that brings the overlay back does not measure the whole
    // absence as one animation step.
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

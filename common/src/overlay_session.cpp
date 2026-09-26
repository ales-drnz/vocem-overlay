// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// vocem/overlay_session.h: the injected code's shared bookkeeping, compiled
// once per width into vocem_common and linked into both injected libraries.
// Nothing here is reached at frame rate except decide() and delta_time(), and
// neither of those allocates. The logger it speaks through is its own object
// (overlay_log.cpp), so a file that only logs pulls none of this in.

#include "vocem/overlay_session.h"

#include <cstdio>

#include "vocem/apps.h"
#include "vocem/clock.h"
#include "vocem/flatpak.h"
#include "vocem/fonts.h"
#include "vocem/journal.h"
#include "vocem/overlay_log.h"

namespace vocem {

void OverlaySession::enter_flatpak_bridge_once() {
    if (bridge_asked_) {
        return;
    }
    bridge_asked_ = true;
    const char* id = flatpak_app_id();
    if (!id) {
        return;  // on the host, where everything is where it has always been
    }
    if (enter_flatpak_bridge()) {
        char root[512];
        bridge_root(root, sizeof(root));
        VOCEM_OVERLAY_LOG(tag_,
                          "inside the Flatpak sandbox of %s: state, settings and avatars come "
                          "from %s", id, root);
        return;
    }
    VOCEM_OVERLAY_LOG(tag_,
                      "inside the Flatpak sandbox of %s but could not ask vocemd for a bridge: "
                      "no XDG_RUNTIME_DIR, or its app directory is not writable. There will be "
                      "no overlay in this process.", id);
}

bool OverlaySession::decide(const Config& config) {
    if (decision_.refresh(config)) {
        // The evidence, not just the verdict. "not drawing in" whole is what
        // tests/gl_noop_quiet.cpp matches; the first probe matched "drawing in"
        // inside it and reported the opposite (entry 54).
        if (!decision_.allowed()) {
            VOCEM_OVERLAY_LOG(tag_, "not drawing in '%s': %s (%s)", process_name().c_str(),
                              looks_like_game() ? "on the hidden list"
                                                : "does not look like a game",
                              game_verdict().reason.c_str());
        } else if (config.enabled) {
            VOCEM_OVERLAY_LOG(tag_, "drawing in '%s': %s", process_name().c_str(),
                              game_verdict().reason.c_str());
        } else {
            // Allowed by the lists and switched off by the user: saying
            // "drawing in" here would be the log telling the opposite of what
            // the screen shows, which is the one thing it may not do.
            VOCEM_OVERLAY_LOG(tag_, "not drawing in '%s': the overlay is switched off (%s)",
                              process_name().c_str(), game_verdict().reason.c_str());
        }
    }
    // Inside a Flatpak, tell the daemon. It adopted this sandbox before the
    // settings could be read, because the settings arrive across the bridge;
    // this is where it learns whether the overlay is actually drawing here, and
    // whether to keep sending the channel and the faces at all. The master
    // switch is part of that answer: with it off nothing is drawn, and a daemon
    // told otherwise would mirror the channel into a sandbox showing nothing
    // (entry 138 -- the Vulkan side left the switch out of this sentence).
    const bool drawing = config.enabled && decision_.allowed();
    flatpak_bridge_drawing(drawing);
    // The whole answer, not half of it. Entry 138 made the two paths agree on
    // what the daemon is told and left the two CALL SITES spelling the master
    // switch themselves -- and the GL one spelled it `config.enabled &&
    // session_.decide(config)`, which short-circuits: with the overlay switched
    // off, decide() was never reached, so flatpak_bridge_drawing() was never
    // told, and it only writes on a change. A Flatpak game on the OpenGL path
    // therefore went on receiving the channel and every face from a daemon that
    // believed it was drawing -- entry 138's own defect, surviving on the other
    // side of entry 138's own fix. Returning the whole answer is what makes the
    // question unaskable by halves.
    return drawing;
}

void OverlaySession::journal_begin_once() {
    if (journal_open_) {
        return;
    }
    journal_begin(api_, process_name().c_str());
    if (!detail::journal_file()) {
        return;  // could not be opened this time; the next drawn frame asks again
    }
    journal_open_ = true;
    char note[300];
    std::snprintf(note, sizeof(note), "drawing: %s", game_verdict().reason.c_str());
    journal_note(note);
}

void OverlaySession::frame_seen() {
    ++frames_seen_;
    const double now = monotonic_seconds();
    if (now - last_stat_seconds_ >= 5.0) {
        last_stat_seconds_ = now;
        journal_stat(frames_seen_, frames_drawn_);
    }
}

void OverlaySession::say_font_statuses() {
    if (const char* status = fonts_emoji_status(); status != emoji_status_said_) {
        emoji_status_said_ = status;
        if (status) {
            VOCEM_OVERLAY_LOG(tag_, "no colour emoji: %s", status);
        }
    }
    // The overlay falls back to its own Inter, which looks like a setting that
    // was never applied unless the log says otherwise.
    if (const char* status = fonts_font_status(); status != font_status_said_) {
        font_status_said_ = status;
        if (status) {
            VOCEM_OVERLAY_LOG(tag_, "drawing in the built-in font: %s", status);
        }
    }
}

const char* OverlaySession::note_words(uint64_t serial) {
    const char* words = note_.body_for(serial);
    if (serial != 0 && words[0] == '\0' && said_empty_note_ != serial) {
        said_empty_note_ = serial;
        if (const char* refusal = note_.refusal()) {
            VOCEM_OVERLAY_LOG(tag_, "message %llu has no words here: the note segment was "
                              "refused, %s", static_cast<unsigned long long>(serial), refusal);
        } else {
            VOCEM_OVERLAY_LOG(tag_,
                              "message %llu has no words here: the note segment is empty or "
                              "unreachable from this process",
                              static_cast<unsigned long long>(serial));
        }
    }
    return words;
}

float OverlaySession::delta_time(double now) {
    // This used to be a hardcoded 1/60 on the GL side, defensible while nothing
    // animated and wrong once the panel did: a 144 Hz game ran the motion at
    // 2.4x and a 30 Hz one at half speed. The first frame has no predecessor,
    // and a stalled or hitching game must not feed ImGui a zero step.
    const double delta = last_frame_seconds_ > 0.0 ? now - last_frame_seconds_ : 1.0 / 60.0;
    last_frame_seconds_ = now;
    return delta > 0.0001 ? static_cast<float>(delta) : 1.0f / 60.0f;
}

}  // namespace vocem

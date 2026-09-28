// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Only decide() and delta_time() run at frame rate, and neither allocates.

#include "vocem/overlay_session.h"

#include <errno.h>

#include <cstdio>
#include <cstring>

#include "vocem/apps.h"
#include "vocem/clock.h"
#include "vocem/flatpak.h"
#include "vocem/fonts.h"
#include "vocem/journal.h"
#include "vocem/overlay_log.h"

namespace vocem {

namespace {

// When a journal that could not be created may be asked for again. The GL hook
// calls journal_begin_once() on every drawn frame, and each attempt walks the
// journal directory twice (entry 217, tests/journal_retry_cadence.cpp). A cache
// that refuses once almost always refuses for good. Process-wide, as the
// journal is: plain globals, constant initialisation.
constexpr double kJournalRetrySeconds = 30.0;
double g_journal_retry_at = 0.0;
bool g_journal_refusal_said = false;

}  // namespace

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
        // The evidence, not just the verdict. tests/gl_noop_quiet.cpp matches
        // "not drawing in" whole.
        if (!decision_.allowed()) {
            VOCEM_OVERLAY_LOG(tag_, "not drawing in '%s': %s (%s)", process_name().c_str(),
                              looks_like_game() ? "on the hidden list"
                                                : "does not look like a game",
                              game_verdict().reason.c_str());
        } else if (config.enabled) {
            VOCEM_OVERLAY_LOG(tag_, "drawing in '%s': %s", process_name().c_str(),
                              game_verdict().reason.c_str());
        } else {
            // Allowed by the lists, switched off by the user: not "drawing in".
            VOCEM_OVERLAY_LOG(tag_, "not drawing in '%s': the overlay is switched off (%s)",
                              process_name().c_str(), game_verdict().reason.c_str());
        }
    }
    // Inside a Flatpak, tell the daemon whether the overlay is drawing here, so
    // it stops mirroring the channel and faces into a sandbox that shows
    // nothing. The master switch is part of that answer, and returning the
    // whole answer keeps callers from asking half (entry 152).
    const bool drawing = config.enabled && decision_.allowed();
    flatpak_bridge_drawing(drawing);
    return drawing;
}

void OverlaySession::journal_begin_once() {
    if (journal_open_) {
        return;
    }
    const double now = monotonic_seconds();
    if (now < g_journal_retry_at) {
        return;
    }
    if (!journal_begin(api_, process_name().c_str())) {
        // Could not be created: asked again on the slow cadence above, and
        // said once.
        const int error = errno;
        g_journal_retry_at = now + kJournalRetrySeconds;
        if (!g_journal_refusal_said) {
            g_journal_refusal_said = true;
            VOCEM_OVERLAY_LOG(tag_, "no journal for this process: %s cannot take one (%s); "
                              "asking again every %.0f s", journal_dir().c_str(),
                              std::strerror(error), kJournalRetrySeconds);
        }
        return;
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
    const double delta = last_frame_seconds_ > 0.0 ? now - last_frame_seconds_ : 1.0 / 60.0;
    last_frame_seconds_ = now;
    return delta > 0.0001 ? static_cast<float>(delta) : 1.0f / 60.0f;
}

}  // namespace vocem

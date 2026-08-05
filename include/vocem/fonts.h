// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The overlay's typeface.
//
// ImGui's built-in font is ProggyClean, a 13-pixel bitmap. Scaling a bitmap is
// what the overlay did until now, and on a 3840x2160 display it is legible and
// ugly: the glyphs are magnified squares. Instead the atlas is rasterised from a
// real outline font at the pixel size the output actually needs.
//
// Rebuilding an atlas is not free and it destroys a texture the renderer may still
// be using, so it happens at exactly one point: the post-present phase, alongside
// the other work that is allowed to block (rule 10 in DESIGN.md). Nothing here may
// be called from inside vkQueuePresentKHR.

#ifndef VOCEM_FONTS_H
#define VOCEM_FONTS_H

#include <cstdint>

#include "imgui.h"

namespace vocem {

struct Fonts {
    ImFont* body = nullptr;    // names, message text
    ImFont* strong = nullptr;  // the channel name and a notification's sender
    // The size the atlas was actually built at. Zero until the first build.
    float pixel_size = 0.0f;
    // The text size the layout is expressed in multiples of -- the font_size
    // setting. What ui_scale() divides by, so that making the text larger makes
    // the text larger and leaves the padding, the pictures and the gaps alone.
    float reference = 16.0f;
};

const Fonts& fonts();

// The body text size for an output of this height, before the user's own scale.
// Proportional to the output, so the panel keeps roughly one physical size across
// displays, and clamped so that neither a tiny window nor an 8K one produces an
// absurd atlas. `reference` is the font_size setting, in the overlay's own units.
float font_pixel_size(uint32_t height, float user_scale, float reference);

// Which height font_pixel_size should be fed: the display's published mode
// height, because a window is where the overlay is drawn, not how large it
// should be (entry 39) -- except when the drawable is TALLER than every
// display. A drawable taller than the display can only be headed for the
// compositor's downscale (supersampling/DSR: a 4320-pixel drawable on a 2160
// display lands on screen at 0.5x), and sizing from the display there is what
// makes the overlay illegibly small -- the direction DESIGN refuses. Sizing
// from the drawable cancels the downscale exactly. A window is never taller
// than the display, so windowed games keep the display's sizing and entry 39's
// measurement stays byte-identical; zero display height is the old fallback
// (no daemon that new, no readable mode). One spelling for both injected
// paths, because two copies of this decision would drift (entry 33).
inline uint32_t sizing_height(uint32_t display_height, uint32_t drawable_height) {
    if (display_height == 0) {
        return drawable_height;
    }
    return display_height > drawable_height ? display_height : drawable_height;
}

// Everything laid out in the panel is expressed as a multiple of this, so the
// whole overlay grows with the display and with the size setting. 1.0 is a 1080p
// output at a size of 1.
//
// The text is the one thing it does not carry: the atlas is built at the font_size
// setting times this, and this is the atlas size divided by that same setting, so
// the two move independently. Making the text larger used to be the only way to
// make the panel larger, and vice versa.
float ui_scale();

// Rebuilds the atlas when the requested size differs from the current one, when
// the chosen typeface has changed, or when fonts_note_emoji() has seen a colour
// emoji the atlas does not carry yet. Returns true when it did, in which case
// the caller must recreate its backend's font texture -- the old one no longer
// matches the atlas.
//
// `body_path` and `strong_path` are the files the user chose (Config's
// font_path / font_path_strong), or null for the carried Inter. They are
// compared as well as the size, because a font changed in the settings has to
// reach the game the way a resolution change does -- and a dead band that
// remembers only the size would hold the old typeface until the display moved
// (entry 37's shape, one field further along).
//
// The chosen file is read here, which is why this belongs where it already was:
// the post-present phase, where allocating, rasterising and reading are allowed.
// Never inside a present.
//
// A path is a line in a text file, so the bytes behind it are treated as input:
// the header is checked before the rasteriser sees them (stb_truetype does not
// check its own, and a file that is not a font used to crash the game -- entry
// 92), and if the rasteriser then refuses them the atlas is built again from the
// carried Inter. Either way this returns true and the reason is readable below.
bool ensure_fonts(float pixel_size, float reference, const char* body_path = nullptr,
                  const char* strong_path = nullptr);

// Why the overlay is not drawing in the font that was asked for, or nullptr
// while there is nothing to say. Same contract as fonts_emoji_status(): a
// literal, so "once" is a pointer comparison, and both injected paths log it.
// A font that quietly does not load is a setting that quietly does nothing.
const char* fonts_font_status();

// Tells the atlas which colour emoji the frame's text needs. Walks the string,
// remembers the codepoints the bank carries (vocem/emoji_bank.h), and the next
// ensure_fonts() folds any new ones into the atlas as coloured glyphs.
// Allocation-free, and free of syscalls for a codepoint it has seen before -- so
// the steady state a game spends its life in costs a UTF-8 decode and nothing
// else. A codepoint it has NOT seen is asked of the bank, which is an open and a
// binary search of preads: bounded by the session cap on distinct emoji, but not
// free, and not what this comment claimed before it was measured. Without a bank
// on disk it remembers nothing, and the monochrome emoji keep drawing exactly as
// they always have.
void fonts_note_emoji(const char* utf8_text);

// Why the overlay is drawing no colour emoji, or nullptr while there is nothing
// to say -- either they are working or no text has needed one yet. Both injected
// paths log it once, because a feature that quietly does not happen is
// indistinguishable from a feature nobody asked for (entry 38's lesson, and the
// reason every declining component here says why). The string is a literal, so
// "once" is a pointer comparison; the second value it can take is the seen-cap,
// past which a new emoji stays monochrome for the rest of the session.
const char* fonts_emoji_status();

// The same, over everything a snapshot can put on screen: the channel name,
// every participant's name, the notification's title and body. One spelling,
// because both injected paths need exactly this walk and two copies of it
// would drift (entry 33).
struct Snapshot;
void fonts_note_emoji_in(const Snapshot& snapshot);

}  // namespace vocem

#endif  // VOCEM_FONTS_H

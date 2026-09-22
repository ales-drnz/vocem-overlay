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
// Rebuilding an atlas is not free and it replaces a texture the renderer may
// still be using. On the Vulkan path it happens after the present has returned
// (rule 10 in DESIGN.md), never inside vkQueuePresentKHR; on the OpenGL path
// inside the swap call, because that is all there is; and the FIRST build of a
// process, on both, on a worker thread beside the game (entry 192). The module
// has no lock of its own: its callers never overlap it -- a draw path does not
// touch the fonts while its worker is building.

#ifndef VOCEM_FONTS_H
#define VOCEM_FONTS_H

#include <cstddef>
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

// The atlas this module fills, and the one every context that wants these fonts
// must be created with: `ImGui::CreateContext(vocem::fonts_atlas())`.
//
// A context owns the atlas it creates and destroys it with itself, and this
// module caches ImFont pointers into that atlas -- so the question "is the atlas
// I remember still alive" had to be answered from outside, and both answers
// available from outside are wrong. Emptiness (entry 37's) stopped being true in
// 0.1.8, when the GL path began building its renderer's font texture inside
// ensure_backend(), before asking for the fonts: building that texture builds
// the atlas, which puts ImGui's default font in it, and a fresh atlas is
// therefore never empty by the time this is asked. Addresses are no better --
// measured on this machine, the second context's atlas landed at the SAME
// address as the dead one, with its default font at the address of the dead
// heavier weight.
//
// So the module owns the atlas instead, and the question does not arise: it
// outlives every context created with it. That is also what makes a context
// cycle cheap -- rasterising the atlas is 122 ms plus 11 ms to widen it to RGBA
// (4096x4096, measured twice), and a game that destroys and recreates a context
// used to pay it every time. A context created WITHOUT it gets an atlas of its
// own, which this module will fill but never promise anything about: the dead
// band below holds for its own atlas alone.
ImFontAtlas* fonts_atlas();

// Hand the pixels back: the atlas above is cleared and the cached pointers
// forgotten, so the next ensure_fonts() builds from nothing.
//
// For the cases where a guest has been asked to leave and should not still be
// holding 64 MB of rasterised glyphs (the RGBA32 copy at 4096x4096; the 16 MB
// alpha8 image it was widened from is freed as soon as it is widened, entry
// 207): the user switched the overlay off, the daemon stopped, and -- on the
// Vulkan path -- the last instance went, after which the loader unloads the
// library holding the only pointer to it (entry 211). NOT for a context death:
// an atlas outliving the context is the whole point, and a game that destroys
// and recreates one must pay nothing for it -- nor for a daemon that was merely
// replaced, which StatePoll tells apart.
//
// **The caller must have no live ImGui context pointing at this atlas.**
// Clear() IM_DELETEs every ImFont in it, and ImGui's own NewFrame would then
// dereference a freed one through GetDefaultFont() -- with IM_ASSERT compiled
// out of both injected targets, silently. Every call site satisfies this by
// destroying the context first (the GL path in release(), the Vulkan path in
// OverlayRenderer::shutdown(), called just before); the next one has to do the
// same.
void fonts_release();

// Rebuilds the atlas when the requested size differs from the current one or
// the chosen typeface has changed, and FOLDS into it a colour emoji
// fonts_note_emoji() has seen and the atlas does not carry yet -- into space
// every build reserves, without rasterising anything else (entry 191). Returns
// true when either happened: after a rebuild (fonts_build_count() moved) the
// caller replaces its backend's font texture whole, after a fold it copies the
// squares fonts_take_folded() hands over.
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

// How many times this process has rasterised the atlas.
//
// A count and not a clock, for entry 145's reason: a build's wall time on this
// machine is partly the machine's, and the thing worth holding is that a build
// does not happen. It is what tells the two costs apart from outside -- an atlas
// RASTERISED (14,954 glyphs in two weights, 125 to 145 ms measured) against an
// emoji FOLDED into space the build already reserved (0.3 to 0.9 ms). A session
// that draws for an hour and meets forty new emoji should say 1.
uint32_t fonts_build_count();

// A rectangle of the atlas's RGBA32 pixels, in pixels.
struct AtlasRegion {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
};

// The rectangles fold_wanted_emoji() has written since the last call, handed
// over and forgotten: exactly the pixels a new colour emoji changed, so a
// caller whose font texture already holds the rest of the atlas can upload
// these and nothing else. A 32x32 square is 4 KB; the whole atlas is 64 MB and
// on the Vulkan path its stock upload is two vkQueueWaitIdle on the game's own
// queue, measured at 36 to 43 ms of every arrival once the rebuild was gone
// (entry 192).
//
// Only meaningful when ensure_fonts() answered true WITHOUT fonts_build_count()
// moving. After a real build the texture has to be replaced whole, and this
// list is empty: a build forgets what earlier folds wrote, because the new
// atlas carries it anyway. Returns how many were written into `out`; at most
// two per emoji (one per weight), so a capacity of kMaxFoldedRegions never
// truncates; each is at most kMaxFoldedSide pixels on a side.
constexpr uint32_t kMaxFoldedRegions = 2 * 96;
constexpr int kMaxFoldedSide = 32;
uint32_t fonts_take_folded(AtlasRegion* out, uint32_t capacity);

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

// The noting, and before it the rewriting: an emoji SEQUENCE the bank's table
// knows -- 🍋‍🟩, a flag, a keycap, a family -- is collapsed in place into the
// bank key that draws it as one coloured glyph (vocem/emoji_bank.h says what a
// key is and why the table travels with the bank), and the result is noted.
// Without a table, or for a key the bank refuses, the text is left exactly as
// it was and the sequence draws as its coloured parts, which is what it did
// until 0.1.9. `capacity` is the field's size; the result is never longer
// than the text. No syscall beyond the noting's own, which is once per
// codepoint or key for the life of the process (tests/fonts_frame_quiet.cpp).
void fonts_prepare_text(char* text, size_t capacity);

// The same, over everything a snapshot can put on screen: the channel name,
// every participant's name, the notification's title and body. One spelling,
// because both injected paths need exactly this walk and two copies of it
// would drift (entry 33). The snapshot is the caller's own copy, and it is
// written to: a collapsed sequence changes the bytes the panel then measures
// and draws.
struct Snapshot;
void fonts_note_emoji_in(Snapshot& snapshot);

}  // namespace vocem

#endif  // VOCEM_FONTS_H

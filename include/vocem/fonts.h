// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The overlay's typeface: the atlas is rasterised from real outline fonts at
// the pixel size the output needs, rather than scaling ImGui's bitmap font.
//
// Rebuilding an atlas is not free and replaces a texture the renderer may
// still be using. On the Vulkan path it happens after the present has returned
// (rule 10), never inside vkQueuePresentKHR; on the OpenGL path inside the
// swap call; and the FIRST build of a process, on both, on a worker thread
// beside the game. The module has no lock of its own: its callers never
// overlap it -- a draw path does not touch the fonts while its worker builds.

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

// Which height font_pixel_size should be fed: the display's mode height,
// because a window is where the overlay is drawn, not how large it should be
// (entry 39) -- except when the drawable is TALLER than the display. That
// drawable is headed for the compositor's downscale (supersampling/DSR), and
// sizing from it cancels the downscale exactly. Zero display height (no
// readable mode) falls back to the drawable. One spelling for both injected
// paths.
inline uint32_t sizing_height(uint32_t display_height, uint32_t drawable_height) {
    if (display_height == 0) {
        return drawable_height;
    }
    return display_height > drawable_height ? display_height : drawable_height;
}

// Everything laid out in the panel is expressed as a multiple of this, so the
// whole overlay grows with the display and with the size setting. 1.0 is a 1080p
// output at a size of 1. The text is the one thing it does not carry: the atlas
// is built at the font_size setting times this, and this is the atlas size
// divided by that same setting, so the two move independently.
float ui_scale();

// The atlas this module fills, and the one every context that wants these fonts
// must be created with: `ImGui::CreateContext(vocem::fonts_atlas())`.
//
// The module owns the atlas so that it outlives every context created with it:
// the cached ImFont pointers stay valid across a context cycle, which also costs
// no rasterisation. Whether a context-owned atlas died cannot be told from
// outside -- a fresh one is never empty, and can reuse the dead one's address
// (entry 37). A context created WITHOUT it gets an atlas of its own, which this
// module will fill but never promise anything about.
ImFontAtlas* fonts_atlas();

// Whether fonts_atlas() has ever made the atlas in this process: true from the
// moment the object exists, before any context or build uses it. Asking does
// not make it. Atomic, because the Vulkan layer's last-instance teardown asks
// from whichever thread destroys the instance while the first build may be
// running on the atlas worker (fonts_build_count() moves too late for that).
bool fonts_atlas_made();

// Hand the pixels back: the atlas above is cleared and the cached pointers
// forgotten, so the next ensure_fonts() builds from nothing.
//
// For when the overlay should stop holding its rasterised glyphs: the user
// switched it off, the daemon stopped, or -- on the Vulkan path -- the last
// instance went, after which the loader unloads the library holding the only
// pointer to it. NOT for a context death, which must cost nothing, nor for a
// daemon that was merely replaced (StatePoll tells those apart).
//
// **The caller must have no live ImGui context pointing at this atlas.**
// Clear() IM_DELETEs every ImFont in it, and ImGui's NewFrame would then
// dereference a freed one through GetDefaultFont() -- silently, with IM_ASSERT
// compiled out. Every call site destroys the context first.
void fonts_release();

// Rebuilds the atlas when the requested size or the chosen typeface differs from
// the current one, and otherwise FOLDS in any colour emoji fonts_note_emoji()
// has seen and the atlas does not carry yet, into space every build reserves.
// Returns true when either happened: after a rebuild (fonts_build_count()
// moved) the caller replaces its backend's font texture whole, after a fold it
// copies the squares fonts_take_folded() hands over.
//
// `body_path` and `strong_path` are the files the user chose (Config's
// font_path / font_path_strong), or null for the carried Inter. They are
// compared as well as the size, so a font changed in the settings reaches the
// game the way a resolution change does.
//
// The chosen file is read here: post-present only, never inside a present. Its
// bytes are input: the header is checked before the rasteriser sees them
// (stb_truetype does not check its own), and if the rasteriser still refuses
// them the atlas is built again from the carried Inter. Either way this returns
// true and fonts_font_status() says why.
bool ensure_fonts(float pixel_size, float reference, const char* body_path = nullptr,
                  const char* strong_path = nullptr);

// How many times this process has rasterised the atlas. A count and not a
// clock: it tells a full rasterisation apart from an emoji folded into reserved
// space. A session that meets forty new emoji should say 1.
uint32_t fonts_build_count();

// A rectangle of the atlas, in pixels, and the RGBA32 pixels that belong in
// it: width * height * 4 bytes, rows packed, owned by this module and valid
// until the next build or fonts_release(). A fold's square is read from here,
// not from the whole atlas's RGBA copy, which exists only around a whole upload.
struct AtlasRegion {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;
    const unsigned char* pixels = nullptr;
};

// The whole atlas as RGBA32, for a whole upload of a font texture: what
// `GetTexDataAsRGBA32` answers, widened from the alpha8 image if the RGBA
// copy is not there, with every colour emoji folded so far pasted back in.
// ImGui's own widening knows nothing of the colour squares, so every whole
// upload -- the backend's own included (the GL backend's CreateDeviceObjects
// and CreateFontsTexture read the atlas themselves) -- goes after a call to
// this. False when the atlas has no pixels.
//
// The copy is 4 bytes a pixel, 43 MB at a 2160-line display, and after the
// upload nothing needs it: fonts_atlas_uploaded() hands it back and keeps the
// alpha8 image (a quarter of it) and the squares. A build leaves the copy in
// place, widened where the build ran (on the worker for the first one), for
// the upload that follows it.
bool fonts_atlas_rgba(unsigned char** pixels, int* width, int* height);

// The same pixels as fonts_atlas_rgba(), written into `destination`
// (width * height * 4 bytes, the atlas's own size) without making the RGBA
// copy: widened from the alpha8 image straight there when the copy is not
// there, copied from it when it is. `destination` is only written, never
// read, so it can be a mapped staging buffer (the Vulkan layer's whole
// upload). False when the atlas has no pixels or is not that size.
bool fonts_atlas_widen_into(unsigned char* destination, int width, int height);

// The whole atlas went up: its RGBA copy is freed, the alpha8 image and the
// folded squares stay. Nothing is freed unless the alpha8 image is there to
// widen from again -- asking ImGui for pixels with neither would rebuild the
// atlas.
void fonts_atlas_uploaded();

// The rectangles fold_wanted_emoji() has written since the last call, handed
// over and forgotten: exactly the pixels a new colour emoji changed, so a
// caller whose font texture already holds the rest of the atlas uploads these
// and nothing else rather than the whole atlas.
//
// Only meaningful when ensure_fonts() answered true WITHOUT fonts_build_count()
// moving; after a real build this list is empty and the texture is replaced
// whole. Returns how many were written into `out`; at most two per emoji (one
// per weight), so a capacity of kMaxFoldedRegions never truncates; each is at
// most kMaxFoldedSide pixels on a side.
constexpr uint32_t kMaxFoldedRegions = 2 * 96;
constexpr int kMaxFoldedSide = 32;
uint32_t fonts_take_folded(AtlasRegion* out, uint32_t capacity);

// Why the overlay is not drawing in the font that was asked for, or nullptr
// while there is nothing to say. Same contract as fonts_emoji_status().
const char* fonts_font_status();

// Tells the atlas which colour emoji the frame's text needs. Walks the string,
// remembers the codepoints the bank carries (vocem/emoji_bank.h), and the next
// ensure_fonts() folds any new ones into the atlas as coloured glyphs.
// Allocation-free, and free of syscalls once the bank has been asked for (the
// first ensure_fonts() does that): a codepoint not seen before is queued for
// fonts_look_up_noted(), after the present. The one exception is the OpenGL
// path's first frame, which notes before its first build and opens the bank
// here, inside the swap call where the build is too. Without a bank on disk it
// remembers nothing and the monochrome emoji draw as they always do.
void fonts_note_emoji(const char* utf8_text);

// The lookups the noting queued, and the bank's own open and sequence table
// read the first time this runs. Until this has run and ensure_fonts() folded
// it, a new codepoint draws from the monochrome font. ensure_fonts() calls this
// first; it is public for a caller that notes without folding.
void fonts_look_up_noted();

// Why the overlay is drawing no colour emoji, or nullptr while there is nothing
// to say -- either they are working or no text has needed one yet. Both injected
// paths log it once. The string is a literal, so "once" is a pointer
// comparison; one value it can take is the seen-cap, past which a new emoji
// stays monochrome for the rest of the session.
const char* fonts_emoji_status();

// The noting, and with it the rewriting: an emoji SEQUENCE the bank's table
// knows -- 🍋‍🟩, a flag, a keycap, a family -- is noted as its key, and
// collapsed in place into that key (vocem/emoji_bank.h says what a key is) once
// the atlas carries the key's glyph, and not before: until the fold the
// sequence draws as its parts (tests/fonts_key_drawable.cpp). Without a table,
// or for a key the bank refuses, the text is left as it was. `capacity` is the
// field's size; the result is never longer than the text. No syscall once the
// bank has been asked for (tests/fonts_frame_quiet.cpp).
void fonts_prepare_text(char* text, size_t capacity);

// Where a sequence key lives in the TEXT and the ATLAS: the Basic Multilingual
// Plane's private use area, numbered from its start in the order of the keys
// on disk (a key U+F0000 + n is drawn as U+E000 + n). The files keep their own
// numbering (entry 142) and only this module translates: ImGui's glyph index is
// sized by the highest codepoint a font holds, and a key at U+F0000 would grow
// it to ~984,000 entries a weight (tests/fonts_key_index.cpp). A key past the
// room draws as its parts, said by fonts_emoji_status(). A codepoint of this
// area arriving in a name becomes U+FFFD unless it is a key already folded
// (fonts.cpp says why).
constexpr uint32_t kSequenceKeyFirst = 0xE000;
constexpr uint32_t kSequenceKeyLast = 0xF8FF;

// The same, over everything a snapshot can put on screen: the channel name,
// every participant's name, the notification's title and body. One spelling
// for both injected paths. The snapshot is the caller's own copy, and it is
// written to: a collapsed sequence changes the bytes the panel then measures
// and draws.
struct Snapshot;
void fonts_note_emoji_in(Snapshot& snapshot);

}  // namespace vocem

#endif  // VOCEM_FONTS_H

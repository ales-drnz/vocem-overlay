// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What the overlay is made of, separately from how it is drawn: every colour
// and proportion once, read by common/src/panel.cpp, by the settings window's
// bridge, and by tests/panel_geometry.cpp, which finds shapes by colour.
//
// Plain arithmetic on the stack, no allocation: the injected code reads this on
// the present path. And vocem-config is not built against ImGui, so a colour is
// four bytes, not an ImU32; each side converts at the point of use.
//
// Distances are in reference units -- a pixel at 1080 lines of display height
// with the size settings at 1 -- and carry no scale: the drawing multiplies by
// ui_scale() and rounds through pixels() (rule 23), the previews let the view
// scale (rule 24).

#ifndef VOCEM_THEME_H
#define VOCEM_THEME_H

#include <cstdint>

#include "vocem/config.h"

namespace vocem {

// Four bytes, deliberately not an ImGui type (see above).
struct Colour {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
    uint8_t a = 255;
};

// A literal, written the way it is read: `hex(0x5865f2)`.
inline constexpr Colour hex(uint32_t value, uint8_t alpha = 255) {
    return Colour{static_cast<uint8_t>((value >> 16) & 0xff),
                  static_cast<uint8_t>((value >> 8) & 0xff),
                  static_cast<uint8_t>(value & 0xff), alpha};
}

// Whether text on this background should be light or dark, by Rec. 601 luma.
// The geometry harness forces a near-black panel, so the pale-box palette is
// checked by asking this file rather than by drawing.
inline constexpr bool is_light(uint32_t colour) {
    return (0.299f * static_cast<float>((colour >> 16) & 0xff) +
            0.587f * static_cast<float>((colour >> 8) & 0xff) +
            0.114f * static_cast<float>(colour & 0xff)) > 150.0f;
}

// How strong the outline around the text is: a constant, not a curve tied to
// the box's opacity. Background and outline both, because neither alone
// survives every scene; on the solid dark box the dark outline is invisible
// (entry 41).
inline constexpr float kTextOutlineStrength = 0.65f;

// The RGB of a token, for comparing two: a collision is visual, alpha aside.
inline constexpr uint32_t token_rgb(Colour colour) {
    return (static_cast<uint32_t>(colour.r) << 16) | (static_cast<uint32_t>(colour.g) << 8) |
           colour.b;
}

// A pinned colour that lands exactly on another measured role's value, stepped
// off it along the blue channel (tests/panel_geometry.cpp, distinct_tokens).
uint32_t step_off_taken(uint32_t rgb, const uint32_t* taken, int count);

// The mark on a placeholder disc, blended from the disc's own colour towards
// white on a dark disc, towards black on a pale one.
Colour avatar_mark_for(Colour disc);

struct Theme {
    // --- the voice panel ---------------------------------------------------
    // No alpha: transparency is the `opacity` setting, applied where the window
    // background is pushed, so colour and transparency change independently.
    Colour panel_surface;
    // The line under the channel name: a short blurple accent bar, then the
    // hairline to the edge. The blurple decorates and never carries a meaning
    // alone (entry 40). 22 units, the default line-with-spacing, so it reads as
    // an underline of the channel's first letters.
    Colour separator;
    Colour separator_accent;
    float separator_accent_length = 22.0f;

    // The hairline just inside the panel's edge, which lets a dark box end on a
    // dark scene. Its alpha is premultiplied by the panel's opacity in
    // theme_for(), so a transparent panel leaves no floating outline.
    // 0xfefefe, not white, and 0x010101 on a light surface, not black: the
    // geometry test finds shapes by colour within one draw list, and the channel
    // name (white) and the avatar scrim (black) share it.
    Colour panel_hairline;

    // The speaker is marked twice: the ring, and the name at full strength while
    // the others are greyed -- weight is easier to find than a thin stroke.
    Colour text_channel;
    Colour text_speaking;
    Colour text_idle;
    // Dimmer still for somebody muted or deafened, as Discord does.
    Colour text_muted;
    // "+N more": its own token, since on a pale box it is not an idle name's grey.
    Colour text_overflow;

    // --- pictures and their decorations ------------------------------------
    // Not downloaded yet: a neutral disc keeps the layout stable.
    Colour avatar_placeholder;
    // The head-and-shoulders mark on that disc: a token so the panel, the QML
    // preview and the geometry test read one value.
    Colour avatar_mark;
    // Laid over a muted or deafened picture, so the badge reads as a state.
    Colour avatar_scrim;
    Colour speaking_ring;
    Colour badge_fill;
    // The surface's own colour at full opacity, drawn as a ring outside the
    // disc: it carves the badge out of whatever it hangs over, so the glyph
    // never sits on pixels nobody chose.
    Colour badge_rim;
    Colour badge_glyph;

    // --- the message box ---------------------------------------------------
    Colour toast_surface;
    Colour toast_title;
    Colour toast_body;
    // The panel's edge treatment, following the message box's own opacity.
    Colour toast_hairline;
    // A blurple bar down the left edge marking where a message begins, never the
    // only carrier of anything. Three units wide, inset top and bottom by the box
    // radius so its ends stop where the corner curves, outer corners rounded by
    // its own width (the approved mockup's numbers).
    Colour toast_accent;
    float toast_accent_width = 3.0f;

    // --- proportions, in reference units -----------------------------------
    // One radius for both boxes.
    float box_radius = 8.0f;
    // How far outside the picture the ring sits.
    float ring_offset = 2.0f;
    // The ring's stroke as a proportion of the picture's radius, so it is neither
    // a blot on a small avatar nor a wire on a large one: 2 units at the default
    // avatar size, radius 13.86. The drawing floors it at one device pixel.
    float ring_width_factor = 0.144f;
    // What the row reserves for the ring: more than it reaches
    // (ring_offset + width/2 is 3.0 at the default avatar size, 3.33 at 2.0).
    // The slack is kept because reserving less would change every row's height.
    float ring_allowance = 3.75f;

    // The picture's radius as a multiple of the bare line of text
    // (GetTextLineHeight(), never the with-spacing figure, or the avatars would
    // resize with the spacing slider). 0.5775 x 16 = 9.24 units, the calibrated
    // radius the previews and the comparison are measured against.
    float avatar_radius_factor = 0.5775f;

    // The badge hangs off the bottom-right of the picture, on a grid of
    // sixteenths: offset and radius are fractions of the avatar's radius, the
    // two strokes fractions of the badge's own, so its shape holds at every size.
    float badge_offset_factor = 0.75f;
    float badge_radius_factor = 0.5f;
    // The glyph's stroke: 3/16 of the badge radius.
    float badge_stroke_factor = 0.1875f;
    // The rim, drawn outside the disc (see badge_rim).
    float badge_rim_stroke_factor = 0.25f;
    // What the row reserves for the badge beyond the picture's edge, exactly:
    // 0.75 out + 0.5 radius + 0.125 of rim = 1.375 from the centre.
    float badge_allowance_factor = 0.375f;

    // The pill behind one name (Config::kBoxNames): how far it reaches past the
    // glyphs. Its radius is half its own height, so it is a pill at every text
    // size and needs no token. The vertical pad is the smallest that leaves a
    // visible band around a line of text; it also enters the layout -- a row is
    // at least a pill tall (panel.cpp says why).
    float name_box_padding_x = 6.0f;
    float name_box_padding_y = 2.0f;

    // The outline's ink: the opposite pole of the ramp (dark around pale text,
    // pale around dark), so the glyph keeps an edge wherever the box does not
    // cover. Its alpha comes from the strength below.
    Colour text_outline_ink;

    // --- text outline, 0 to 1 ----------------------------------------------
    // kTextOutlineStrength while the switch is on, zero when it is off; one
    // field per box because their switches are separate.
    float panel_text_outline = 0.0f;
    float toast_text_outline = 0.0f;
};

// The theme the user's settings come to. Pure: same settings, same answer, no
// allocation, no state.
Theme theme_for(const Config& config);

// The surfaces the settings window offers ready-made, and the opacity each is
// promised at. The window reads them through the bridge;
// tests/theme_contrast.cpp holds every entry but purple and transparent to the
// 4.5:1 floor.
//
//   * pills         -- the default: #17181c at 88% behind each name only, so
//                      the floor holds where the text is and the rest stays
//                      the game's.
//   * transparent   -- no box. The surface stays the dark one so the pale ramp
//                      lands on the game and the opacity slider starts from a
//                      measured surface.
//   * dark          -- #17181c at 88% behind the whole panel (config.h).
//   * light         -- #eff0f1, solid: at 88% over a dark scene the muted grey
//                      cannot clear 4.5:1 without crossing the idle grey.
//   * purple        -- the client's blurple, solid, promised no floor.
//
// Presets are named for what they look like, never after another product. The
// id is stable and untranslated; the window shows its own words for it.
struct Preset {
    const char* id;
    uint32_t colour;
    float opacity;
    // Where the surface is drawn (Config::kBoxPanel / kBoxNames): `pills` and
    // `dark` share colour and opacity and differ only here.
    int box;
};
// The first entry is the defaults, asserted equal to config.h's in
// tests/theme_contrast.cpp.
inline constexpr Preset kPresets[] = {
    {"pills", 0x17181c, 0.88f, Config::kBoxNames},
    {"transparent", 0x17181c, 0.0f, Config::kBoxPanel},
    {"dark", 0x17181c, 0.88f, Config::kBoxPanel},
    {"light", 0xeff0f1, 1.0f, Config::kBoxPanel},
    {"purple", 0x5865f2, 1.0f, Config::kBoxPanel},
};

// What the row reserves for the picture's decorations, whether or not anyone is
// speaking or muted, so no state change resizes the panel; without it the ring
// is clipped by the window's edge at small paddings (tests/panel_geometry.cpp).
// The ring's reach is in reference units and takes the scale; the badge's is a
// fraction of the radius, which already carries it.
inline float decoration_allowance(const Theme& theme, float radius, float scale) {
    const float ring = theme.ring_allowance * scale;
    const float badge = radius * theme.badge_allowance_factor;
    return ring > badge ? ring : badge;
}

}  // namespace vocem

#endif  // VOCEM_THEME_H

// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What the overlay is made of, separately from how it is drawn.
//
// Every colour and every proportion in the panel used to be a literal inside the
// drawing code, which meant three copies of each: one in `common/src/panel.cpp`,
// one hand-mirrored in the QML previews, and a third in `tests/panel_geometry.cpp`,
// which finds a shape by looking for its colour. Three copies of a number is three
// chances for two of them to disagree, and the disagreement is silent -- a test
// that looks for a colour nothing draws any more measures an empty rectangle and
// reports it as a rectangle.
//
// So the numbers live here, once, and the three consumers read them. The drawing
// code asks for `theme.text_speaking`; the configuration window asks the bridge,
// which asks this file; the geometry test asks this file directly, so it cannot
// look for a colour the panel has stopped using.
//
// Two constraints shape the interface, and both are why this is not simply a
// header full of ImGui colours:
//
//   * The injected code must not gain dependencies and must not allocate on the
//     present path (DESIGN.md, "Rules for the layer"). Everything here is
//     header-only, `inline`, and made of plain arithmetic on the stack.
//   * `vocem-config` compiles this file too, and it is not built against ImGui --
//     `gui/CMakeLists.txt` gives it `include/` and its own sources and nothing
//     else. So a colour is four bytes, not an `ImU32`, and each side converts at
//     the point of use.
//
// Distances are in the overlay's reference unit -- a pixel at 1080 lines of
// display height with the size settings at 1 -- and carry no scale. The drawing
// code multiplies by `ui_scale()` and rounds through `pixels()` where the result
// adds up into the size of a box (DESIGN.md rule 23); the previews are laid out in
// these units directly and let the view do the scaling (rule 24). A theme that
// carried scaled pixels would be readable by neither.

#ifndef VOCEM_THEME_H
#define VOCEM_THEME_H

#include <cstdint>

#include "vocem/config.h"

namespace vocem {

// Four bytes, in the order everything else here writes them. Deliberately not an
// ImGui type: see the note above.
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

// Whether text on this background should be light or dark. A user who picks a pale
// colour for the box should not be left with white names on it.
//
// Rec. 601 luma, which is close enough for a legibility decision. Note that no test
// exercises the light branch through the geometry harness: `measure()` forces the
// panel colour to a near-black sentinel so that shapes can be told apart by colour,
// so the pale-box palette is checked by asking this file rather than by drawing.
inline constexpr bool is_light(uint32_t colour) {
    return (0.299f * static_cast<float>((colour >> 16) & 0xff) +
            0.587f * static_cast<float>((colour >> 8) & 0xff) +
            0.114f * static_cast<float>(colour & 0xff)) > 150.0f;
}

// How strong the outline drawn around the text is. A constant, not a curve: the
// outline used to be a shadow that faded in as the background was turned down --
// a remedy, applied when the box had stopped doing its job. MangoHud's default is
// the instruction here: background *and* outline, both always, because neither
// alone survives every scene. Against the solid dark box the dark outline
// disappears into it and costs nothing visually; the moment the box thins, the
// outline is already there. Measured before being made permanent: four extra
// text draws per string cost 2.07 us a frame at eight participants at 4K --
// 0.012% of a 60 Hz frame -- so the treatment that guards every edge of a glyph
// was affordable and the fading single shadow was not worth its subtlety.
inline constexpr float kTextOutlineStrength = 0.65f;

// The RGB of a token, for comparing two of them: the alpha is not part of what
// a user picks, and the collision the comparison guards against is visual.
inline constexpr uint32_t token_rgb(Colour colour) {
    return (static_cast<uint32_t>(colour.r) << 16) | (static_cast<uint32_t>(colour.g) << 8) |
           colour.b;
}

// A pinned colour that lands exactly on another measured role's value, stepped
// off it by the smallest visible-to-nobody amount. tests/panel_geometry.cpp
// finds every shape by its colour within a draw list, so two roles on one value
// make a figure plausible and wrong (distinct_tokens is the assertion); the
// hairline's 0xfefefe and the badge glyph's 0xfffffe already pay one part in
// 255 for the same property, and a user's pick pays the same coin. The blue
// channel walks outward one step at a time until nothing collides -- pure
// arithmetic on the stack, at most a few hundred comparisons against a dozen
// tokens, so it stays within what a header the injected code compiles may do.
inline uint32_t step_off_taken(uint32_t rgb, const uint32_t* taken, int count) {
    const auto collides = [&](uint32_t value) {
        for (int i = 0; i < count; ++i) {
            if (taken[i] == value) {
                return true;
            }
        }
        return false;
    };
    if (!collides(rgb)) {
        return rgb;
    }
    const int blue = static_cast<int>(rgb & 0xff);
    for (int step = 1; step < 256; ++step) {
        for (int sign : {-1, 1}) {
            const int candidate = blue + sign * step;
            if (candidate < 0 || candidate > 255) {
                continue;
            }
            const uint32_t value = (rgb & 0xffff00u) | static_cast<uint32_t>(candidate);
            if (!collides(value)) {
                return value;
            }
        }
    }
    return rgb;  // Unreachable: there are far fewer roles than blues.
}

struct Theme {
    // --- the voice panel ---------------------------------------------------
    // The surface carries no alpha: transparency is the `opacity` setting, applied
    // where the window background is pushed, so a colour and its transparency can
    // be changed without disturbing each other.
    Colour panel_surface;
    // The line under the channel name is two segments now: a short accent bar,
    // and the hairline that carries on to the edge. The accent is the blurple's
    // one job on the panel -- it may decorate, it may never be the only carrier
    // of a meaning (DESIGN.md), and a bar under a name it decorates. Its length
    // is in reference units, from the approved mockup: 22, which is also the
    // default line-with-spacing, so it reads as an underline of the channel's
    // first letters rather than as a rule that stops short.
    Colour separator;
    Colour separator_accent;
    float separator_accent_length = 22.0f;

    // The hairline just inside the panel's edge: what lets a dark box end on a
    // dark scene. It carries its alpha here, premultiplied by the panel's opacity
    // in theme_for(), so a panel turned down to nothing does not leave a floating
    // outline. There used to be a drop shadow outside the edge as well -- Fluent's
    // elevation equation, four stacked fills -- removed on the owner's judgement:
    // it read as weight, not as an edge, and the hairline alone says where the
    // box ends.
    //
    // The hairline is 0xfefefe rather than 0xffffff: the channel name is pure
    // white on the dark surface, and tests/panel_geometry.cpp finds a shape by its
    // colour within one draw list -- two roles on one value and the measurement
    // silently returns the wrong rectangle. One part in 255 is invisible and keeps
    // the two measurable. Same reason the light-surface hairline is 0x010101 and
    // not black: the avatar scrim is 0x000000 in the same list.
    Colour panel_hairline;

    // Who is talking is said twice -- the ring around the picture, and the name at
    // full strength while everybody else's is greyed. The ring alone is a thin
    // circle read against whatever the game happens to be drawing behind it, and in
    // a channel of eight it was work to find. Weight is easier to see than a stroke.
    Colour text_channel;
    Colour text_speaking;
    Colour text_idle;
    // Dimmer still for somebody who cannot speak or cannot hear, which is what
    // Discord does -- it dims them rather than colouring the name.
    Colour text_muted;
    // Whoever did not fit. Its own token because on a pale box it is not the same
    // grey as an idle name: it is a remark about the list, not a person in it.
    Colour text_overflow;

    // --- pictures and their decorations ------------------------------------
    // Not downloaded yet: a neutral disc keeps the layout stable so nothing jumps
    // when the image arrives.
    Colour avatar_placeholder;
    // Laid over the picture of somebody muted or deafened. Dimming alone is
    // ambiguous, which is what the badge is for; dimming is what makes the badge
    // read as a state rather than as a sticker.
    Colour avatar_scrim;
    Colour speaking_ring;
    Colour badge_fill;
    // The surface's own colour at full opacity, drawn as a ring outside the
    // disc: the badge is carved out of whatever it hangs over -- a pale avatar,
    // a busy one, the game through a translucent box -- by a band of the box's
    // colour, so the glyph never sits directly on pixels nobody chose. It used
    // to be translucent black on the disc's edge, which read as a badge against
    // a light avatar and vanished against a dark one.
    Colour badge_rim;
    Colour badge_glyph;

    // --- the message box ---------------------------------------------------
    Colour toast_surface;
    Colour toast_title;
    Colour toast_body;
    // The same edge treatment as the panel's, following the message box's own
    // opacity instead of the panel's.
    Colour toast_hairline;
    // The blurple's job on the toast: a bar down the left edge, marking where a
    // message begins -- decoration beside a sender the title already names,
    // never the only carrier of anything. Three units wide; inset from the top
    // and bottom by the box radius, so its ends stop where the corner curvature
    // starts, with its outer corners rounded by its own width. All three
    // numbers are the approved mockup's.
    Colour toast_accent;
    float toast_accent_width = 3.0f;

    // --- proportions, in reference units -----------------------------------
    // One radius for both boxes. The panel had 8 and the toast 10, and nobody
    // could say why: two boxes from one hand carry one corner.
    float box_radius = 8.0f;
    // How far outside the picture the ring sits.
    float ring_offset = 2.0f;
    // How thick it is drawn: a proportion of the radius, not the fixed 2.0 it
    // was. A stroke that stays two units while the picture halves or doubles
    // reads as a blot on a small avatar and as a wire on a large one. The
    // constant is chosen so the default look does not move: at the default
    // avatar size the radius is 13.86 reference units and the tuned stroke was
    // 2.0, and 2 / 13.86 = 0.144. The drawing keeps a one-device-pixel floor
    // under the result, as it does for the badge's strokes.
    float ring_width_factor = 0.144f;
    // What the row reserves for the ring, which is not the same thing as what the
    // ring occupies: the ring reaches `ring_offset + width/2`, which is 3.0 units
    // at the default avatar size and 3.33 at avatar_size 2.0 -- both under this
    // 3.75. The slack has been there since the ring was drawn 2.5 out with a 2.5
    // stroke, and it is kept deliberately: reserving less would change the height
    // of every row, which is a change to the size of the box rather than to the
    // look of the ring, and the two are worth making separately.
    float ring_allowance = 3.75f;

    // The picture, as a multiple of the bare line of text -- GetTextLineHeight(),
    // never the with-spacing figure: that one includes row_spacing, and a radius
    // derived from it made every avatar resize when the spacing slider moved.
    // The constant is the old calibrated look re-expressed against the new base
    // so that nothing moved at the defaults: it was 0.42 of a line-with-default-
    // spacing (22 reference units), and 0.42 x 22 / 16 = 0.5775 of the bare line
    // is the same 9.24-unit radius to the last digit. Not rounded to something
    // prettier, because prettier would have moved the geometry the previews and
    // the comparison are calibrated to.
    float avatar_radius_factor = 0.5775f;

    // The badge hangs off the bottom-right of the picture. On a grid now: every
    // proportion is a multiple of a sixteenth, where they used to be 0.72, 0.52,
    // 0.16 -- values tuned by eye one at a time, each a hundredth or two off a
    // grid step for no reason anybody could name. The offset and the radius are
    // fractions of the avatar's radius; the two strokes are fractions of the
    // badge's own, so the badge stays the same shape at every avatar size.
    float badge_offset_factor = 0.75f;
    float badge_radius_factor = 0.5f;
    // The glyph's stroke: 3/16 of the badge radius.
    float badge_stroke_factor = 0.1875f;
    // The rim: a quarter of the badge radius, drawn *outside* the disc, in the
    // surface's own colour at full opacity -- see badge_rim below. Thicker than
    // it was (it was 0.8 of the glyph stroke, on the disc's edge, translucent
    // black) because its job changed: it no longer frames the badge, it carves
    // the badge out of whatever it hangs over.
    float badge_rim_stroke_factor = 0.25f;
    // What the row reserves for the badge, as a fraction of the avatar's radius,
    // and exact now rather than approximate: 0.75 out, plus a 0.5 radius, plus
    // the 0.25 * 0.5 = 0.125 of rim beyond the disc -- 1.375 from the centre, so
    // 0.375 beyond the picture's edge.
    float badge_allowance_factor = 0.375f;

    // What the outline around the text is made of. The opposite pole of the ramp:
    // dark around pale text, pale around dark text, so on whatever the box fails
    // to cover the glyph still has an edge on its readable side. Its alpha is not
    // this token: the strength below decides that.
    Colour text_outline_ink;

    // --- text outline, 0 to 1 ----------------------------------------------
    // kTextOutlineStrength while the switch is on, zero when it is off. Constant
    // by design -- see the note on kTextOutlineStrength. The two boxes stay
    // separate fields because their switches could yet diverge, and a message is
    // read once while the panel sits there all session.
    float panel_text_outline = 0.0f;
    float toast_text_outline = 0.0f;
};

// The theme the user's settings come to. Pure: same settings, same answer, no
// allocation, no state.
//
// The palette is Discord's for the parts that are not configurable, because the
// thing this overlay stands in for is the Discord client and a participant list
// that looked like something else would be read as something else.
inline Theme theme_for(const Config& config) {
    Theme theme;

    const bool light_panel = is_light(config.panel_colour);
    const bool light_toast = is_light(config.notification_colour);

    theme.panel_surface = hex(config.panel_colour);
    theme.toast_surface = hex(config.notification_colour);
    theme.speaking_ring = hex(config.speaking_colour);

    // Two palettes, chosen by the box they sit on rather than by a setting: a user
    // who picks a pale colour has said what the box should be, not that they would
    // like to pick the text colours too.
    if (light_panel) {
        theme.text_channel = hex(0x3c3e44);
        theme.text_speaking = hex(0x18191c);
        theme.text_idle = hex(0x5a5c62);
        // Not 0x96989e, which this was: measured on the palest surface offered --
        // Breeze Light's window at 88% over snow -- that grey read 2.5:1, and a
        // muted name that misses the floor is the exact defect this palette
        // replaced on the dark side. 0x66696f is the lightest step that clears
        // 4.5:1 there (4.8:1), while staying visibly under the idle grey (5.9:1)
        // so the ordering survives greyscale.
        theme.text_muted = hex(0x66696f);
        theme.text_overflow = hex(0x6e7076);
    } else {
        // Not Discord's greys any more. Those were drawn by Discord on a
        // near-black surface and arrived here on the blurple, where the dimmest of
        // them crossed under the box -- a muted name at 1.02:1, invisible. This
        // ramp is chosen against the surface it actually sits on, composited over
        // the scene behind it (WCAG 2.2, measured in tests/theme_contrast.cpp):
        // on the default surface at its default opacity the worst case -- a muted
        // name over snow -- reads 4.9:1, over the 4.5:1 floor, and the best is the
        // channel name at 12.8:1. The steps stay far enough apart that speaking /
        // idle / muted survive greyscale, which is the ordering the same test
        // asserts.
        theme.text_channel = hex(0xffffff);
        theme.text_speaking = hex(0xf2f3f5);
        theme.text_idle = hex(0xb5bac1);
        theme.text_muted = hex(0x9aa0ab);
        // The idle grey to the eye, one part in 255 off it to the measurement:
        // the "+N more" line's alignment is asserted by tests/panel_geometry.cpp,
        // and a shape is found there by its colour within a draw list.
        theme.text_overflow = hex(0xb5bac0);
    }

    if (light_toast) {
        theme.toast_title = hex(0x18191c);
        theme.toast_body = hex(0x46484e);
    } else {
        theme.toast_title = hex(0xffffff);
        // White, to the last usable part: the body was the panel's idle grey
        // (0xb5bac1, 7.8:1 on the worst composite), and at a real display's scale
        // the grey rendered under half the pixel coverage of the title on the
        // dark box -- measured by the backend, decided by the owner. The
        // title/body hierarchy is carried by the font weight (the title is drawn
        // in the heavier Inter, panel.cpp), not by the colour step any more. Not
        // 0xffffff: the title owns pure white in the same draw list, and the
        // measurement tells shapes apart by colour -- the badge glyph's trade,
        // one part in 255 on blue, invisible.
        theme.toast_body = hex(0xfffffe);
    }

    // The toast's edge and accent, both premultiplied by the message box's own
    // opacity, exactly as the panel's hairline is by the panel's: the bar is part
    // of the box, and a box turned down to nothing must not leave a full-strength
    // blurple bar floating on the game. The fade-out multiplies on top, at the
    // draw.
    const auto toast_premultiplied = [&](uint32_t rgb, float alpha) {
        return hex(rgb, static_cast<uint8_t>(alpha * config.notification_opacity + 0.5f));
    };
    theme.toast_hairline = light_toast ? toast_premultiplied(0x010101, 20.0f)
                                       : toast_premultiplied(0xfefefe, 20.0f);
    theme.toast_accent = toast_premultiplied(0x5865f2, 255.0f);

    theme.text_outline_ink = light_panel ? hex(0xffffff) : hex(0x000000);
    theme.avatar_placeholder = hex(0x4f545c);
    theme.avatar_scrim = hex(0x000000, 110);
    theme.badge_fill = hex(0xf23f43);
    // The surface at full opacity, deliberately not at the panel's: the rim's job
    // is to be the one band around the glyph that is always the box's colour,
    // whatever the box lets through elsewhere.
    theme.badge_rim = hex(config.panel_colour);
    // 0xfffffe, not white: the channel name is pure white in the same draw list,
    // and the geometry test finds a shape by its colour. One part in 255 keeps
    // the two measurable separately -- same trade as the hairline's 0xfefefe.
    theme.badge_glyph = hex(0xfffffe);

    // The line under the channel name, chosen now rather than inherited. Phase 1
    // named ImGui's default (0x6e6e80 at half alpha) without changing a pixel,
    // exactly so that this change would happen where it could be seen: the
    // hairline is now the same treatment as the panel's edge -- pale at 10% on a
    // dark box, dark on a pale one -- and the accent bar in front of it is the
    // blurple, doing the one job it is allowed (decoration, never meaning).
    // 0xfdfdfd and 0x020202 rather than pure white and black: the channel name
    // owns 0xffffff and the edge hairlines own 0xfefefe / 0x010101 in the same
    // draw list, and the measurement tells shapes apart by colour.
    theme.separator = light_panel ? hex(0x020202, 38) : hex(0xfdfdfd, 26);
    theme.separator_accent = hex(0x5865f2);

    // The hairline follows the box's own opacity, premultiplied here rather than
    // at the draw: a panel faded to nothing must take its edge with it, and every
    // consumer of the token -- the drawing, the preview, the measurement -- has
    // to agree on that without re-deriving it. The 20 is 8% of 255: enough to
    // read as an edge highlight on the dark surface, not enough to read as a
    // border.
    const auto premultiplied = [&](uint32_t rgb, float alpha) {
        return hex(rgb, static_cast<uint8_t>(alpha * config.opacity + 0.5f));
    };
    theme.panel_hairline =
        light_panel ? premultiplied(0x010101, 20.0f) : premultiplied(0xfefefe, 20.0f);

    theme.panel_text_outline = config.text_shadow ? kTextOutlineStrength : 0.0f;
    theme.toast_text_outline = config.text_shadow ? kTextOutlineStrength : 0.0f;

    // The three colours a user may pin (config.h: kColourAuto, the default, keeps
    // the ramps above). A pinned value replaces its role exactly -- except when
    // it lands on a colour another measured role in the same draw list already
    // wears, where it takes the one-part-in-255 step step_off_taken() explains.
    // Applied last, so every token it must stay clear of already has its final
    // value; idle before speaking, so a pair pinned to one colour still comes out
    // as two. What is deliberately not here: no contrast promise. A pinned text
    // colour is the user's own choice, like a pinned surface, and
    // tests/theme_contrast.cpp prints what an arbitrary choice measures rather
    // than promising a floor no palette can hold on it.
    if (config.text_idle_colour != Config::kColourAuto) {
        const uint32_t taken[] = {
            token_rgb(theme.avatar_placeholder), token_rgb(theme.badge_fill),
            token_rgb(theme.text_channel),       token_rgb(theme.text_speaking),
            token_rgb(theme.text_muted),         token_rgb(theme.separator),
            token_rgb(theme.separator_accent),   token_rgb(theme.text_overflow),
            token_rgb(theme.panel_hairline),     token_rgb(theme.badge_glyph)};
        theme.text_idle = hex(step_off_taken(config.text_idle_colour, taken, 10));
    }
    if (config.text_speaking_colour != Config::kColourAuto) {
        const uint32_t taken[] = {
            token_rgb(theme.avatar_placeholder), token_rgb(theme.badge_fill),
            token_rgb(theme.text_channel),       token_rgb(theme.text_idle),
            token_rgb(theme.text_muted),         token_rgb(theme.separator),
            token_rgb(theme.separator_accent),   token_rgb(theme.text_overflow),
            token_rgb(theme.panel_hairline),     token_rgb(theme.badge_glyph)};
        theme.text_speaking = hex(step_off_taken(config.text_speaking_colour, taken, 10));
    }
    if (config.notification_text_colour != Config::kColourAuto) {
        const uint32_t taken[] = {token_rgb(theme.avatar_placeholder), token_rgb(theme.toast_title),
                                  token_rgb(theme.toast_hairline), token_rgb(theme.toast_accent)};
        theme.toast_body = hex(step_off_taken(config.notification_text_colour, taken, 4));
    }

    return theme;
}

// The surfaces the configuration window offers ready-made, and the opacity each
// one is promised at. Values here and nowhere else: the window's preset row reads
// them through the bridge and tests/theme_contrast.cpp holds every non-Discord,
// non-transparent entry to the 4.5:1 floor -- so a preset cannot be added or moved
// without the measurement following it.
//
//   * transparent   -- the default, on the owner's judgement: no box at all, the
//                      names straight on the game. The surface colour stays the
//                      dark one so the pale ramp is what lands on the game, and
//                      so that raising the opacity slider starts from a surface
//                      that was measured rather than from nothing.
//   * dark          -- #17181c at 88%: the surface whose composite passes the
//                      4.5:1 floor in every scene; see the notes in config.h.
//   * discord       -- the old default, kept for whoever wants the client's own
//                      blurple. Solid, and deliberately not promised a floor: the
//                      blurple cannot carry one, which is the measured reason it
//                      is not the boxed default.
//   * breeze-dark   -- Breeze's View surface (#141618), from BreezeDark.colors.
//   * breeze-light  -- Breeze's Window surface (#EFF0F1). Solid, not 88%: a pale
//                      box at 88% over a dark scene composites down towards the
//                      scene, and the muted grey cannot clear 4.5:1 there without
//                      crossing the idle grey. Pale is promised solid or not at
//                      all -- measured, in tests/theme_contrast.cpp.
//
// The id is stable and untranslated; the window shows its own words for it.
struct Preset {
    const char* id;
    uint32_t colour;
    float opacity;
};
// Four, on the owner's ask. The two Breeze entries are gone -- they were the
// desktop's own surfaces borrowed for a thing that is read over a game rather
// than over this desktop, and the dark one sat two hundredths from `dark` while
// the light one duplicated `light`'s promise. And the purple is named for what it
// is: the overlay stands in for a Discord client, it does not claim to be one, and
// a preset named after somebody else's product said otherwise.
inline constexpr Preset kPresets[] = {
    {"transparent", 0x17181c, 0.0f},
    {"dark", 0x17181c, 0.88f},
    {"light", 0xeff0f1, 1.0f},
    {"purple", 0x5865f2, 1.0f},
};
inline constexpr int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

// How far outside the picture its decorations reach, and therefore what the row has
// to reserve for them. Reserved whether or not anyone is speaking or muted: a row
// that changed width when somebody started talking would make the whole panel
// twitch.
//
// Measured, not guessed: without this the ring crossed the box's padding at every
// small padding and was clipped by the window's own edge, and at avatar sizes above
// about 1.2 the picture itself did the same. tests/panel_geometry.cpp asserts both.
//
// The ring's reach is in reference units and so takes the scale; the badge's is a
// fraction of the radius, which already carries it.
inline float decoration_allowance(const Theme& theme, float radius, float scale) {
    const float ring = theme.ring_allowance * scale;
    const float badge = radius * theme.badge_allowance_factor;
    return ring > badge ? ring : badge;
}

}  // namespace vocem

#endif  // VOCEM_THEME_H

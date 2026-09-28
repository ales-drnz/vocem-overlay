// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.

#include "vocem/panel.h"
#include "vocem/placement.h"

#include <cmath>
#include <cstdio>

#include "vocem/fonts.h"
#ifndef VOCEM_IMCONFIG_INJECTED
#error "vocem/imconfig_injected.h is not in effect: IM_ASSERT would be assert() inside a game"
#endif
#include "vocem/theme.h"

namespace vocem {
namespace {

// A theme colour as ImGui wants it. The theme is deliberately ImGui-free -- the
// configuration window reads the same file and is not built against ImGui -- so
// the conversion happens here, at the one place that draws.
ImU32 col(Colour colour) {
    return IM_COL32(colour.r, colour.g, colour.b, colour.a);
}

// A theme surface as an opaque ImGui colour. Transparency is applied separately,
// from the matching opacity setting, so the two never interfere -- which is why
// this drops the alpha rather than passing it through `col()`.
ImU32 opaque(Colour colour) {
    return IM_COL32(colour.r, colour.g, colour.b, 255);
}

// The outline's ink at the strength it has been asked for. The colour is the
// theme's -- the opposite pole of the text's own ramp; how much of it there is
// belongs to the strength token.
ImU32 outline_ink(const Theme& theme, float strength) {
    Colour ink = theme.text_outline_ink;
    ink.a = static_cast<uint8_t>(strength * 255.0f);
    return col(ink);
}

// One offset per cardinal direction. Four and not eight: at one unit of offset a
// diagonal gap is at most 1 - 1/sqrt(2) of a pixel, which antialiasing owns, and
// eight would double the cost (theme.h, at kTextOutlineStrength).
constexpr float kOutlineDirections[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};

// Draws the text five times: four offset copies in the outline's ink, then the
// real one. The copies go into the same draw list first, so they stay behind,
// and the real text is emitted through ImGui so the layout is unchanged.
void text_outlined(const Theme& theme, const char* text, ImU32 colour, float outline,
                   float scale) {
    if (outline > 0.0f) {
        const ImVec2 position = ImGui::GetCursorScreenPos();
        const float offset = scale < 1.0f ? 1.0f : scale;
        const ImU32 ink = outline_ink(theme, outline);
        for (const auto& direction : kOutlineDirections) {
            ImGui::GetWindowDrawList()->AddText(
                ImVec2(position.x + direction[0] * offset, position.y + direction[1] * offset),
                ink, text);
        }
    }
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::TextUnformatted(text);
    ImGui::PopStyleColor();
}

// The same, for text that wraps. The copies have to be given the identical wrap
// width or they break their lines somewhere else and show up as a second, offset
// paragraph behind the first.
void text_wrapped_outlined(const Theme& theme, const char* text, ImU32 colour, float outline,
                           float scale, float wrap_width) {
    if (outline > 0.0f) {
        const ImVec2 position = ImGui::GetCursorScreenPos();
        const float offset = scale < 1.0f ? 1.0f : scale;
        const ImU32 ink = outline_ink(theme, outline);
        for (const auto& direction : kOutlineDirections) {
            ImGui::GetWindowDrawList()->AddText(
                ImGui::GetFont(), ImGui::GetFontSize(),
                ImVec2(position.x + direction[0] * offset, position.y + direction[1] * offset),
                ink, text, nullptr, wrap_width);
        }
    }
    ImGui::PushStyleColor(ImGuiCol_Text, colour);
    ImGui::PushTextWrapPos(ImGui::GetCursorPos().x + wrap_width);
    ImGui::TextUnformatted(text);
    ImGui::PopTextWrapPos();
    ImGui::PopStyleColor();
}

// The box behind one name, when the panel's surface is drawn there rather than
// around everything (Config::kBoxNames). Decided once a frame in build_panel(),
// because it is the same box at all three places the panel draws text.
struct NameBox {
    bool drawn = false;
    float pad_x = 0.0f;
    float pad_y = 0.0f;
    // The surface, carrying the panel's opacity as its alpha.
    Colour fill;
};

// --- motion -----------------------------------------------------------------
//
// Little and aimed: an in-game overlay that moves is an overlay that distracts.
// Everything here animates alpha, never geometry, so when every phase has
// settled the geometry is identical to the resting one (tests/panel_geometry.cpp).
//
// Durations from Material 3's table: 150-200 ms for a state change, 250-300 ms
// for an entrance or an exit. A phase advances linearly with time and the drawn
// value is 1-(1-phase)^3: rising, M3's decelerate curve; falling, its accelerate
// curve; and a reversal mid-fade continues from where the fade had got to.
constexpr float kRingRiseSeconds = 0.15f;
constexpr float kRingFallSeconds = 0.20f;
// Speech arrives broken -- pauses inside a sentence -- and a ring that fell at
// every one of them would flicker. The fall waits this long first.
constexpr float kRingHoldSeconds = 0.12f;
constexpr float kRowFadeSeconds = 0.20f;
constexpr float kAvatarFadeSeconds = 0.15f;

float eased(float phase) {
    const float inverse = 1.0f - (phase < 0.0f ? 0.0f : (phase > 1.0f ? 1.0f : phase));
    return 1.0f - inverse * inverse * inverse;
}

// An alpha multiplied by an animation's value, exact at 1: a settled animation
// must reproduce the resting byte the geometry test's colour searches look for.
uint8_t scaled(uint8_t alpha, float multiplier) {
    return multiplier >= 1.0f
               ? alpha
               : static_cast<uint8_t>(static_cast<float>(alpha) * multiplier + 0.5f);
}

ImU32 col_scaled(Colour colour, float multiplier) {
    colour.a = scaled(colour.a, multiplier);
    return col(colour);
}

// One line of text, with its own box behind it where the panel puts one there
// and as plain text where it does not.
//
// The cursor is the *box's* top-left, not the text's, and the padding is claimed
// from the layout as well as drawn: a window clips its own draw list, so at
// box_padding 0 a pill outside its items would lose its edges. The trailing item
// is the right-hand pad wide and reaches the pill's bottom, claiming both.
// The radius is half the box's height: a pill at every text size.
void text_boxed(const Theme& theme, const NameBox& box, const char* text, ImU32 colour,
                float outline, float scale, float alpha) {
    if (!box.drawn) {
        text_outlined(theme, text, colour, outline, scale);
        return;
    }
    const float line = ImGui::GetTextLineHeight();
    const ImVec2 start = ImGui::GetCursorPos();
    ImGui::SetCursorPos(ImVec2(start.x + box.pad_x, start.y + box.pad_y));
    const ImVec2 at = ImGui::GetCursorScreenPos();
    const float width = ImGui::CalcTextSize(text).x;
    const float height = line + box.pad_y * 2.0f;
    ImGui::GetWindowDrawList()->AddRectFilled(
        ImVec2(at.x - box.pad_x, at.y - box.pad_y),
        ImVec2(at.x + width + box.pad_x, at.y + line + box.pad_y), col_scaled(box.fill, alpha),
        height * 0.5f);
    text_outlined(theme, text, colour, outline, scale);
    ImGui::SameLine(0.0f, 0.0f);
    ImGui::Dummy(ImVec2(box.pad_x, line + box.pad_y));
}

// The picture of somebody whose picture has not arrived (or who has none): a
// head above a pair of shoulders, so a waiting row reads as a person rather than
// an empty slot. The shoulders are the lens where a larger circle below overlaps
// the disc, built as a path because ImGui clips to rectangles only.
void draw_avatar_placeholder(ImDrawList* draw_list, ImVec2 centre, float radius, Colour disc,
                             Colour mark_colour, float alpha) {
    draw_list->AddCircleFilled(centre, radius, col_scaled(disc, alpha), 0);
    if (radius < 3.0f) {
        return;  // below this the mark is one pixel of mud
    }

    // The mark's colour is a theme token (theme.h, avatar_mark_for), shared with
    // the window's QML; the geometry test identifies the shape by it.
    const ImU32 mark = col_scaled(mark_colour, alpha);

    // The head.
    draw_list->AddCircleFilled(ImVec2(centre.x, centre.y - radius * 0.28f), radius * 0.28f, mark,
                               0);

    // The shoulders. A circle of radius 0.72r whose centre sits 0.87r below the
    // disc's crosses the disc at y = 0.712r, x = +-0.702r; the two arcs between
    // those points bound a convex lens, which is what ImGui can fill in one go.
    //
    // Which way round each arc runs is the whole shape: ImGui walks an arc
    // linearly from a_min to a_max (_PathArcToN). Both pairs here increase and
    // each passes the pole the lens needs: the shoulders' -2.92 -> -0.22 through
    // -pi/2, the disc's 0.79 -> 2.35 through +pi/2.
    // tests/panel_geometry.cpp's placeholder_inside_disc holds the mark inside
    // the picture at three avatar sizes.
    const float shoulder_radius = radius * 0.72f;
    const float shoulder_drop = radius * 0.87f;
    const float meet_y = radius * 0.712f;
    const float meet_x = radius * 0.702f;
    const ImVec2 shoulder_centre(centre.x, centre.y + shoulder_drop);
    const float from = std::atan2(meet_y - shoulder_drop, -meet_x);
    const float to = std::atan2(meet_y - shoulder_drop, meet_x);
    draw_list->PathArcTo(shoulder_centre, shoulder_radius, from, to, 24);
    // Half a pixel past the disc's edge: two antialiased fills that share an
    // edge do not add up to a solid one, and the game would show through as a
    // dark rim under the figure. The overlap puts the mark's own fade where the
    // disc has already faded.
    draw_list->PathArcTo(centre, radius + 0.5f, std::atan2(meet_y, meet_x),
                         std::atan2(meet_y, -meet_x), 24);
    draw_list->PathFillConvex(mark);
}

// What one participant's animations are at. A fixed array parallel to the
// snapshot's 24 users, matched by id: no allocation on the present path, and a
// slot whose id has left the channel is simply claimed by whoever needs one.
//
// There is deliberately no fade *out* for a row: it would draw a kept copy of
// somebody the snapshot no longer contains, a small lie about who is in the
// channel.
struct RowMotion {
    uint64_t id = 0;
    bool speaking = false;
    float ring_phase = 0.0f;
    float row_phase = 0.0f;
    float avatar_phase = 0.0f;
    double quiet_since = 0.0;
};
RowMotion row_motion[kMaxUsers];
double motion_last_now = 0.0;

// The slot for this id, or a slot whose owner is no longer in the snapshot.
// Linear scans over 24 entries, at most once per drawn row.
RowMotion& motion_slot(uint64_t id, const Snapshot& snapshot) {
    for (RowMotion& slot : row_motion) {
        if (slot.id == id) {
            return slot;
        }
    }
    for (RowMotion& slot : row_motion) {
        bool present = false;
        for (uint32_t i = 0; i < snapshot.user_count && !present; ++i) {
            present = snapshot.users[i].id == slot.id;
        }
        if (!present) {
            slot = RowMotion{};
            slot.id = id;
            return slot;
        }
    }
    // Unreachable while the array parallels the snapshot's capacity; the first
    // slot is still a safe answer if it ever stops doing so.
    return row_motion[0];
}

// A round badge in the bottom-right of an avatar, with a symbol drawn into it.
// Discord marks these states with an icon rather than a colour, and so does this:
// a red dot says "something", a crossed-out microphone says which something.
//
// The shapes are drawn rather than loaded: an icon font would be another atlas
// to build and upload inside somebody else's process for two glyphs. Every
// measure is a multiple of a sixteenth of the badge's radius, so the glyph
// lands on the same grid at every avatar size.
void draw_state_badge(ImDrawList* draw_list, const Theme& theme, ImVec2 centre, float radius,
                      bool deafened, float alpha) {
    const ImU32 glyph = col_scaled(theme.badge_glyph, alpha);
    const float nominal = radius * theme.badge_stroke_factor;
    const float stroke = nominal < 1.0f ? 1.0f : nominal;

    draw_list->AddCircleFilled(centre, radius, col_scaled(theme.badge_fill, alpha), 24);
    // The rim, outside the disc: a band of the surface's colour at full opacity,
    // so the glyph never sits directly on the pixels of the game or of an
    // avatar. Centred at radius + rim/2, which puts its inner edge exactly on
    // the disc's.
    const float rim_nominal = radius * theme.badge_rim_stroke_factor;
    const float rim = rim_nominal < 1.0f ? 1.0f : rim_nominal;
    draw_list->AddCircle(centre, radius + rim * 0.5f, col_scaled(theme.badge_rim, alpha), 24, rim);

    if (deafened) {
        // Headphones: a band over the top and an earcup on each side.
        const float band = radius * 0.5f;
        draw_list->PathArcTo(ImVec2(centre.x, centre.y + radius * 0.125f), band,
                             3.1416f, 2.0f * 3.1416f, 16);
        draw_list->PathStroke(glyph, 0, stroke);
        const float cup_w = radius * 0.1875f;
        draw_list->AddRectFilled(ImVec2(centre.x - band - cup_w, centre.y),
                                 ImVec2(centre.x - band + cup_w, centre.y + radius * 0.4375f),
                                 glyph, cup_w);
        draw_list->AddRectFilled(ImVec2(centre.x + band - cup_w, centre.y),
                                 ImVec2(centre.x + band + cup_w, centre.y + radius * 0.4375f),
                                 glyph, cup_w);
    } else {
        // Microphone: a capsule, the bow and the stand under it, and a stroke
        // through the lot.
        const float cap_w = radius * 0.25f;
        draw_list->AddRectFilled(ImVec2(centre.x - cap_w, centre.y - radius * 0.4375f),
                                 ImVec2(centre.x + cap_w, centre.y + radius * 0.0625f),
                                 glyph, cap_w);
        draw_list->PathArcTo(ImVec2(centre.x, centre.y), radius * 0.375f, 0.0f, 3.1416f, 12);
        draw_list->PathStroke(glyph, 0, stroke);
        draw_list->AddLine(ImVec2(centre.x, centre.y + radius * 0.375f),
                           ImVec2(centre.x, centre.y + radius * 0.5625f), glyph, stroke);
        // The bar that means "off", drawn last so it sits on top of the microphone.
        draw_list->AddLine(ImVec2(centre.x - radius * 0.5625f, centre.y - radius * 0.5625f),
                           ImVec2(centre.x + radius * 0.5625f, centre.y + radius * 0.5625f),
                           glyph, stroke * 1.25f);
    }
}

// Whole pixels, for every distance that adds up into the size of a box. ImGui
// truncates the content extent it fits a window to, so a fraction would come off
// the bottom or right padding; on pixel boundaries the preview can follow exactly.
float pixels(float value) { return std::round(value); }

}  // namespace

// Idempotent: the values below are the look at the reference font size, and the
// current scale is applied on top of them, so this can be called again after every
// atlas rebuild without the sizes compounding (ScaleAllSizes would compound them).
void configure_style(const Config& config) {
    const float scale = ui_scale();
    const Theme theme = theme_for(config);
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    // ImGuiCol_Separator is not set: the line under the channel name is drawn
    // by hand in build_panel().
    style.WindowRounding = theme.box_radius * scale;
    style.WindowBorderSize = 0.0f;
    style.WindowPadding =
        ImVec2(pixels(config.box_padding_x * scale), pixels(config.box_padding_y * scale));
    style.ItemSpacing =
        ImVec2(pixels(config.avatar_gap * scale), pixels(config.row_spacing * scale));
    style.FramePadding = ImVec2(pixels(4.0f * scale), pixels(3.0f * scale));
    style.ItemInnerSpacing = ImVec2(pixels(4.0f * scale), pixels(4.0f * scale));
    style.ScrollbarSize = 0.0f;
}

void build_panel(const Snapshot& snapshot, const Config& config, uint32_t width, uint32_t height,
                 AvatarProvider* avatars, double now_seconds) {
    // The "Voice panel" switch in the window's header.
    if (!config.panel_enabled) {
        return;
    }

    // This frame's step for the animation phases, clamped to a tenth of a
    // second: a hitching game or a jumping clock advances an animation, it does
    // not teleport it.
    const float raw_dt = static_cast<float>(now_seconds - motion_last_now);
    const float dt = motion_last_now <= 0.0 ? 0.0f
                     : (raw_dt < 0.0f ? 0.0f : (raw_dt > 0.1f ? 0.1f : raw_dt));
    motion_last_now = now_seconds;

    const float scale = ui_scale();
    const Theme theme = theme_for(config);
    const float outline = theme.panel_text_outline;
    const float line_height = ImGui::GetTextLineHeight();

    // The picture is a multiple of the bare line of text, not
    // GetTextLineHeightWithSpacing(): that includes ItemSpacing.y, the
    // row_spacing setting, and would make the spacing slider resize avatars.
    //
    // The row is at least a line of text tall, and taller when the picture is.
    // The spacing is *not* part of the row: ImGui puts ItemSpacing.y between
    // items, and counting it inside the row too would count it twice.
    const float radius = line_height * theme.avatar_radius_factor * config.avatar_size;
    const float picture = (radius + decoration_allowance(theme, radius, scale)) * 2.0f;

    // Where the surface goes. Rounded through pixels() like every other distance
    // that adds up into the size of a box, so the preview can follow it exactly.
    NameBox name_box;
    name_box.drawn = config.panel_box == Config::kBoxNames;
    if (name_box.drawn) {
        name_box.pad_x = pixels(theme.name_box_padding_x * scale);
        name_box.pad_y = pixels(theme.name_box_padding_y * scale);
        name_box.fill = theme.panel_surface;
        name_box.fill.a = static_cast<uint8_t>(config.opacity * 255.0f + 0.5f);
    }
    // What one line of text occupies, which is the line itself plus whatever its
    // own box reaches past it. The row is at least this tall: the pill is drawn
    // inside the row, so two of them cannot overlap and darken each other where
    // the rows sit close together.
    const float text_block = line_height + name_box.pad_y * 2.0f;

    // Rounded up to a whole pixel, because ImGui truncates the content extent it
    // fits the window to: a fractional row would clip the last avatar at small
    // paddings.
    const float row = std::ceil(text_block > picture ? text_block : picture);

    // The panel is clamped so it cannot leave the screen as its height changes;
    // anchors do not flip at the middle, which would make it jump while dragged.
    // The size is last frame's, because ImGui only knows this frame's after
    // layout; one frame of lag on a dragged panel is invisible.
    static float last_width = 200.0f;
    static float last_height = 80.0f;
    // The distance from the edge of the display, a setting.
    const float inset = config.screen_margin * scale;

    // A position fraction is a place between the margins, not a coordinate
    // (vocem/placement.h).
    ImVec2 position(place_within(config.position_x, last_width, static_cast<float>(width), inset),
                    place_within(config.position_y, last_height, static_cast<float>(height), inset));

    // First, how many people the settings ask for at all (the filters), and
    // the distances a row is built from.
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool horizontal = config.panel_layout == Config::kLayoutHorizontal;
    // The distance between one person and the next: `row_spacing` in both
    // layouts. ItemSpacing.x is `avatar_gap`, picture to name, which is why a
    // horizontal panel passes this explicitly at the SameLine.
    const float cell_gap = pixels(config.row_spacing * scale);
    const float channel_block =
        config.show_channel_name ? text_block + style.ItemSpacing.y * 2.0f : 0.0f;
    const float pitch = row + style.ItemSpacing.y;

    uint32_t wanted_rows = 0;
    for (uint32_t i = 0; i < snapshot.user_count; ++i) {
        const uint32_t flags = snapshot.users[i].flags;
        if (config.only_speaking && (flags & kFlagSpeaking) == 0) continue;
        if (config.hide_self && (flags & kFlagSelf) != 0) continue;
        ++wanted_rows;
    }

    // What the display can actually hold. Left to itself, ImGui clamps an
    // auto-sized window to the viewport and clips a face in half; deciding here
    // ends the panel on a whole person, says how many it left out, and is
    // something the preview can reproduce. A column's budget is one division by
    // the row pitch; a row's is an accumulation, each cell as wide as its name.
    bool truncated = false;
    int row_budget = 0;
    if (horizontal) {
        const float room =
            static_cast<float>(width) - inset * 2.0f - style.WindowPadding.x * 2.0f;
        // The remark is measured at its widest spelling, since the count is
        // not known yet: two digits covers every channel the snapshot carries.
        static_assert(kMaxUsers < 100, "the overflow remark is measured at two digits");
        const float remark =
            cell_gap + ImGui::CalcTextSize("+99 more").x + name_box.pad_x * 2.0f;
        float used = 0.0f;
        int fitted = 0;
        for (uint32_t i = 0; i < snapshot.user_count; ++i) {
            const User& user = snapshot.users[i];
            if (config.only_speaking && (user.flags & kFlagSpeaking) == 0) continue;
            if (config.hide_self && (user.flags & kFlagSelf) != 0) continue;
            const float cell = picture + style.ItemSpacing.x + ImGui::CalcTextSize(user.name).x +
                               name_box.pad_x * 2.0f;
            const float advance = (fitted == 0 ? 0.0f : cell_gap) + cell;
            if (fitted > 0 && used + advance > room) {
                break;
            }
            used += advance;
            ++fitted;
        }
        truncated = fitted < static_cast<int>(wanted_rows);
        // The remark takes the end of the line, so it comes out of the budget.
        // At least one person is always drawn.
        while (truncated && fitted > 1 && used + remark > room) {
            const User* last = nullptr;
            int seen = 0;
            for (uint32_t i = 0; i < snapshot.user_count; ++i) {
                const User& user = snapshot.users[i];
                if (config.only_speaking && (user.flags & kFlagSpeaking) == 0) continue;
                if (config.hide_self && (user.flags & kFlagSelf) != 0) continue;
                if (++seen == fitted) {
                    last = &user;
                    break;
                }
            }
            if (!last) {
                break;
            }
            used -= cell_gap + picture + style.ItemSpacing.x +
                    ImGui::CalcTextSize(last->name).x + name_box.pad_x * 2.0f;
            --fitted;
        }
        row_budget = fitted > 0 ? fitted : 1;
    } else {
        const float room = static_cast<float>(height) - inset * 2.0f -
                           style.WindowPadding.y * 2.0f - channel_block;
        int rows_that_fit =
            pitch > 0.0f ? static_cast<int>((room + style.ItemSpacing.y) / pitch) : 1;
        if (rows_that_fit < 1) {
            rows_that_fit = 1;
        }
        // The line that says how many are missing takes a row of its own, so it is
        // taken out of the budget rather than pushed off the bottom with them.
        truncated = static_cast<int>(wanted_rows) > rows_that_fit;
        row_budget = truncated ? (rows_that_fit > 1 ? rows_that_fit - 1 : 1) : rows_that_fit;
    }

    ImGui::SetNextWindowPos(position, ImGuiCond_Always);
    // No fixed width: with AlwaysAutoResize the box ends where its longest name
    // ends. The constraint stops a pathological name from crossing the screen and
    // never lets the box be wider than the display. A horizontal panel is capped
    // by the display alone, which is what its budget above counted against.
    const float widest_panel = static_cast<float>(width) - inset * 2.0f;
    const float wanted_limit = horizontal ? widest_panel : 520.0f * scale;
    const float panel_limit = wanted_limit < widest_panel ? wanted_limit
                                                          : (widest_panel > 80.0f * scale
                                                                 ? widest_panel
                                                                 : 80.0f * scale);
    ImGui::SetNextWindowSizeConstraints(ImVec2(80.0f * scale, 0.0f),
                                        ImVec2(panel_limit, FLT_MAX));

    ImVec4 background = ImGui::ColorConvertU32ToFloat4(opaque(theme.panel_surface));
    // Nothing at all where the surface is drawn behind the names instead: ImGui
    // culls a fill whose alpha is exactly zero, so the geometry test finds the
    // pills by the surface's own colour.
    background.w = name_box.drawn ? 0.0f : config.opacity;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, background);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_AlwaysAutoResize;

    if (ImGui::Begin("##vocem", nullptr, flags)) {
        // The window's rectangle, read here because only inside Begin is it this
        // frame's.
        const ImVec2 window_pos = ImGui::GetWindowPos();
        const ImVec2 window_size = ImGui::GetWindowSize();

        // The hairline, just inside the edge and drawn by hand. Not ImGui's
        // WindowBorderSize: the content origin is ImMax(WindowPadding.x,
        // WindowBorderSize) (imgui.cpp:7598), so at box_padding_x 0 a border
        // would shift the contents by a unit the QML preview does not know about.
        if (theme.panel_hairline.a > 0) {
            const float nominal = pixels(1.0f * scale);
            const float thickness = nominal < 1.0f ? 1.0f : nominal;
            const float inset = thickness * 0.5f;
            ImGui::GetWindowDrawList()->AddRect(
                ImVec2(window_pos.x + inset, window_pos.y + inset),
                ImVec2(window_pos.x + window_size.x - inset,
                       window_pos.y + window_size.y - inset),
                col(theme.panel_hairline), style.WindowRounding - inset, 0, thickness);
        }

        if (config.show_channel_name) {
            // The heavier weight separates the channel from the names under it.
            if (fonts().strong) {
                ImGui::PushFont(fonts().strong);
            }
            text_boxed(theme, name_box, snapshot.channel_name, col(theme.text_channel), outline,
                       scale, 1.0f);
            if (fonts().strong) {
                ImGui::PopFont();
            }

            // The line under the channel name, by hand instead of
            // ImGui::Separator(), which draws one colour: the accent bar, then
            // the hairline to the edge. The layout is Separator's: an item one
            // thickness tall that claims no width (claiming it would stop an
            // auto-resized window from ever shrinking), drawn across this
            // frame's content region.
            {
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const float span = ImGui::GetContentRegionAvail().x;
                const float nominal = pixels(1.0f * scale);
                const float thickness = nominal < 1.0f ? 1.0f : nominal;
                const float wanted = pixels(theme.separator_accent_length * scale);
                const float bar = wanted < span ? wanted : span;
                ImDrawList* line = ImGui::GetWindowDrawList();
                line->AddRectFilled(ImVec2(at.x, at.y), ImVec2(at.x + bar, at.y + thickness),
                                    col(theme.separator_accent));
                if (span > bar) {
                    line->AddRectFilled(ImVec2(at.x + bar, at.y),
                                        ImVec2(at.x + span, at.y + thickness),
                                        col(theme.separator));
                }
                ImGui::Dummy(ImVec2(0.0f, thickness));
            }
        }

        int drawn = 0;
        for (uint32_t i = 0; i < snapshot.user_count; ++i) {
            if (drawn >= row_budget) {
                break;
            }
            const User& user = snapshot.users[i];
            const bool speaking = (user.flags & kFlagSpeaking) != 0;
            const bool muted = (user.flags & kFlagMuted) != 0;
            const bool deafened = (user.flags & kFlagDeafened) != 0;
            const bool is_self = (user.flags & kFlagSelf) != 0;

            if (config.only_speaking && !speaking) {
                continue;
            }
            if (config.hide_self && is_self) {
                continue;
            }

            // Sideways, one person follows the last on the same line; upright,
            // ImGui's own newline does it with ItemSpacing.y.
            if (horizontal && drawn > 0) {
                ImGui::SameLine(0.0f, cell_gap);
            }

            const ImVec2 cursor = ImGui::GetCursorScreenPos();
            const ImVec2 centre(cursor.x + picture * 0.5f, cursor.y + row * 0.5f);
            ImDrawList* draw_list = ImGui::GetWindowDrawList();

            // This participant's animation phases: the ring rises while they
            // speak and falls -- after the hold -- when they stop; the whole row
            // fades in over their first fifth of a second in the channel.
            RowMotion& motion = motion_slot(user.id, snapshot);
            if (speaking != motion.speaking) {
                motion.speaking = speaking;
                if (!speaking) {
                    motion.quiet_since = now_seconds;
                }
            }
            if (motion.speaking) {
                motion.ring_phase += dt / kRingRiseSeconds;
            } else if (now_seconds - motion.quiet_since >= kRingHoldSeconds) {
                motion.ring_phase -= dt / kRingFallSeconds;
            }
            motion.ring_phase =
                motion.ring_phase < 0.0f ? 0.0f : (motion.ring_phase > 1.0f ? 1.0f : motion.ring_phase);
            motion.row_phase += dt / kRowFadeSeconds;
            motion.row_phase = motion.row_phase > 1.0f ? 1.0f : motion.row_phase;
            const float row_alpha = eased(motion.row_phase);
            // The picture of somebody who is not talking is quieter
            // (avatar_idle_opacity in config.h). It rides the ring's phase, hold
            // included, rather than the speaking flag, which drops between words.
            // The placeholder, the image and the muted scrim take it; the badge
            // does not, because the state it names is still true.
            const float picture_alpha =
                config.avatar_idle_opacity +
                (1.0f - config.avatar_idle_opacity) * eased(motion.ring_phase);

            // Round avatars come from rounding the image corners to half its size.
            // The picture crossfades in over the placeholder when it arrives.
            const ImTextureID avatar = avatars ? avatars->texture(user.id, user.avatar_hash) : 0;
            if (avatar) {
                motion.avatar_phase += dt / kAvatarFadeSeconds;
                motion.avatar_phase = motion.avatar_phase > 1.0f ? 1.0f : motion.avatar_phase;
            } else {
                motion.avatar_phase = 0.0f;
            }
            if (!avatar || motion.avatar_phase < 1.0f) {
                // Not downloaded yet, still arriving, or an account with no
                // picture at all. The placeholder holds the layout so nothing
                // jumps when an image lands.
                draw_avatar_placeholder(draw_list, centre, radius, theme.avatar_placeholder,
                                        theme.avatar_mark, row_alpha * picture_alpha);
            }
            if (avatar) {
                const uint8_t image_alpha =
                    scaled(255, row_alpha * picture_alpha * eased(motion.avatar_phase));
                draw_list->AddImageRounded(avatar, ImVec2(centre.x - radius, centre.y - radius),
                                           ImVec2(centre.x + radius, centre.y + radius),
                                           ImVec2(0, 0), ImVec2(1, 1),
                                           IM_COL32(255, 255, 255, image_alpha), radius);
            }

            {
                // Always drawn, nearly transparent at rest: the ring is a border
                // that goes from clear to green, never a shape that appears, so
                // the row cannot reflow. Drawn outside the avatar with 48
                // segments, so the avatar's corners do not show through. The
                // stroke follows the radius, floored at a device pixel.
                //
                // At rest the alpha is 1, not 0: ImGui culls a primitive whose
                // alpha is exactly zero (imgui_draw.cpp:768), and the geometry
                // test asserts one ring per picture. The animation runs over
                // the other 254.
                Colour ring = theme.speaking_ring;
                ring.a = static_cast<uint8_t>(
                    1.0f + 254.0f * eased(motion.ring_phase) * row_alpha + 0.5f);
                const float nominal = radius * theme.ring_width_factor;
                const float ring_stroke = nominal < 1.0f ? 1.0f : nominal;
                draw_list->AddCircle(centre, radius + theme.ring_offset * scale,
                                     col(ring), 48, ring_stroke);
            }

            // Muted or deafened: the picture is dimmed and the state named by an
            // icon -- dimming alone is ambiguous, and the icon alone gets lost
            // against a busy avatar.
            if ((muted || deafened) && config.show_muted_state) {
                draw_list->AddCircleFilled(centre, radius,
                                           col_scaled(theme.avatar_scrim,
                                                      row_alpha * picture_alpha),
                                           32);
                const float offset = radius * theme.badge_offset_factor;
                draw_state_badge(draw_list, theme, ImVec2(centre.x + offset, centre.y + offset),
                                 radius * theme.badge_radius_factor, deafened, row_alpha);
            }

            // The picture occupies its own box and nothing more: the gap to the
            // name is ItemSpacing.x, the avatar_gap setting.
            ImGui::Dummy(ImVec2(picture, row));
            ImGui::SameLine();
            // Centred against the picture rather than at the top of the row --
            // the text's own block, line plus box, so the pill sits in the
            // middle of the row and the glyphs in the middle of the pill.
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (row - text_block) * 0.5f);

            // Who is talking, said twice: the ring, and the name at full strength
            // while everybody else's is greyed -- a thin ring alone is hard to
            // find over a busy game. Muted or deafened participants are dimmer
            // still, as Discord does it.
            const bool dim = (muted || deafened) && config.show_muted_state;
            const Colour name_colour = dim        ? theme.text_muted
                                       : speaking ? theme.text_speaking
                                                  : theme.text_idle;
            // The joining fade rides on the alpha, the outline's strength and the
            // box, which are raw draw-list colours a style alpha cannot reach.
            text_boxed(theme, name_box, user.name, col_scaled(name_colour, row_alpha),
                       outline * row_alpha, scale, row_alpha);
            ++drawn;
        }

        // Whoever did not fit, counted rather than dropped in silence.
        if (truncated) {
            char remainder[32];
            std::snprintf(remainder, sizeof(remainder), "+%d more",
                          static_cast<int>(wanted_rows) - drawn);
            if (horizontal) {
                // At the end of the line, in line with the names.
                ImGui::SameLine(0.0f, cell_gap);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (row - text_block) * 0.5f);
            } else {
                ImGui::Dummy(ImVec2(picture, 0.0f));
                ImGui::SameLine();
            }
            text_boxed(theme, name_box, remainder, col(theme.text_overflow), outline, scale,
                       1.0f);
        }
        const ImVec2 size = ImGui::GetWindowSize();
        if (size.x > 1.0f && size.y > 1.0f) {
            last_width = size.x;
            last_height = size.y;
        }
    }
    ImGui::End();
    ImGui::PopStyleColor();
}

void build_notification(const Snapshot& snapshot, const Config& config, uint32_t width,
                        uint32_t height, AvatarProvider* avatars, double now_seconds) {
    // The predicate, not a copy of it: panel.h promises one spelling of
    // "something to draw" for the paths AND the build functions.
    if (!notification_wanted(snapshot, config, now_seconds)) {
        return;
    }

    const double age = now_seconds - snapshot.notification.received;

    // In over a quarter of a second, out over three tenths, through the one
    // easing map (see the motion block). Both are functions of the
    // notification's own timestamps, so the toast keeps no state between frames.
    constexpr float kToastEntrySeconds = 0.25f;
    constexpr float kToastExitSeconds = 0.30f;
    const double remaining = config.notification_seconds - age;
    const float entry = eased(static_cast<float>(age) / kToastEntrySeconds);
    const float fade = entry * eased(static_cast<float>(remaining) / kToastExitSeconds);

    // The message has a size of its own on top of the shared one, applied to the
    // whole box -- text, picture, padding and corners -- not only its width.
    const float scale = ui_scale() * config.notification_scale;
    const Theme theme = theme_for(config);
    // The distance from the edge belongs to the display, so it is not multiplied
    // by the message's own size; it is the message's own setting, because the two
    // boxes are placed independently.
    const float inset = config.notification_margin * ui_scale();
    // Wide enough to read, never wider than the screen it has to fit on.
    const float widest = static_cast<float>(width) - inset * 2.0f;
    const float wanted = 320.0f * scale;
    const float toast_width = pixels(wanted < widest ? wanted : (widest > 1.0f ? widest : wanted));

    const bool right = config.notification_corner == 1 || config.notification_corner == 3;
    const bool bottom = config.notification_corner == 2 || config.notification_corner == 3;
    ImVec2 position(right ? static_cast<float>(width) - inset : inset,
                    bottom ? static_cast<float>(height) - inset : inset);
    const ImVec2 pivot(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f);
    // Placed by the corner and its own margin, which is what the anchor marks in
    // the window are drawn from as well (vocem/placement.h).

    // The entrance also slides in from the anchored edge: eight reference units
    // -- half the default margin, felt rather than seen -- covered by the same
    // decelerate that carries the fade. Zero the moment the entry settles, so
    // the resting position is exactly the one the geometry test measures.
    const float slide = (1.0f - entry) * 8.0f * ui_scale();
    position.x += right ? slide : -slide;

    ImGui::SetNextWindowPos(position, ImGuiCond_Always, pivot);
    ImGui::SetNextWindowSize(ImVec2(toast_width, 0.0f), ImGuiCond_Always);

    ImVec4 background = ImGui::ColorConvertU32ToFloat4(opaque(theme.toast_surface));
    // Its own setting, independent of the panel's: small text on a see-through
    // box over a bright scene cannot be read.
    background.w = config.notification_opacity * fade;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, background);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, fade);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, theme.box_radius * scale);
    // The spacings are the shared settings at the message's own size. Pushed before
    // Begin because the padding is consumed as the window opens.
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding,
                        ImVec2(pixels(config.box_padding_x * scale),
                               pixels(config.box_padding_y * scale)));
    // The horizontal gap is `avatar_gap`, exactly as in the panel: it is the same
    // distance, between a picture and the text beside it.
    //
    // The vertical one is **not** `row_spacing`: that is the gap between two
    // people, and the sender and the message are two lines of one block, whose
    // leading is already inside the line height. Zero means "no gap beyond the
    // type's own", not "the lines touch".
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                        ImVec2(pixels(config.avatar_gap * scale), 0.0f));

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_AlwaysAutoResize;

    if (ImGui::Begin("##vocem_toast", nullptr, flags)) {
        // The box's edge and its accent, drawn by hand like the panel's edge and
        // multiplied by the fade here: raw draw-list colours the style alpha
        // pushed above does not reach.
        {
            const ImVec2 window_pos = ImGui::GetWindowPos();
            const ImVec2 window_size = ImGui::GetWindowSize();
            ImDrawList* draw_list = ImGui::GetWindowDrawList();
            const auto faded = [fade](Colour colour) {
                colour.a = static_cast<uint8_t>(static_cast<float>(colour.a) * fade);
                return col(colour);
            };
            const float rounding = theme.box_radius * scale;

            // The blurple's bar, down the left edge: three units wide, stopped
            // where the corner curvature starts, its outer corners rounded by
            // its own width (the mockup's 0 3px 3px 0).
            if (theme.toast_accent.a > 0) {
                const float bar = pixels(theme.toast_accent_width * scale);
                draw_list->AddRectFilled(
                    ImVec2(window_pos.x, window_pos.y + rounding),
                    ImVec2(window_pos.x + bar, window_pos.y + window_size.y - rounding),
                    faded(theme.toast_accent), bar, ImDrawFlags_RoundCornersRight);
            }

            // The hairline, just inside the edge: same treatment, same reasons
            // as the panel's (WindowBorderSize would move the content at
            // padding 0 -- imgui.cpp:7598).
            if (theme.toast_hairline.a > 0) {
                const float nominal = pixels(1.0f * scale);
                const float thickness = nominal < 1.0f ? 1.0f : nominal;
                const float inset = thickness * 0.5f;
                draw_list->AddRect(
                    ImVec2(window_pos.x + inset, window_pos.y + inset),
                    ImVec2(window_pos.x + window_size.x - inset,
                           window_pos.y + window_size.y - inset),
                    faded(theme.toast_hairline), rounding - inset, 0, thickness);
            }
        }

        // The text at the message's own size as well: magnified from the panel's
        // atlas rather than a second atlas in every game process for a box that
        // appears for a few seconds. At the default size nothing is scaled.
        ImGui::SetWindowFontScale(config.notification_scale);

        const float avatar_radius = ImGui::GetTextLineHeight() * 0.9f;
        const float diameter = avatar_radius * 2.0f;

        // The picture is centred against the text beside it, and the text against
        // the picture, so whichever is taller decides the box. Both heights have to
        // be known before either is drawn, so the wrap width is worked out here:
        // ImGui lays out as it goes.
        const float spacing_x = ImGui::GetStyle().ItemSpacing.x;
        const float text_width = ImGui::GetContentRegionAvail().x - diameter - spacing_x;
        const float body_height =
            ImGui::CalcTextSize(snapshot.notification.body, nullptr, false, text_width).y;
        const float text_height =
            ImGui::GetTextLineHeight() + ImGui::GetStyle().ItemSpacing.y + body_height;
        const float content_height = diameter > text_height ? diameter : text_height;

        const float start_y = ImGui::GetCursorPosY();
        ImGui::SetCursorPosY(start_y + (content_height - diameter) * 0.5f);

        const ImVec2 cursor = ImGui::GetCursorScreenPos();
        const ImVec2 centre(cursor.x + avatar_radius, cursor.y + avatar_radius);
        ImDrawList* draw_list = ImGui::GetWindowDrawList();

        const ImTextureID avatar =
            avatars ? avatars->texture(snapshot.notification.user_id,
                                       snapshot.notification.avatar_hash)
                    : 0;
        if (avatar) {
            draw_list->AddImageRounded(avatar,
                                       ImVec2(centre.x - avatar_radius, centre.y - avatar_radius),
                                       ImVec2(centre.x + avatar_radius, centre.y + avatar_radius),
                                       ImVec2(0, 0), ImVec2(1, 1),
                                       IM_COL32(255, 255, 255, static_cast<int>(255 * fade)),
                                       avatar_radius);
        } else {
            // The fade by hand, exactly as the image above: the placeholder goes
            // into the draw list as raw colours the pushed style alpha cannot
            // reach (the panel's rows pass row_alpha here for the same reason).
            draw_avatar_placeholder(draw_list, centre, avatar_radius, theme.avatar_placeholder,
                                    theme.avatar_mark, fade);
        }

        // As in the panel: the picture's box is the picture, and the gap beside it
        // is the avatar_gap setting through ItemSpacing.
        ImGui::Dummy(ImVec2(diameter, diameter));
        ImGui::SameLine();
        ImGui::SetCursorPosY(start_y + (content_height - text_height) * 0.5f);

        ImGui::BeginGroup();
        // The same outline as the panel's, from the same switch. The strength
        // rides the fade, because the copies bypass the pushed style alpha (the
        // panel does the same with row_alpha).
        const float outline = theme.toast_text_outline * fade;

        // The sender is the part that has to be readable at a glance, mid-game,
        // so it gets the heavier weight.
        if (fonts().strong) {
            ImGui::PushFont(fonts().strong);
        }
        text_outlined(theme, snapshot.notification.title, col(theme.toast_title), outline, scale);
        if (fonts().strong) {
            ImGui::PopFont();
        }

        // Wrapped, because a direct message is not guaranteed to be short.
        text_wrapped_outlined(theme, snapshot.notification.body, col(theme.toast_body), outline,
                              scale, text_width);
        ImGui::EndGroup();
    }
    ImGui::End();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor();
}

}  // namespace vocem

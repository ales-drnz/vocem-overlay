// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.

#include "vocem/panel.h"
#include "vocem/placement.h"

#include <cmath>
#include <cstdio>

#include "vocem/fonts.h"
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
// eight directions measured exactly twice the cost of four for that subpixel
// (the numbers are in theme.h, at kTextOutlineStrength).
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

// --- motion -----------------------------------------------------------------
//
// Little and aimed: an in-game overlay that moves is an overlay that distracts.
// Everything here animates alpha, never geometry -- state arrives as colour on
// shapes that were always there (the rule the ring already lives by), so when
// every phase has settled the measured geometry is identical to the resting one,
// and tests/panel_geometry.cpp measures twice to hold it there.
//
// Durations from Material 3's table, not invented: 150-200 ms for a state
// change, 250-300 ms for an entrance or an exit. One easing map serves both
// directions: a phase advances linearly with time and the drawn value is
// 1-(1-phase)^3 -- rising, that is the decelerate curve M3 asks of an entrance;
// falling, the same map traversed backwards is exactly the accelerate curve it
// asks of an exit; and because the phase itself is continuous, somebody who
// starts talking mid-fade rises from where the fade had got to instead of
// snapping to zero.
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
// must reproduce the resting byte, or the geometry test's colour searches would
// be looking for a colour the drawing has stopped using.
uint8_t scaled(uint8_t alpha, float multiplier) {
    return multiplier >= 1.0f
               ? alpha
               : static_cast<uint8_t>(static_cast<float>(alpha) * multiplier + 0.5f);
}

ImU32 col_scaled(Colour colour, float multiplier) {
    colour.a = scaled(colour.a, multiplier);
    return col(colour);
}

// The picture of somebody whose picture has not arrived.
//
// A flat disc says nothing. At a glance it reads as an empty slot rather than as
// a person the overlay is still fetching, and for an account with no picture at
// all it never becomes anything else. This draws the silhouette every interface
// uses for the same job, a head above a pair of shoulders, so a row that is
// waiting looks like a row with somebody in it.
//
// The shoulders are the lens where a larger circle below overlaps the disc, built
// as a path rather than clipped. ImGui clips to rectangles only, and a rectangle
// clip would leave square corners exactly where the disc is roundest.
void draw_avatar_placeholder(ImDrawList* draw_list, ImVec2 centre, float radius, Colour disc,
                             Colour mark_colour, float alpha) {
    draw_list->AddCircleFilled(centre, radius, col_scaled(disc, alpha), 0);
    if (radius < 3.0f) {
        return;  // below this the mark is one pixel of mud
    }

    // The mark's colour is a token now (theme.h, avatar_mark_for): it was blended
    // here and again in the window's QML, and a colour this file draws a shape in
    // is a colour the geometry measurement identifies that shape by.
    const ImU32 mark = col_scaled(mark_colour, alpha);

    // The head.
    draw_list->AddCircleFilled(ImVec2(centre.x, centre.y - radius * 0.28f), radius * 0.28f, mark,
                               0);

    // The shoulders. A circle of radius 0.72r whose centre sits 0.87r below the
    // disc's crosses the disc at y = 0.712r, x = +-0.702r; the two arcs between
    // those points bound a convex lens, which is what ImGui can fill in one go.
    //
    // Which way round each arc runs is the whole shape -- ImGui walks an arc
    // linearly from a_min to a_max (imgui_draw.cpp, _PathArcToN), so a pair
    // handed over decreasing is traversed decreasing. Both pairs here increase
    // and each passes the pole the lens needs: the shoulders' -2.92 -> -0.22
    // through -pi/2, the top of that circle, and the disc's 0.79 -> 2.35 through
    // +pi/2, its bottom. Measured rather than reasoned about, because the sign
    // of atan2 for a point above the centre is exactly the sort of thing a
    // reading gets backwards: tests/panel_geometry.cpp's placeholder_inside_disc
    // holds the mark inside the picture at three avatar sizes.
    const float shoulder_radius = radius * 0.72f;
    const float shoulder_drop = radius * 0.87f;
    const float meet_y = radius * 0.712f;
    const float meet_x = radius * 0.702f;
    const ImVec2 shoulder_centre(centre.x, centre.y + shoulder_drop);
    const float from = std::atan2(meet_y - shoulder_drop, -meet_x);
    const float to = std::atan2(meet_y - shoulder_drop, meet_x);
    draw_list->PathArcTo(shoulder_centre, shoulder_radius, from, to, 24);
    // Half a pixel past the disc's edge, and that half pixel is the whole point.
    // The lens's lower boundary IS the disc's edge, and two antialiased fills
    // that share an edge do not add up to a solid one: ImGui fades each of them
    // out across the same pixel, so where the shoulders meet the picture both
    // coverages are partial and the game shows through between them. Measured on
    // a captured frame (VOCEM_CAPTURE_FRAME, the column under the left shoulder):
    // the pixel read 0x3e4953 against a 0x4f545c disc and a 0x192633 scene --
    // darker than the disc it sits in, which is the dark rim under the figure
    // the owner reported. Overlapping by half a device pixel puts the mark's own
    // fade where the disc has already faded, which is what the disc's edge looks
    // like everywhere else.
    draw_list->PathArcTo(centre, radius + 0.5f, std::atan2(meet_y, meet_x),
                         std::atan2(meet_y, -meet_x), 24);
    draw_list->PathFillConvex(mark);
}

// What one participant's animations are at. A fixed array parallel to the
// snapshot's 24 users, matched by id: no allocation on the present path, and a
// slot whose id has left the channel is simply claimed by whoever needs one.
//
// There is deliberately no fade *out* for a row: the plan asked for one, and it
// is not here on judgement. A leaving row would have to be drawn from a kept
// copy of somebody the snapshot no longer contains -- a ghost, holding a dead
// name on the screen and the box open for a fifth of a second, every frame of
// it a small lie about who is in the channel. The leave is the one transition
// where the honest answer is disappearance.
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
// The shapes are drawn rather than loaded, because an icon font would be another
// atlas to build and upload inside somebody else's process for two glyphs.
//
// Every measure below is a multiple of a sixteenth of the badge's radius. The
// old values -- 0.46, 0.34, 0.12, 0.26 -- were each tuned by eye in isolation
// and sat a hundredth or two off any step for no reason anybody could name; the
// grid is what lets the glyph be checked at avatar_size 0.5 the same way as at
// 2.0, because a stroke that lands between pixels at one size lands between
// them at every size.
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
// truncates the content extent it fits a window to, so a spacing with a fraction in
// it is a fraction the bottom or right padding loses -- and a preview cannot
// reproduce a figure the drawing then rounds off differently. Rounding here puts
// the whole layout on pixel boundaries, which the preview can follow exactly.
float pixels(float value) { return std::round(value); }

}  // namespace

// Idempotent: the values below are the look at the reference font size, and the
// current scale is applied on top of them, so this can be called again after every
// atlas rebuild without the sizes compounding.
//
// Takes the config because the paddings are settings now. ScaleAllSizes is not used
// -- it would multiply the values a second time.
void configure_style(const Config& config) {
    const float scale = ui_scale();
    const Theme theme = theme_for(config);
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();
    // ImGuiCol_Separator is deliberately not set any more: the line under the
    // channel name is drawn by hand in build_panel(), because it has two
    // segments now and ImGui::Separator() can only draw one colour. Setting a
    // style colour nothing reads would be configuration that lies.
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
    // The "Voice panel" switch in the window's header, honoured here at last. It
    // was read, saved and toggled -- and no drawing code ever looked at it, so
    // the switch did nothing: the inert control, the worst defect a window can
    // carry. The message box has always had this exact guard on its own switch,
    // two lines into build_notification().
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

    // The picture is a multiple of the bare line of text -- GetTextLineHeight(),
    // deliberately not GetTextLineHeightWithSpacing(). The with-spacing figure
    // includes ItemSpacing.y, which configure_style() sets from the row_spacing
    // setting, so a radius derived from it made the pictures follow the spacing
    // slider: dragging "how far apart the rows sit" resized every avatar on the
    // way past, which is two settings in one slider and neither of them asked
    // for it.
    //
    // The row is at least a line of text tall, and taller when the picture is:
    // an avatar large enough to outgrow the line, drawn centred on a row that
    // had not grown to hold it, put it through the padding above and through the
    // row below. The spacing is *not* part of the row: rows are laid out as
    // items, ImGui puts ItemSpacing.y between items, and a row that carried the
    // spacing inside its own height as well counted it twice in the pitch.
    const float radius = line_height * theme.avatar_radius_factor * config.avatar_size;
    const float picture = (radius + decoration_allowance(theme, radius, scale)) * 2.0f;
    // Rounded up to a whole pixel, because ImGui truncates the content extent it
    // fits the window to: a row whose height ended in a fraction left the last one
    // hanging a pixel below the box, which is visible as a clipped avatar wherever
    // the padding is small.
    const float row = std::ceil(line_height > picture ? line_height : picture);

    // The stored fraction is the panel's top-left, and it is clamped here so the
    // panel cannot leave the screen as its height changes with the number of
    // participants. Clamping rather than switching anchor corners is deliberate: an
    // anchor that flips at the middle of the screen makes the panel jump by its own
    // width while the user is dragging it.
    //
    // The size is last frame's, because ImGui only knows this frame's after layout.
    // One frame of lag on a panel that moves when a human drags it is invisible.
    static float last_width = 200.0f;
    static float last_height = 80.0f;
    // The distance from the edge of the display is a setting, and it is the same
    // setting for both boxes. This was a hardcoded 12 while the message box used
    // screen_margin, so the two sat at different distances from the edge and the
    // slider moved only one of them -- which is also why no preview could agree
    // with both.
    const float inset = config.screen_margin * scale;

    // A position fraction is a place between the margins, not a coordinate:
    // vocem/placement.h says why, and the middle-of-a-side anchors are the reason
    // it had to change -- they used to put the panel's top edge at half the display
    // and hang the box below the middle.
    ImVec2 position(place_within(config.position_x, last_width, static_cast<float>(width), inset),
                    place_within(config.position_y, last_height, static_cast<float>(height), inset));

    // How many people the display can actually hold. Left to itself, ImGui clamps
    // an auto-sized window to the viewport and clips whatever does not fit, which
    // ends the panel on a face cut in half at the bottom edge -- and at a size the
    // configuration window has no way to predict, since the clamp is ImGui's rather
    // than ours. Deciding here means the panel always ends on a whole row, says how
    // many people it left out, and does something the preview can reproduce.
    const ImGuiStyle& style = ImGui::GetStyle();
    const bool horizontal = config.panel_layout == Config::kLayoutHorizontal;
    // The distance between one person and the next. It is `row_spacing` in both
    // layouts, because that is what the setting says it is: turned sideways, the
    // gap between two rows of a list is the gap between two cells of a line.
    // ItemSpacing.x is `avatar_gap` and stays what it is -- the distance from a
    // picture to the name beside it -- which is why a horizontal panel asks for
    // its spacing explicitly at the SameLine rather than through the style.
    const float cell_gap = pixels(config.row_spacing * scale);
    const float channel_block =
        config.show_channel_name ? ImGui::GetTextLineHeight() + style.ItemSpacing.y * 2.0f : 0.0f;
    const float pitch = row + style.ItemSpacing.y;

    uint32_t wanted_rows = 0;
    for (uint32_t i = 0; i < snapshot.user_count; ++i) {
        const uint32_t flags = snapshot.users[i].flags;
        if (config.only_speaking && (flags & kFlagSpeaking) == 0) continue;
        if (config.hide_self && (flags & kFlagSelf) != 0) continue;
        ++wanted_rows;
    }

    // What the display can actually hold. Left to itself, ImGui clamps an
    // auto-sized window to the viewport and clips whatever does not fit, which
    // ends the panel on a face cut in half -- and at a size the configuration
    // window has no way to predict, since the clamp is ImGui's rather than ours.
    // Deciding here means the panel always ends on a whole person, says how many
    // it left out, and does something the preview can reproduce.
    //
    // The two layouts run out of room in different directions: a column against
    // the display's height, a row against its width. A column's people are all
    // the same height, so its budget is one division; a row's are each as wide
    // as their own name, so its budget is an accumulation.
    bool truncated = false;
    int row_budget = 0;
    if (horizontal) {
        const float room =
            static_cast<float>(width) - inset * 2.0f - style.WindowPadding.x * 2.0f;
        // The remark is measured at its widest plausible spelling rather than
        // the exact one, which is not known until it is known how many are
        // missing: two digits covers a channel of 24, which is all the snapshot
        // can carry. Asserted rather than remembered -- a wider channel would
        // leave the line reserving less room than the remark takes.
        static_assert(kMaxUsers < 100, "the overflow remark is measured at two digits");
        const float remark = cell_gap + ImGui::CalcTextSize("+99 more").x;
        float used = 0.0f;
        int fitted = 0;
        for (uint32_t i = 0; i < snapshot.user_count; ++i) {
            const User& user = snapshot.users[i];
            if (config.only_speaking && (user.flags & kFlagSpeaking) == 0) continue;
            if (config.hide_self && (user.flags & kFlagSelf) != 0) continue;
            const float cell = picture + style.ItemSpacing.x + ImGui::CalcTextSize(user.name).x;
            const float advance = (fitted == 0 ? 0.0f : cell_gap) + cell;
            if (fitted > 0 && used + advance > room) {
                break;
            }
            used += advance;
            ++fitted;
        }
        truncated = fitted < static_cast<int>(wanted_rows);
        // The remark takes the end of the line, so it is taken out of the budget
        // rather than pushed off the edge with the people it is about. At least
        // one person is always drawn: a panel of nothing but "+8 more" would say
        // less than the panel it replaced.
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
            used -= cell_gap + picture + style.ItemSpacing.x + ImGui::CalcTextSize(last->name).x;
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
    // ends, which is what it should have done from the start -- a panel padded out
    // to 280 units looked like a bug in every screenshot of it. The constraint
    // stops a pathological display name from crossing the screen, and never lets
    // the box be wider than the display itself: at three times the size on a small
    // output, 520 units is wider than the screen.
    //
    // A horizontal panel is as wide as the people in it, so the 520-unit cap is
    // not its cap: what stops it is the display, which is also what its own
    // budget above counted against. Capping it at 520 would have cut the line
    // off at the third person and left the box claiming there was no room.
    const float widest_panel = static_cast<float>(width) - inset * 2.0f;
    const float wanted_limit = horizontal ? widest_panel : 520.0f * scale;
    const float panel_limit = wanted_limit < widest_panel ? wanted_limit
                                                          : (widest_panel > 80.0f * scale
                                                                 ? widest_panel
                                                                 : 80.0f * scale);
    ImGui::SetNextWindowSizeConstraints(ImVec2(80.0f * scale, 0.0f),
                                        ImVec2(panel_limit, FLT_MAX));

    ImVec4 background = ImGui::ColorConvertU32ToFloat4(opaque(theme.panel_surface));
    background.w = config.opacity;
    ImGui::PushStyleColor(ImGuiCol_WindowBg, background);

    const ImGuiWindowFlags flags =
        ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse |
        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_AlwaysAutoResize;

    if (ImGui::Begin("##vocem", nullptr, flags)) {
        // The window's rectangle, read here because only inside Begin is it this
        // frame's. There used to be a drop shadow drawn off it as well, into the
        // background draw list; removed on the owner's judgement -- the hairline
        // alone says where the box ends.
        const ImVec2 window_pos = ImGui::GetWindowPos();
        const ImVec2 window_size = ImGui::GetWindowSize();

        // The hairline, just inside the edge and drawn by hand. Not ImGui's
        // WindowBorderSize: the content origin is ImMax(WindowPadding.x,
        // WindowBorderSize) (imgui.cpp:7598), so with box_padding_x at 0 -- a real
        // setting, and a tested extreme -- a 1-pixel border would shift the
        // contents by a unit the QML preview does not know about. This draws at
        // the same place at every padding and moves nothing.
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
            // The heavier weight is what separates the channel from the names under
            // it; before there was one font and the distinction had to be made with
            // colour alone.
            if (fonts().strong) {
                ImGui::PushFont(fonts().strong);
            }
            text_outlined(theme, snapshot.channel_name, col(theme.text_channel), outline, scale);
            if (fonts().strong) {
                ImGui::PopFont();
            }

            // The line under the channel name, by hand instead of
            // ImGui::Separator(): it is two segments now -- the blurple accent
            // bar, then the hairline to the edge -- and Separator can only draw
            // one colour. The layout is Separator's exactly: an item one
            // thickness tall that claims no width of its own (claiming the
            // available width would stop an auto-resized window from ever
            // shrinking, since this frame's width would become next frame's
            // content), drawn across the content region the window had this
            // frame.
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

            // Sideways, one person follows the last on the same line, at the
            // distance the row spacing asks for. Upright, ImGui's own newline
            // does it and ItemSpacing.y is that distance -- which is why this is
            // the only line the two layouts do not share.
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

            // Round avatars come from rounding the image corners to half its size,
            // the same trick the client's CSS uses. The picture crossfades in over
            // the placeholder when it arrives from the cache, so a face appears
            // rather than pops.
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
                                        theme.avatar_mark, row_alpha);
            }
            if (avatar) {
                const uint8_t image_alpha = scaled(255, row_alpha * eased(motion.avatar_phase));
                draw_list->AddImageRounded(avatar, ImVec2(centre.x - radius, centre.y - radius),
                                           ImVec2(centre.x + radius, centre.y + radius),
                                           ImVec2(0, 0), ImVec2(1, 1),
                                           IM_COL32(255, 255, 255, image_alpha), radius);
            }

            {
                // Always drawn, transparent at rest -- StreamKit's trick: the
                // ring is a border of the picture that goes from clear to green,
                // never a shape that appears, so the row cannot reflow when
                // somebody starts talking and the motion phase can animate the
                // alpha without adding anything. Drawn outside the avatar rather
                // than on its edge, and with enough segments to be a circle
                // rather than a polygon: at the sizes this runs at, ImGui's
                // automatic segment count showed the avatar's corners through
                // the ring.
                //
                // The stroke follows the radius (see ring_width_factor), floored
                // at a device pixel like every other stroke here.
                //
                // At rest the alpha is 1, not 0: ImGui culls a primitive whose
                // alpha is exactly zero before emitting a vertex
                // (imgui_draw.cpp:768), so a ring "drawn at alpha 0" is a ring
                // not drawn at all -- indistinguishable from the conditional
                // this replaced, and invisible to the geometry test, which
                // asserts one ring per picture and caught exactly that. One
                // part in 255 of green is beneath what antialiasing already
                // does to these edges; the animation therefore runs over the
                // other 254.
                Colour ring = theme.speaking_ring;
                ring.a = static_cast<uint8_t>(
                    1.0f + 254.0f * eased(motion.ring_phase) * row_alpha + 0.5f);
                const float nominal = radius * theme.ring_width_factor;
                const float ring_stroke = nominal < 1.0f ? 1.0f : nominal;
                draw_list->AddCircle(centre, radius + theme.ring_offset * scale,
                                     col(ring), 48, ring_stroke);
            }

            // Someone who cannot hear you, or cannot talk to you, is worth seeing
            // at a glance. The picture is dimmed and the state is named by an icon
            // on it -- dimming alone is ambiguous, and the icon alone gets lost
            // against a busy avatar.
            if ((muted || deafened) && config.show_muted_state) {
                draw_list->AddCircleFilled(centre, radius,
                                           col_scaled(theme.avatar_scrim, row_alpha), 32);
                const float offset = radius * theme.badge_offset_factor;
                draw_state_badge(draw_list, theme, ImVec2(centre.x + offset, centre.y + offset),
                                 radius * theme.badge_radius_factor, deafened, row_alpha);
            }

            // The picture occupies its own box and nothing more: the gap to the
            // name is ItemSpacing.x, which is the avatar_gap setting, so that
            // setting is the distance it says it is. There used to be a further
            // 10 units added here, which meant a gap of zero still left ten.
            ImGui::Dummy(ImVec2(picture, row));
            ImGui::SameLine();
            // Centred against the picture rather than sitting at the top of the
            // row. ImGui puts an item at the line's top, and with a large avatar
            // the name was visibly high against the face beside it.
            ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                                 (row - ImGui::GetTextLineHeight()) * 0.5f);

            // Who is talking, said twice: the ring around the picture, and the
            // name at full strength while everybody else's is greyed. The ring
            // alone is a thin circle a few pixels wide, read against whatever the
            // game happens to be drawing behind it, and in a channel of eight it
            // was work to find. Weight is easier to see than a stroke.
            //
            // A muted participant is dimmer still, which is what Discord does --
            // it dims them rather than colouring the name.
            // Deafened as well as muted: somebody who cannot hear is no more
            // present in the conversation than somebody who cannot speak, and the
            // picture already marks both.
            const bool dim = (muted || deafened) && config.show_muted_state;
            const Colour name_colour = dim        ? theme.text_muted
                                       : speaking ? theme.text_speaking
                                                  : theme.text_idle;
            // The joining fade rides on the alpha and on the outline's strength,
            // so the name and its ink arrive together.
            text_outlined(theme, user.name, col_scaled(name_colour, row_alpha),
                          outline * row_alpha, scale);
            ++drawn;
        }

        // Whoever did not fit, counted rather than dropped in silence: a list that
        // simply stops looks like an overlay that has lost track of the channel.
        if (truncated) {
            char remainder[32];
            std::snprintf(remainder, sizeof(remainder), "+%d more",
                          static_cast<int>(wanted_rows) - drawn);
            if (horizontal) {
                // At the end of the line, in line with the names it is about,
                // rather than under a picture that is not there: sideways there
                // is no column of pictures for it to align with.
                ImGui::SameLine(0.0f, cell_gap);
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() +
                                     (row - ImGui::GetTextLineHeight()) * 0.5f);
            } else {
                ImGui::Dummy(ImVec2(picture, 0.0f));
                ImGui::SameLine();
            }
            text_outlined(theme, remainder, col(theme.text_overflow), outline, scale);
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
    if (!config.notifications_enabled || snapshot.notification.serial == 0) {
        return;
    }

    const double age = now_seconds - snapshot.notification.received;
    if (age < 0.0 || age > config.notification_seconds) {
        return;
    }

    // In over a quarter of a second, out over three tenths: M3's entrance and
    // exit pair, through the one easing map (see the motion block) -- the
    // entrance decelerates, and the same map on the shrinking remainder is the
    // accelerating exit that replaced the linear ramp this used to be. Both are
    // functions of the notification's own timestamps, so the toast needs no
    // state of its own between frames.
    constexpr float kToastEntrySeconds = 0.25f;
    constexpr float kToastExitSeconds = 0.30f;
    const double remaining = config.notification_seconds - age;
    const float entry = eased(static_cast<float>(age) / kToastEntrySeconds);
    const float fade = entry * eased(static_cast<float>(remaining) / kToastExitSeconds);

    // The message has a size of its own on top of the shared one, so it can be
    // legible without the voice panel having to grow with it. That size applies to
    // the whole box -- text, picture, padding and corners -- and not only to its
    // width: a box that grew while its text stayed put is what the setting used to
    // do, and it is neither what the setting says nor what the preview showed.
    const float scale = ui_scale() * config.notification_scale;
    const Theme theme = theme_for(config);
    // The distance from the edge of the display belongs to the display, not to the
    // message, so it is not multiplied by the message's own size. It is the
    // message's *own* setting: the two boxes are placed independently and a person
    // moving one does not expect the other to follow, which is the mirror of
    // entry 17 -- there the two shared a distance that should have been one each.
    const float inset = config.notification_margin * ui_scale();
    // Wide enough to read, never wider than the screen it has to fit on: at a large
    // message size on a small output, 320 units came out wider than the display and
    // the box hung off both edges.
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
    // Its own setting, solid unless the user says otherwise. It used to be derived
    // from the panel's opacity, which meant a panel turned down to sit lightly over
    // the game took the message box with it -- and small text on a see-through box
    // over a bright scene cannot be read at all.
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
    // The vertical one is **not** `row_spacing`, and used to be. `row_spacing` is
    // the gap between one person and the next -- between two rows of a list -- and
    // the sender and the message are not two rows. They are two lines of one block,
    // and a line of text already carries its own leading inside its height. Adding
    // the between-people distance on top of that put a full line of empty space
    // between a name and what the person said: measured at 1080 lines, eleven pixels
    // of gap under a name whose letters are eleven pixels tall. Five of those eleven
    // are the leading and belong there; the other six were the panel's setting
    // arriving somewhere it was never about.
    //
    // Zero here therefore means "no gap beyond the one the type already has", not
    // "the lines touch" -- and it is the only value that does not change when
    // somebody drags the panel's row spacing for the panel's own sake.
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
        // multiplied by the fade at the point of use -- these go into the draw
        // list as raw colours, so the style alpha pushed above does not reach
        // them.
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

        // The text at the message's own size as well. The atlas is rasterised for
        // the voice panel, so a message made larger than it is magnified rather
        // than re-rasterised; a second atlas would cost a couple of megabytes in
        // every game process, which is not worth it for a box that appears for five
        // seconds. At the default size nothing is scaled at all.
        ImGui::SetWindowFontScale(config.notification_scale);

        const float avatar_radius = ImGui::GetTextLineHeight() * 0.9f;
        const float diameter = avatar_radius * 2.0f;

        // The picture is centred against the text beside it, and the text against
        // the picture, so whichever of the two is taller decides the box and the
        // shorter one sits in the middle of it. Drawn from the top, the picture of
        // a two-line message sat five units above the centre of the box it was in
        // -- measured, and visible once anybody looked for it.
        //
        // Both heights have to be known before either is drawn, which is why the
        // wrap width is worked out here rather than taken from the cursor after the
        // picture: ImGui lays out as it goes, and by then it is too late to move
        // anything up.
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
        // The same permanent outline as the panel's, from the same switch: one
        // treatment, one answer, whatever either box's opacity is set to. The
        // strength rides the fade -- the copies bypass the pushed style alpha,
        // so without this the entrance and the exit showed full-strength ink
        // around a fading glyph (the panel does the same with row_alpha).
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

// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What the overlay's geometry actually is, measured rather than derived.
//
// The panel had been judged from screenshots three times, and each pass fixed some
// numbers by eye and broke others. This runs the real build_panel() and
// build_notification() -- the same translation unit the layer and the interposer
// compile -- against a null backend, then reads the geometry back out of the
// vertices ImGui produced. Nothing here restates a formula from panel.cpp: a
// distance is measured between two things that were drawn, so a wrong formula
// shows up as a wrong number instead of agreeing with itself.
//
// Colours are the handle. Every shape the panel draws has a colour of its own, and
// the configurable ones are set to sentinels here, so the bounding box of every
// vertex carrying a given colour is that shape's rectangle. The window rectangles
// come from ImGui's own bookkeeping, which is the one thing worth trusting
// directly: it is what the backend would be told to scissor to.
//
// Output is JSON on stdout, so the comparison against the QML preview is a
// numeric one. With no arguments it runs a self-check instead, asserting the
// invariants that the drawing must satisfy at every extreme of every setting --
// which is the part that has to keep holding after this file has been forgotten.

#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_internal.h"
#include "vocem/config.h"
#include "vocem/fonts.h"
#include "vocem/panel.h"
#include "vocem/placement.h"
#include "vocem/shared_state.h"
#include "vocem/theme.h"

using namespace vocem;

namespace {

// Colours chosen so no two shapes can be confused, and so none of them collides
// with a colour panel.cpp uses for something else.
constexpr uint32_t kPanelSentinel = 0x010203;
constexpr uint32_t kSpeakingSentinel = 0x040506;
constexpr uint32_t kToastSentinel = 0x070809;

struct Rect {
    float x0 = FLT_MAX, y0 = FLT_MAX, x1 = -FLT_MAX, y1 = -FLT_MAX;
    bool valid() const { return x1 >= x0; }
    float width() const { return valid() ? x1 - x0 : 0.0f; }
    float height() const { return valid() ? y1 - y0 : 0.0f; }
    void add(float x, float y) {
        if (x < x0) x0 = x;
        if (y < y0) y0 = y;
        if (x > x1) x1 = x;
        if (y > y1) y1 = y;
    }
};

// Every vertex of this colour, whatever its alpha: opacity is a setting, and a box
// turned down to nothing still occupies the same rectangle.
Rect colour_bounds(const ImDrawList* list, ImU32 rgb) {
    Rect rect;
    if (!list) {
        return rect;
    }
    const ImU32 mask = IM_COL32(255, 255, 255, 0);
    for (int i = 0; i < list->VtxBuffer.Size; ++i) {
        const ImDrawVert& vertex = list->VtxBuffer[i];
        if ((vertex.col & mask) == (rgb & mask)) {
            rect.add(vertex.pos.x, vertex.pos.y);
        }
    }
    return rect;
}

// The same colour, once per shape. One participant's picture and the next one's
// are the same disc drawn twice, and ImGui emits each shape's vertices
// consecutively, so a break in the run of matching indices is a break between
// shapes. Splitting them by position instead would fail exactly where it matters:
// at an avatar size large enough for two rows to overlap.
std::vector<Rect> colour_clusters(const ImDrawList* list, ImU32 rgb) {
    std::vector<Rect> clusters;
    if (!list) {
        return clusters;
    }
    const ImU32 mask = IM_COL32(255, 255, 255, 0);
    int previous = -10;
    for (int i = 0; i < list->VtxBuffer.Size; ++i) {
        const ImDrawVert& vertex = list->VtxBuffer[i];
        if ((vertex.col & mask) != (rgb & mask)) {
            continue;
        }
        if (i != previous + 1 || clusters.empty()) {
            clusters.push_back(Rect());
        }
        clusters.back().add(vertex.pos.x, vertex.pos.y);
        previous = i;
    }
    return clusters;
}

// The surface that is drawn behind a piece of text, whatever shape it is in:
// the smallest cluster of the surface's own colour that contains the text's ink.
//
// One measurement for both of the panel's modes, deliberately. With the box
// around everything that cluster is the window's background; with the box behind
// the names it is that name's own pill -- so "where is the surface, relative to
// this name" is asked once and answered by the drawing rather than by a flag.
// The smallest, not the first: the window's background wears the same colour
// when it is drawn, and so does the rim around a state badge.
Rect surface_around(const ImDrawList* list, ImU32 rgb, const Rect& text) {
    Rect best;
    if (!text.valid()) {
        return best;
    }
    for (const Rect& candidate : colour_clusters(list, rgb)) {
        if (!candidate.valid() || candidate.x0 > text.x0 || candidate.x1 < text.x1 ||
            candidate.y0 > text.y0 || candidate.y1 < text.y1) {
            continue;
        }
        if (!best.valid() ||
            candidate.width() * candidate.height() < best.width() * best.height()) {
            best = candidate;
        }
    }
    return best;
}

Rect cluster(const ImDrawList* list, ImU32 rgb, size_t index) {
    const std::vector<Rect> clusters = colour_clusters(list, rgb);
    return index < clusters.size() ? clusters[index] : Rect();
}

// The strongest alpha any vertex of this colour carries, or -1 when the colour
// is absent. Alpha is where a fade lives (motion never moves geometry), so this
// is the instrument for asking whether a shape actually rode one.
int max_alpha(const ImDrawList* list, ImU32 rgb) {
    int alpha = -1;
    if (!list) {
        return alpha;
    }
    const ImU32 mask = IM_COL32(255, 255, 255, 0);
    for (int i = 0; i < list->VtxBuffer.Size; ++i) {
        const ImDrawVert& vertex = list->VtxBuffer[i];
        if ((vertex.col & mask) == (rgb & mask)) {
            const int a = (vertex.col >> IM_COL32_A_SHIFT) & 0xff;
            if (a > alpha) {
                alpha = a;
            }
        }
    }
    return alpha;
}

// The strongest alpha of each shape drawn in this colour, in drawing order:
// colour_clusters' split, max_alpha's measurement. One participant's picture and
// the next one's are the same disc, and what tells them apart is how strongly
// each was drawn.
std::vector<int> cluster_alphas(const ImDrawList* list, ImU32 rgb) {
    std::vector<int> alphas;
    if (!list) {
        return alphas;
    }
    const ImU32 mask = IM_COL32(255, 255, 255, 0);
    int previous = -10;
    for (int i = 0; i < list->VtxBuffer.Size; ++i) {
        const ImDrawVert& vertex = list->VtxBuffer[i];
        if ((vertex.col & mask) != (rgb & mask)) {
            continue;
        }
        if (i != previous + 1 || alphas.empty()) {
            alphas.push_back(0);
        }
        const int a = (vertex.col >> IM_COL32_A_SHIFT) & 0xff;
        if (a > alphas.back()) {
            alphas.back() = a;
        }
        previous = i;
    }
    return alphas;
}

ImU32 to_rgb(uint32_t colour) {
    return IM_COL32((colour >> 16) & 0xff, (colour >> 8) & 0xff, colour & 0xff, 255);
}

// A theme colour in the form the searches above compare against. The alpha is
// dropped because they mask it off anyway: a box turned down to nothing still
// occupies the same rectangle.
ImU32 ink(Colour colour) {
    return IM_COL32(colour.r, colour.g, colour.b, 255);
}

// The same four people, in the same channel, that the configuration window draws, in the same states and with
// the same message. Two geometries can only be compared if they are drawing the
// same thing, and the window's roster -- fixed, and never the live channel -- is
// what a comparison can be made against with Discord closed. It is written out in
// ConfigBridge::participants(); this is the other half of it.
Snapshot make_snapshot(uint32_t users, const char* channel) {
    static const struct {
        const char* name;
        uint32_t flags;
    } roster[] = {
        {"User 1", kFlagSpeaking},
        {"User 2", 0},
        {"User 3", kFlagMuted},
        {"User 4", kFlagDeafened},
    };

    Snapshot snapshot;
    snapshot.connected = true;
    snapshot.in_channel = true;
    std::snprintf(snapshot.channel_name, sizeof(snapshot.channel_name), "%s", channel);
    snapshot.user_count = users > kMaxUsers ? kMaxUsers : users;
    for (uint32_t i = 0; i < snapshot.user_count; ++i) {
        snapshot.users[i].id = 1000 + i;
        if (i < IM_ARRAYSIZE(roster)) {
            std::snprintf(snapshot.users[i].name, sizeof(snapshot.users[i].name), "%s",
                          roster[i].name);
            snapshot.users[i].flags = roster[i].flags;
        } else {
            std::snprintf(snapshot.users[i].name, sizeof(snapshot.users[i].name), "Participant %u",
                          i + 1);
            // Beyond the four the window knows about, one of each state, so the
            // decorations are exercised at every row count.
            if (i % 4 == 3) snapshot.users[i].flags |= kFlagDeafened;
        }
    }
    snapshot.notification.serial = 1;
    snapshot.notification.user_id = 1000;
    snapshot.notification.received = 0.0;
    std::snprintf(snapshot.notification.title, sizeof(snapshot.notification.title), "User 1");
    std::snprintf(snapshot.notification.body, sizeof(snapshot.notification.body),
                  "sent you a direct message");
    return snapshot;
}

// What one configuration draws, in pixels of the output it was drawn for.
struct Measurement {
    float output_width = 0.0f;
    float output_height = 0.0f;

    // What ImGui was told to use, reported so a divergence can be traced to the
    // input rather than to the layout.
    float font_pixels = 0.0f;
    float ui_scale = 0.0f;
    // The unit the message box is laid out in: ui_scale times its own size setting.
    float message_scale = 0.0f;
    float line_height = 0.0f;
    float line_height_with_spacing = 0.0f;
    float item_spacing_y = 0.0f;
    float item_spacing_x = 0.0f;
    float window_padding_x = 0.0f;
    float window_padding_y = 0.0f;
    // The width of a known string at the reference size, which is what lets the
    // QML preview pick a font size that matches this one rather than a nominally
    // equal one that measures differently.
    float reference_text_width = 0.0f;
    float reference_em = 0.0f;
    // The advance width of the strings the preview shows, which is what the two
    // sides have to agree on before any box that ends where its text ends can.
    float name_advance = 0.0f;
    float channel_advance = 0.0f;
    float title_advance = 0.0f;
    float body_advance = 0.0f;

    Rect panel;         // the box, from its background
    Rect avatar;        // the first participant's picture
    Rect ring;          // the speaking ring around it
    Rect badge;         // the muted badge on the second participant
    Rect channel_text;  // the channel name's ink
    Rect first_name;    // the first participant's name
    Rect second_name;   // the second participant's, who is the idle one
    // The surface each of those two sits on: the window's background where the
    // panel draws a box around everything, that name's own pill where it draws
    // one behind the names instead.
    Rect first_surface;
    Rect second_surface;
    Rect channel_surface;
    Rect second_avatar;
    Rect separator;
    Rect hairline;      // the stroke just inside the panel's edge
    Rect separator_accent;  // the blurple bar in front of the separator's hairline
    Rect overflow;      // the "+N more" line's ink, when anybody was left out

    // The ring is drawn around every picture now, transparent at rest, so the
    // two counts have to agree -- a ring that appears only for the speaker is
    // the reflow this design got rid of.
    size_t ring_count = 0;
    size_t avatar_count = 0;

    Rect toast;
    Rect toast_avatar;
    Rect toast_title;
    Rect toast_body;
    Rect toast_accent;    // the blurple bar down the box's left edge
    Rect toast_hairline;  // the stroke just inside the box's edge

    // What the message box's padding and spacing should be: the shared setting at
    // the message's own size. The one figure here that is a contract rather than a
    // measurement, because it is the contract being checked.
    float toast_padding_x = 0.0f;
    float toast_padding_y = 0.0f;

    float panel_two_rows_height = 0.0f;  // for the row pitch, measured as a difference
    float panel_one_row_height = 0.0f;
    float panel_no_channel_height = 0.0f;
};

// Several frames with a null backend, and only the last is read. Several for two
// reasons now: ImGui hides an auto-resized window until it knows its size, and
// the panel's animations have to settle -- this measures the resting geometry,
// which the motion contract promises is what an ended animation leaves behind.
//
// The clock is monotone across every call and jumps ten seconds a frame: the
// drawing clamps a frame's animation step to a tenth of a second, so six frames
// advance every phase by 0.6 animated seconds, past the longest duration there
// is, whatever state the persistent motion slots were left in by the previous
// configuration measured. The toast's timestamp is restamped each frame so the
// box is measured mid-life -- past its entrance, before its fade.
//
// One clock for every caller of build_panel in this file, at namespace scope
// rather than inside this function: the drawing keeps its own last timestamp and
// the moment each participant fell quiet, so a check stepping a second clock of
// its own would hand the next run_frames a time in the past -- a step of zero, a
// ring hold that never ends, and a settled geometry that is not settled.
double g_clock = 100.0;

void run_frames(const Snapshot& snapshot, const Config& config, uint32_t width, uint32_t height,
                bool with_toast) {
    Snapshot animated = snapshot;
    for (int frame = 0; frame < 6; ++frame) {
        g_clock += 10.0;
        animated.notification.received = g_clock - 1.0;
        ImGui::GetIO().DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        build_panel(animated, config, width, height, nullptr, g_clock);
        if (with_toast) {
            build_notification(animated, config, width, height, nullptr, g_clock);
        }
        ImGui::Render();
    }
}

// One frame of the panel alone, `step` seconds after the last one on the shared
// clock, for the checks that watch an animation move rather than the geometry it
// leaves behind. The panel's draw list is what comes back.
const ImDrawList* panel_frame(const Snapshot& snapshot, const Config& config, double step) {
    g_clock += step;
    ImGui::GetIO().DisplaySize = ImVec2(1920.0f, 1080.0f);
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    build_panel(snapshot, config, 1920, 1080, nullptr, g_clock);
    ImGui::Render();
    ImGuiWindow* panel = ImGui::FindWindowByName("##vocem");
    return panel ? panel->DrawList : nullptr;
}

float window_height(const char* name) {
    ImGuiWindow* window = ImGui::FindWindowByName(name);
    return window ? window->Size.y : 0.0f;
}

Measurement measure(const Config& base, uint32_t width, uint32_t height, uint32_t users) {
    Config config = base;
    config.panel_colour = kPanelSentinel;
    config.speaking_colour = kSpeakingSentinel;
    config.notification_colour = kToastSentinel;

    Measurement out;
    out.output_width = static_cast<float>(width);
    out.output_height = static_cast<float>(height);

    const float pixels = font_pixel_size(height, config.scale, config.font_size);
    ensure_fonts(pixels, config.font_size, config.font_path.c_str(),
                 config.font_path_strong.c_str());
    configure_style(config);

    const Snapshot snapshot = make_snapshot(users, "Voice channel");
    run_frames(snapshot, config, width, height, true);

    const ImGuiStyle& style = ImGui::GetStyle();
    out.font_pixels = pixels;
    out.ui_scale = ui_scale();
    out.item_spacing_x = style.ItemSpacing.x;
    out.item_spacing_y = style.ItemSpacing.y;
    out.window_padding_x = style.WindowPadding.x;
    out.window_padding_y = style.WindowPadding.y;
    out.message_scale = out.ui_scale * config.notification_scale;
    out.toast_padding_x = config.box_padding_x * out.ui_scale * config.notification_scale;
    out.toast_padding_y = config.box_padding_y * out.ui_scale * config.notification_scale;

    ImGuiWindow* panel = ImGui::FindWindowByName("##vocem");
    ImGuiWindow* toast = ImGui::FindWindowByName("##vocem_toast");
    if (panel) {
        out.line_height = panel->CalcFontSize();
        out.panel.add(panel->Pos.x, panel->Pos.y);
        out.panel.add(panel->Pos.x + panel->Size.x, panel->Pos.y + panel->Size.y);
    }
    out.line_height_with_spacing = out.line_height + out.item_spacing_y;

    // The reference string is measured with the body font at the size the atlas was
    // built at, which is the number Qt has to be made to agree with.
    if (fonts().body) {
        const ImVec2 size = fonts().body->CalcTextSizeA(pixels, FLT_MAX, 0.0f,
                                                        "Participant 1 gjqQWM");
        out.reference_text_width = size.x;
        out.reference_em = fonts().body->FontSize;
        out.name_advance = fonts().body->CalcTextSizeA(pixels, FLT_MAX, 0.0f, "User 1").x;
        out.body_advance =
            fonts().body->CalcTextSizeA(pixels, FLT_MAX, 0.0f, "sent you a direct message").x;
    }
    if (fonts().strong) {
        out.channel_advance = fonts().strong->CalcTextSizeA(pixels, FLT_MAX, 0.0f, "Voice channel").x;
        out.title_advance = fonts().strong->CalcTextSizeA(pixels, FLT_MAX, 0.0f, "Someone").x;
    }

    const ImDrawList* panel_list = panel ? panel->DrawList : nullptr;
    const ImDrawList* toast_list = toast ? toast->DrawList : nullptr;

    // The colours come from the theme, not from a copy of it. A shape is found here
    // by looking for its colour, so a palette written out a second time in this file
    // would go on measuring whatever the panel used to draw: the search would simply
    // find nothing, and an empty rectangle reads as a shape at the origin rather
    // than as a test that has stopped looking at the right thing.
    const Theme theme = theme_for(config);
    out.avatar = cluster(panel_list, ink(theme.avatar_placeholder), 0);
    out.second_avatar = cluster(panel_list, ink(theme.avatar_placeholder), 1);
    // The first ring is the speaker's -- the fixture's first participant -- and
    // a cluster now rather than the bounds of the colour, because every picture
    // wears a ring (transparent when its owner is quiet) and the union of all of
    // them would just be the rows.
    out.ring = cluster(panel_list, to_rgb(kSpeakingSentinel), 0);
    out.ring_count = colour_clusters(panel_list, to_rgb(kSpeakingSentinel)).size();
    out.avatar_count = colour_clusters(panel_list, ink(theme.avatar_placeholder)).size();
    out.badge = cluster(panel_list, ink(theme.badge_fill), 0);
    out.separator_accent = colour_bounds(panel_list, ink(theme.separator_accent));
    out.overflow = cluster(panel_list, ink(theme.text_overflow), 0);
    // The channel name is the first thing drawn in its colour; a muted
    // participant's name shares it, and comes later.
    out.channel_text =
        config.show_channel_name ? cluster(panel_list, ink(theme.text_channel), 0) : Rect();
    out.first_name = cluster(panel_list, ink(theme.text_speaking), 0);
    // The fixture's second participant is the quiet one, so the idle grey's
    // first cluster in this list is their name.
    out.second_name = cluster(panel_list, ink(theme.text_idle), 0);
    out.first_surface = surface_around(panel_list, to_rgb(kPanelSentinel), out.first_name);
    out.second_surface = surface_around(panel_list, to_rgb(kPanelSentinel), out.second_name);
    out.channel_surface = surface_around(panel_list, to_rgb(kPanelSentinel), out.channel_text);
    out.separator = colour_bounds(panel_list, ink(theme.separator));
    out.hairline = colour_bounds(panel_list, ink(theme.panel_hairline));

    if (toast) {
        out.toast.add(toast->Pos.x, toast->Pos.y);
        out.toast.add(toast->Pos.x + toast->Size.x, toast->Pos.y + toast->Size.y);
    }
    out.toast_avatar = cluster(toast_list, ink(theme.avatar_placeholder), 0);
    out.toast_title = cluster(toast_list, ink(theme.toast_title), 0);
    out.toast_body = cluster(toast_list, ink(theme.toast_body), 0);
    out.toast_accent = colour_bounds(toast_list, ink(theme.toast_accent));
    out.toast_hairline = colour_bounds(toast_list, ink(theme.toast_hairline));

    // Heights of the same panel with one row fewer, which is how the row pitch is
    // obtained without asking the drawing code what it thinks the pitch is.
    Config quiet = config;
    quiet.notifications_enabled = false;
    run_frames(make_snapshot(1, "Voice channel"), quiet, width, height, false);
    out.panel_one_row_height = window_height("##vocem");
    run_frames(make_snapshot(2, "Voice channel"), quiet, width, height, false);
    out.panel_two_rows_height = window_height("##vocem");
    Config no_channel = quiet;
    no_channel.show_channel_name = false;
    run_frames(make_snapshot(1, "Voice channel"), no_channel, width, height, false);
    out.panel_no_channel_height = window_height("##vocem");

    // Left as it was found, so the caller's next measurement starts from the same
    // place this one did.
    run_frames(snapshot, config, width, height, true);
    return out;
}

void print_rect(const char* name, const Rect& rect, bool last = false) {
    if (!rect.valid()) {
        std::printf("    \"%s\": null%s\n", name, last ? "" : ",");
        return;
    }
    std::printf("    \"%s\": {\"x\": %.3f, \"y\": %.3f, \"w\": %.3f, \"h\": %.3f}%s\n", name,
                rect.x0, rect.y0, rect.width(), rect.height(), last ? "" : ",");
}

void print_json(const Measurement& m) {
    std::printf("{\n");
    std::printf("    \"output\": {\"w\": %.1f, \"h\": %.1f},\n", m.output_width, m.output_height);
    std::printf("    \"font_pixels\": %.4f,\n", m.font_pixels);
    std::printf("    \"ui_scale\": %.4f,\n", m.ui_scale);
    std::printf("    \"message_scale\": %.4f,\n", m.message_scale);
    std::printf("    \"line_height\": %.4f,\n", m.line_height);
    std::printf("    \"line_height_with_spacing\": %.4f,\n", m.line_height_with_spacing);
    std::printf("    \"item_spacing\": {\"x\": %.4f, \"y\": %.4f},\n", m.item_spacing_x,
                m.item_spacing_y);
    std::printf("    \"window_padding\": {\"x\": %.4f, \"y\": %.4f},\n", m.window_padding_x,
                m.window_padding_y);
    std::printf("    \"reference_text_width\": %.4f,\n", m.reference_text_width);
    std::printf("    \"reference_em\": %.4f,\n", m.reference_em);
    std::printf("    \"name_advance\": %.4f,\n", m.name_advance);
    std::printf("    \"channel_advance\": %.4f,\n", m.channel_advance);
    std::printf("    \"title_advance\": %.4f,\n", m.title_advance);
    std::printf("    \"body_advance\": %.4f,\n", m.body_advance);
    std::printf("    \"panel_one_row_height\": %.4f,\n", m.panel_one_row_height);
    std::printf("    \"panel_two_rows_height\": %.4f,\n", m.panel_two_rows_height);
    std::printf("    \"panel_no_channel_height\": %.4f,\n", m.panel_no_channel_height);
    print_rect("panel", m.panel);
    print_rect("avatar", m.avatar);
    print_rect("second_avatar", m.second_avatar);
    print_rect("ring", m.ring);
    print_rect("badge", m.badge);
    print_rect("channel_text", m.channel_text);
    print_rect("first_name", m.first_name);
    print_rect("second_name", m.second_name);
    print_rect("first_surface", m.first_surface);
    print_rect("second_surface", m.second_surface);
    print_rect("channel_surface", m.channel_surface);
    print_rect("separator", m.separator);
    print_rect("separator_accent", m.separator_accent);
    print_rect("overflow", m.overflow);
    print_rect("hairline", m.hairline);
    print_rect("toast", m.toast);
    print_rect("toast_avatar", m.toast_avatar);
    print_rect("toast_title", m.toast_title);
    print_rect("toast_accent", m.toast_accent);
    print_rect("toast_hairline", m.toast_hairline);
    print_rect("toast_body", m.toast_body, true);
    std::printf("}\n");
}

// --- the self-check ---------------------------------------------------------

int failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("  FAIL  %s\n", what.c_str());
        ++failures;
    }
}

void check_close(float measured, float expected, float tolerance, const std::string& what) {
    if (std::fabs(measured - expected) > tolerance) {
        std::printf("  FAIL  %s: measured %.3f, expected %.3f\n", what.c_str(), measured, expected);
        ++failures;
    }
}

std::string describe(const Config& config, uint32_t width, uint32_t height, uint32_t users) {
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  "%s/%s %ux%u scale %.2f opacity %.2f avatar %.2f margin %.0f padding %.0f/%.0f "
                  "gap %.0f/%.0f channel %d users %u",
                  Config::layout_text(config.panel_layout), Config::box_text(config.panel_box),
                  width, height, config.scale,
                  config.opacity, config.avatar_size, config.screen_margin, config.box_padding_x,
                  config.box_padding_y, config.avatar_gap, config.row_spacing,
                  config.show_channel_name ? 1 : 0, users);
    return buffer;
}

// The invariants. Everything here is a property of the drawing that has to hold at
// every setting, not a restatement of a formula: a box inside the screen, its
// contents inside it, one row clear of the next.
void verify(const Config& config, uint32_t width, uint32_t height, uint32_t users) {
    const Measurement m = measure(config, width, height, users);
    const std::string where = describe(config, width, height, users);
    const float slack = 0.75f;  // antialiasing puts a vertex half a pixel outside
    // ImGui truncates a window's content origin to whole pixels, so a padding with
    // a fraction in it lands up to a pixel short of the figure it was given.
    const float truncation = 1.25f;

    // The panel is expected to fit whatever the settings are: it draws as many
    // people as the display has room for and says how many are missing, so there is
    // no configuration in which it runs off the bottom.
    //
    // The message box is different. It is one box with one message in it, and at
    // three times the size on a small output a long message is simply taller than
    // the screen; the alternatives are drawing it at a size the user did not ask
    // for or cutting the message in half, and neither is better than a box that
    // overflows. Its containment is therefore checked where it can fit.
    // The one case the panel cannot answer for is a display too small for its own
    // frame: at 120 units of margin and 48 of padding on a 720p screen, the margins
    // and the padding alone are more than the screen is tall, before a single name
    // is drawn. The height of the smallest panel there is -- one person -- is the
    // measure of that, and it is measured rather than worked out.
    const float margin = config.screen_margin * m.ui_scale;
    const bool panel_fits = m.panel_one_row_height + margin * 2.0f <= m.output_height + 1.0f;
    const bool toast_fits = m.toast.valid() && m.toast.height() + margin * 2.0f <= m.output_height &&
                            m.toast.width() + margin * 2.0f <= m.output_width;

    check(m.panel.valid(), where + ": the panel is drawn");
    if (panel_fits) {
        check(m.panel.x0 >= -slack && m.panel.y0 >= -slack &&
                  m.panel.x1 <= m.output_width + slack && m.panel.y1 <= m.output_height + slack,
              where + ": the panel is on screen");
    } else {
        // Nothing else is possible, but the top-left corner is what a user reads
        // first, so that is the part that has to survive.
        check(m.panel.x0 >= -slack && m.panel.y0 >= -slack,
              where + ": the panel starts on screen even where it cannot fit");
    }
    if (toast_fits) {
        check(m.toast.x0 >= -slack && m.toast.y0 >= -slack &&
                  m.toast.x1 <= m.output_width + slack && m.toast.y1 <= m.output_height + slack,
              where + ": the message is on screen");
    }

    // Each box keeps its own distance from the edge -- the panel's `screen_margin`
    // and the message's `notification_margin`. They were one setting for both once,
    // which is entry 17 in reverse: there a hardcoded 12 ignored the slider, here a
    // shared slider moved a box its owner had not asked to move.
    const float inset = config.screen_margin * m.ui_scale;
    const float toast_inset = config.notification_margin * m.ui_scale;
    // A fraction is a place between the margins, so only the (0,0) anchor puts the
    // panel *at* the margin on both axes (vocem/placement.h).
    if (config.position_x <= 0.0f && config.position_y <= 0.0f) {
        check_close(m.panel.x0, inset, 0.75f, where + ": the panel sits at the margin");
        check_close(m.panel.y0, inset, 0.75f, where + ": the panel sits at the margin, vertically");
    }
    // And the middle of a side means centred, which is the whole of the snap fix:
    // the box's own centre lands on half the display, not its top edge.
    if (config.position_y == 0.5f && m.panel.valid()) {
        const float centre = (m.panel.y0 + m.panel.y1) * 0.5f;
        const float half = m.output_height * 0.5f;
        const bool fits = (m.panel.y1 - m.panel.y0) + inset * 2.0f <= m.output_height;
        if (fits) {
            check_close(centre, half, 1.0f,
                        where + ": a middle anchor centres the panel on that side");
        }
    }
    check_close(m.output_width - m.toast.x1, toast_inset, 0.75f,
                where + ": the message sits at its own margin");

    // Nothing the panel draws may cross its own padding.
    if (m.avatar.valid() && panel_fits) {
        check(m.avatar.x0 >= m.panel.x0 + m.window_padding_x - truncation,
              where + ": the picture starts after the left padding");
        check(m.avatar.y1 <= m.panel.y1 - m.window_padding_y + slack,
              where + ": the picture stays above the bottom padding");
    }
    if (m.ring.valid() && panel_fits) {
        check(m.ring.x0 >= m.panel.x0 - slack && m.ring.x1 <= m.panel.x1 + slack &&
                  m.ring.y0 >= m.panel.y0 - slack && m.ring.y1 <= m.panel.y1 + slack,
              where + ": the speaking ring stays inside the box");
    }
    if (m.badge.valid() && panel_fits) {
        check(m.badge.x1 <= m.panel.x1 + slack && m.badge.y1 <= m.panel.y1 + slack,
              where + ": the muted badge stays inside the box");
    }
    if (m.first_name.valid() && panel_fits) {
        check(m.first_name.x1 <= m.panel.x1 - m.window_padding_x + slack + 2.0f,
              where + ": the name stays inside the right padding");
    }
    if (m.channel_text.valid() && panel_fits) {
        check(m.channel_text.x1 <= m.panel.x1 - m.window_padding_x + slack + 2.0f,
              where + ": the channel name stays inside the right padding");
    }

    // One person clear of the next: the pictures must not touch, whatever the
    // avatar size does to them -- in the direction that layout advances in.
    // Sideways they share a line, which is the whole of that layout, so the same
    // property is about x there and asserting y would assert the opposite.
    const bool horizontal = config.panel_layout == Config::kLayoutHorizontal;
    if (users >= 2 && m.avatar.valid() && m.second_avatar.valid() && panel_fits) {
        if (horizontal) {
            check(m.second_avatar.x0 >= m.avatar.x1 - slack,
                  where + ": consecutive pictures do not overlap");
            check_close(m.second_avatar.y0, m.avatar.y0, 1.0f,
                        where + ": the people share one line");
        } else {
            check(m.second_avatar.y0 >= m.avatar.y1 - slack,
                  where + ": consecutive pictures do not overlap");
        }
    }

    // Every picture wears its ring, transparent or not: a ring drawn only for
    // the speaker is a shape that appears, which is the reflow this design got
    // rid of -- and the alpha is the one thing this measurement cannot see, so
    // the count is what stands in for "always".
    check(m.ring_count == m.avatar_count,
          where + ": every picture wears a ring (" + std::to_string(m.ring_count) + " rings, " +
              std::to_string(m.avatar_count) + " pictures)");
    // The equality above is satisfied by two empty sets: both counts come from
    // colour clustering, so one theme-token change could retire the whole
    // family while the check went on passing on 0 == 0.
    check(m.avatar_count >= 1, where + ": and there is at least one picture");

    // The line under the channel name: the accent bar starts where the content
    // does, the hairline takes over exactly where the bar ends, and the two
    // share their row of pixels.
    if (config.show_channel_name && m.separator_accent.valid()) {
        check_close(m.separator_accent.x0, m.panel.x0 + m.window_padding_x, truncation,
                    where + ": the accent bar starts at the content's left edge");
        if (m.separator.valid()) {
            check_close(m.separator.x0, m.separator_accent.x1, 1.0f,
                        where + ": the hairline takes over where the accent bar ends");
            check_close(m.separator.y0, m.separator_accent.y0, 0.01f,
                        where + ": the two segments share their row");
        }
    }

    // Whoever did not fit is a remark about the list, aligned with the names in
    // it: upright, to the column the names start in, never to the pictures;
    // sideways, to the line they sit on, at the end of it.
    if (m.overflow.valid() && m.first_name.valid()) {
        if (horizontal) {
            check(m.overflow.x0 >= m.first_name.x1 - slack,
                  where + ": the overflow remark ends the line");
            check_close((m.overflow.y0 + m.overflow.y1) * 0.5f,
                        (m.first_name.y0 + m.first_name.y1) * 0.5f, 1.5f,
                        where + ": the overflow remark sits on the line of names");
        } else {
            check_close(m.overflow.x0, m.first_name.x0, 1.0f,
                        where + ": the overflow line is aligned with the names");
        }
    }

    // Where the surface actually is. Two modes, one measurement (surface_around):
    // with the box around everything the surface behind a name IS the window's
    // background, and with the box behind the names it is a pill that hugs that
    // name and nothing else. Both are asserted against the drawing, so a mode
    // that quietly drew the other one fails here rather than in a screenshot.
    const bool names_box = config.panel_box == Config::kBoxNames;
    if (config.opacity > 0.0f && m.first_name.valid() && panel_fits) {
        check(m.first_surface.valid(), where + ": the first name is drawn on a surface");
    }
    if (m.first_surface.valid() && panel_fits) {
        if (names_box) {
            // Strictly inside the box's own rectangle, which is what clips it:
            // the pill's padding is claimed from the layout precisely so that
            // the window it is drawn into does not cut its edges off, and at
            // box_padding 0 -- measured below -- nothing else would stop it.
            check(m.first_surface.x0 >= m.panel.x0 - slack &&
                      m.first_surface.x1 <= m.panel.x1 + slack &&
                      m.first_surface.y0 >= m.panel.y0 - slack &&
                      m.first_surface.y1 <= m.panel.y1 + slack,
                  where + ": the name's box stays inside the panel's own rectangle");
            // A box behind the names is a box behind the names: it must not be
            // the whole panel, which is what a mode that changed nothing would
            // measure as.
            check(m.first_surface.width() < m.panel.width() - 1.0f ||
                      m.first_surface.height() < m.panel.height() - 1.0f,
                  where + ": the name's box is smaller than the panel");
            // It reaches past the glyphs on every side -- a box that ended at
            // the ink would read as a stripe -- and the name is inside it,
            // which surface_around already had to find to return it.
            check(m.first_surface.height() > m.first_name.height(),
                  where + ": the name's box is taller than the name");
            check(m.first_surface.width() > m.first_name.width(),
                  where + ": the name's box is wider than the name");
        } else {
            // The other mode says the opposite, and says it as a measurement
            // rather than by not looking: the surface behind the name is the
            // box, not something drawn behind the text.
            check_close(m.first_surface.width(), m.panel.width(), 1.5f,
                        where + ": the box behind the name is the panel's own box");
            check_close(m.first_surface.height(), m.panel.height(), 1.5f,
                        where + ": the box behind the name is the panel's own box, in height");
        }
    }
    // One person's box clear of the next's, in the direction the layout advances:
    // two translucent pills that overlapped would composite into a darker band
    // where they met, and a band nobody chose between two rows is exactly what a
    // pill drawn taller than the row it sits in would produce.
    //
    // The tolerance is a whole pixel and not the usual half, because two fills
    // meet here and each carries its own antialiasing fringe: at the row spacing
    // 0 with a small avatar the rows are exactly a box apart, which is the case
    // this is about, and the measured overlap there is 1.00 -- the two fringes
    // and nothing else. Against a pill that ignored the row (line + padding
    // drawn into a row of the bare line) the overlap is the padding, four times
    // that at the default text size.
    if (names_box && users >= 2 && m.first_surface.valid() && m.second_surface.valid() &&
        panel_fits) {
        const float fringe = 1.05f;
        if (horizontal) {
            check(m.second_surface.x0 >= m.first_surface.x1 - fringe,
                  where + ": consecutive name boxes do not overlap");
        } else {
            check(m.second_surface.y0 >= m.first_surface.y1 - fringe,
                  where + ": consecutive name boxes do not overlap");
        }
    }
    // And the channel name gets the same treatment as a participant's: it is a
    // line of text over the game like any other, and a box that carried the
    // people but not the room they are in would be two answers to one question.
    if (names_box && config.show_channel_name && config.opacity > 0.0f && panel_fits &&
        m.channel_text.valid()) {
        check(m.channel_surface.valid(), where + ": the channel name has a box behind it too");
        if (m.channel_surface.valid()) {
            check(m.channel_surface.y0 >= m.panel.y0 - slack,
                  where + ": and it stays inside the panel's own rectangle");
        }
    }

    // The hairline follows the box's opacity through its premultiplied alpha, so
    // it exists exactly when the box does: a panel faded to nothing must not
    // leave a floating outline.
    const Theme theme = theme_for(config);
    if (theme.panel_hairline.a > 0) {
        check(m.hairline.valid(), where + ": the hairline is drawn");
        // Just inside the edge: the stroke's own bounds may not leave the box.
        check(m.hairline.x0 >= m.panel.x0 - slack && m.hairline.y0 >= m.panel.y0 - slack &&
                  m.hairline.x1 <= m.panel.x1 + slack && m.hairline.y1 <= m.panel.y1 + slack,
              where + ": the hairline stays inside the box");
    } else {
        check(!m.hairline.valid(), where + ": no box, no hairline");
    }

    // The message box: its contents inside it, on both axes.
    if (m.toast_avatar.valid()) {
        check(m.toast_avatar.x0 >= m.toast.x0 + m.toast_padding_x - truncation,
              where + ": the message picture starts after the left padding");
        check(m.toast_avatar.y0 >= m.toast.y0 - slack,
              where + ": the message picture starts inside the box");
    }
    // The text is not optional. The fixture always publishes a title and a body,
    // so a toast that is drawn and fits must carry both -- these used to be
    // guarded on their own validity, which meant a toast reduced to a textless
    // box skipped every text check instead of failing one (the gl_toast_alone
    // pixel count has the same shape of hole, closed the same day).
    if (toast_fits && config.notification_opacity > 0.0f) {
        check(m.toast_title.valid(), where + ": the sender's name is drawn in the toast");
        check(m.toast_body.valid(), where + ": the message text is drawn in the toast");
    }
    if (m.toast_body.valid() && toast_fits) {
        check(m.toast_body.x1 <= m.toast.x1 - m.toast_padding_x + slack + 2.0f,
              where + ": the message text stays inside the right padding");
        check(m.toast_body.y1 <= m.toast.y1 + slack,
              where + ": the message text stays inside the box");
    }
    if (m.toast_title.valid() && m.toast_body.valid() && toast_fits) {
        check(m.toast_title.y1 <= m.toast_body.y0 + slack,
              where + ": the sender and the message do not overlap");
    }

    // The toast's edge treatments, which exist exactly when its box does -- the
    // accent bar is part of the box and premultiplied with its opacity, so a
    // message turned down to nothing must not leave a blurple bar floating.
    if (m.toast.valid()) {
        if (theme.toast_accent.a > 0) {
            const float rounding = theme.box_radius * m.message_scale;
            check(m.toast_accent.valid(), where + ": the accent bar is drawn");
            check_close(m.toast_accent.x0, m.toast.x0, slack,
                        where + ": the accent bar sits on the box's left edge");
            check_close(m.toast.y0 + rounding, m.toast_accent.y0, 1.0f,
                        where + ": the accent bar starts where the corner curvature ends");
            check_close(m.toast.y1 - rounding, m.toast_accent.y1, 1.0f,
                        where + ": the accent bar stops where the curvature starts again");
        } else {
            check(!m.toast_accent.valid(), where + ": no box, no accent bar");
        }
        if (theme.toast_hairline.a > 0) {
            check(m.toast_hairline.valid(), where + ": the toast hairline is drawn");
            check(m.toast_hairline.x0 >= m.toast.x0 - slack &&
                      m.toast_hairline.y0 >= m.toast.y0 - slack &&
                      m.toast_hairline.x1 <= m.toast.x1 + slack &&
                      m.toast_hairline.y1 <= m.toast.y1 + slack,
                  where + ": the toast hairline stays inside the box");
        } else {
            check(!m.toast_hairline.valid(), where + ": no box, no toast hairline");
        }
    }
}

// Every shape above is found by its colour, which only works while no two shapes
// drawn into the same list share one. That is a property of the palette rather than
// of the drawing, and it is not obvious from looking at the theme: two roles can be
// given the same grey by somebody who has no reason to think the two are related.
//
// When it breaks, nothing fails. `cluster()` returns the first run of vertices in
// that colour, which is now some other shape's, so a figure comes out plausible and
// wrong -- and the QML comparison then agrees with it, because both sides are
// measuring the same confusion. So it is asserted here, once, for every palette the
// theme can produce.
void distinct_tokens() {
    // The roles the measurement searches for, per draw list. Two lists may share a
    // colour: the panel and the message box are separate windows with separate
    // vertex buffers, and `channel_text` and `toast_body` have always been the same
    // value without ever being confused.
    struct Named {
        const char* name;
        Colour colour;
    };

    // Every palette the theme can produce: the five surfaces, and the pinned text
    // colours aimed exactly where they would collide -- the speaking name on the
    // channel's white, the idle grey on the overflow's, both pinned to one value,
    // the message body on the title's white, an idle name on the hairline. Each of
    // these is a value a user can type into the colour dialog, and each one made
    // two measured roles identical until theme_for() learnt to step a pinned
    // colour one part in 255 off whatever already wears it.
    struct Pinned {
        const char* label;
        uint32_t idle;
        uint32_t speaking;
        uint32_t body;
    };
    constexpr uint32_t kAuto = Config::kColourAuto;
    const Pinned pinned_cases[] = {
        {"nothing pinned", kAuto, kAuto, kAuto},
        {"speaking pinned to the channel white", kAuto, 0xffffffu, kAuto},
        {"idle pinned to the overflow grey", 0xb5bac0u, kAuto, kAuto},
        {"idle and speaking pinned to one value", 0x808080u, 0x808080u, kAuto},
        {"body pinned to the title white", kAuto, kAuto, 0xffffffu},
        {"idle pinned to the hairline", 0xfefefeu, kAuto, kAuto},
    };

    for (uint32_t surface : {0x5865f2u, 0x000000u, 0xffffffu, 0x2b2d31u, 0xe8e8e8u})
    for (const Pinned& pinned : pinned_cases) {
        Config config;
        config.panel_colour = surface;
        config.notification_colour = surface;
        config.text_idle_colour = pinned.idle;
        config.text_speaking_colour = pinned.speaking;
        config.notification_text_colour = pinned.body;
        const Theme theme = theme_for(config);
        const std::string where =
            "palette for surface " + std::to_string(surface) +
            (is_light(surface) ? " (light)" : " (dark)") + ", " + pinned.label;

        const Named panel_roles[] = {
            {"avatar_placeholder", theme.avatar_placeholder},
            // The mark on that disc. A role of its own since it became a token,
            // and measurable because placeholder_inside_disc() below finds it by
            // its colour.
            {"avatar_mark", theme.avatar_mark},
            {"badge_fill", theme.badge_fill},
            {"text_channel", theme.text_channel},
            {"text_speaking", theme.text_speaking},
            {"text_idle", theme.text_idle},
            {"text_muted", theme.text_muted},
            {"separator", theme.separator},
            {"separator_accent", theme.separator_accent},
            // Measurable now, which is why the dark value sits one part in 255
            // off the idle grey it is meant to read as.
            {"text_overflow", theme.text_overflow},
            // Why the hairline is 0xfefefe rather than the white the channel name
            // wears, and 0x010101 on a pale box rather than the scrim's black: one
            // part in 255, spent on keeping this table distinct. The badge glyph
            // is 0xfffffe for the same reason. What is deliberately *not* in this
            // table: the scrim, the badge rim and the text outline's ink, which
            // are all black on a dark box by design -- nothing searches for them,
            // and a table that lists what nothing measures would forbid an
            // agreement nothing can be confused by.
            {"panel_hairline", theme.panel_hairline},
            {"badge_glyph", theme.badge_glyph},
        };
        const Named toast_roles[] = {
            {"avatar_placeholder", theme.avatar_placeholder},
            {"avatar_mark", theme.avatar_mark},
            {"toast_title", theme.toast_title},
            {"toast_body", theme.toast_body},
            {"toast_hairline", theme.toast_hairline},
            {"toast_accent", theme.toast_accent},
        };

        for (const Named* roles : {panel_roles, toast_roles}) {
            const size_t count = roles == panel_roles ? IM_ARRAYSIZE(panel_roles)
                                                      : IM_ARRAYSIZE(toast_roles);
            for (size_t i = 0; i < count; ++i) {
                for (size_t j = i + 1; j < count; ++j) {
                    check(ink(roles[i].colour) != ink(roles[j].colour),
                          where + ": " + roles[i].name + " and " + roles[j].name +
                              " are the same colour, so neither can be measured");
                }
            }
        }
    }
}

// The silhouette on a picture that has not arrived stays on the picture.
//
// Written while looking for a dark rim the owner saw under the shoulders, on
// the suspicion that the shoulders' arc ran the wrong way round its circle --
// ImGui walks an arc linearly from a_min to a_max, so a pair handed over
// decreasing draws the other side of the circle, which here would hang a
// crescent under every avatar. It does not: this passed the moment it was
// written, at every avatar size, which cleared the geometry and sent the search
// to the rasterisation, where the rim actually was (two antialiased fills
// sharing an edge; panel.cpp says what was measured and what the half pixel of
// overlap is for). The assertion stays, because the property is worth holding
// and nothing else was checking it: the lens is built from four angles that are
// fractions of the radius, and the sign of atan2 for a point above the centre is
// exactly what a reading gets backwards. The slack below is what carries that
// deliberate half pixel.
//
// One participant, so the panel's list holds exactly one disc and one mark, and
// three avatar sizes, because a shape that is right at one size is not a shape
// that is right.
void placeholder_inside_disc() {
    for (float avatar : {0.5f, 1.0f, 2.0f}) {
        Config config;
        config.avatar_size = avatar;
        config.panel_colour = kPanelSentinel;
        config.notifications_enabled = false;

        const float pixels = font_pixel_size(1080, config.scale, config.font_size);
        ensure_fonts(pixels, config.font_size, config.font_path.c_str(),
                 config.font_path_strong.c_str());
        configure_style(config);
        run_frames(make_snapshot(1, "Voice channel"), config, 1920, 1080, false);

        ImGuiWindow* panel = ImGui::FindWindowByName("##vocem");
        const ImDrawList* list = panel ? panel->DrawList : nullptr;
        const Theme theme = theme_for(config);
        const Rect disc = colour_bounds(list, ink(theme.avatar_placeholder));
        const Rect mark = colour_bounds(list, ink(theme.avatar_mark));

        char buffer[160];
        std::snprintf(buffer, sizeof(buffer), "the placeholder at avatar size %.1f", avatar);
        const std::string where = buffer;

        check(disc.valid(), where + ": the disc is drawn");
        check(mark.valid(), where + ": the head-and-shoulders mark is drawn");
        if (!disc.valid() || !mark.valid()) {
            continue;
        }
        // Antialiasing puts a vertex about half a pixel outside the shape, and
        // both shapes pay it, so the slack is the drawing's rather than the
        // measurement's tolerance for being wrong: the crescent this catches was
        // 0.59 of a radius past the disc, which is 8 pixels at the default size.
        const float slack = 0.75f;
        if (mark.x0 < disc.x0 - slack || mark.x1 > disc.x1 + slack ||
            mark.y0 < disc.y0 - slack || mark.y1 > disc.y1 + slack) {
            std::printf("  FAIL  %s: the mark leaves the disc "
                        "(disc %.2f,%.2f..%.2f,%.2f  mark %.2f,%.2f..%.2f,%.2f)\n",
                        where.c_str(), disc.x0, disc.y0, disc.x1, disc.y1, mark.x0, mark.y0,
                        mark.x1, mark.y1);
            ++failures;
        }

        // And the half pixel of overlap the seam needs, as a number.
        //
        // The rim the owner reported is a rasterisation property, which is why
        // entry 89 said it had no unit test and left the captured frame as the
        // only instrument. It has one: the lens's lower boundary IS the disc's
        // edge, both shapes are filled with the same antialiasing fringe (ImGui
        // pushes the outer ring of a convex fill half a unit out, imgui_draw.cpp
        // AddConvexPolyFilled), and a vertex sits exactly at the bottom of each
        // -- the disc's from the 48-sample circle, the lens's from the arc's
        // midpoint, which its own angles make symmetric. So the distance between
        // their lowest vertices is the overlap itself and nothing else: zero
        // while the two share an edge, half a pixel once the arc is drawn at
        // radius + 0.5. Measured at every avatar size below, which is what says
        // it is the drawing's constant rather than one size's accident.
        const float overlap = mark.y1 - disc.y1;
        std::printf("  %s: the shoulders overhang the disc by %.2f px\n", where.c_str(),
                    overlap);
        check(overlap > 0.25f && overlap < 0.75f,
              where + ": the shoulders overlap the edge of the disc by half a pixel");
    }
}

// A fading toast must fade everything it draws. The surface, the glyphs and a
// downloaded picture ride the pushed style alpha or an explicit multiplication;
// the placeholder disc and the text outline enter the draw list as raw colours
// the style alpha cannot reach, so each has to be multiplied by the fade at its
// call site -- the rule panel.cpp states for the hairline and the accent bar,
// and the rule the panel's own rows already follow for exactly these two shapes
// (row_alpha into draw_avatar_placeholder, outline * row_alpha into the text).
//
// Measured relatively, at a moment inside the 0.25 s entrance, against the same
// shapes at rest: the title's ink is the control that proves a fade is
// happening at that instant, and the two raw-colour shapes must come down in
// the same proportion. Nothing restates the fade's formula -- only "in step
// with the title" is asserted. Against the drawing that passed 1.0f and an
// unmultiplied strength, both ratios stay at 1.0 while the title's falls.
void toast_fade_carries_raw_colours() {
    Config config;
    config.text_shadow = true;  // the outline exists only when asked for
    config.notification_colour = kToastSentinel;
    const float pixels = font_pixel_size(1080, config.scale, config.font_size);
    ensure_fonts(pixels, config.font_size, config.font_path.c_str(),
                 config.font_path_strong.c_str());
    configure_style(config);
    const Theme theme = theme_for(config);
    Snapshot snapshot = make_snapshot(1, "Voice channel");

    // The toast is stateless -- a function of its own timestamps -- so one
    // frame per instant is a measurement, with no motion slots to settle.
    static double clock = 5000.0;
    const double ages[] = {1.0, 0.08};  // mid-life, then mid-entrance
    int title[2] = {-1, -1}, disc[2] = {-1, -1}, outline[2] = {-1, -1};
    for (int pass = 0; pass < 2; ++pass) {
        clock += 10.0;
        snapshot.notification.received = clock - ages[pass];
        ImGui::GetIO().DisplaySize = ImVec2(1920.0f, 1080.0f);
        ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
        ImGui::NewFrame();
        build_notification(snapshot, config, 1920, 1080, nullptr, clock);
        ImGui::Render();
        ImGuiWindow* toast = ImGui::FindWindowByName("##vocem_toast");
        const ImDrawList* list = toast ? toast->DrawList : nullptr;
        title[pass] = max_alpha(list, ink(theme.toast_title));
        disc[pass] = max_alpha(list, ink(theme.avatar_placeholder));
        outline[pass] = max_alpha(list, ink(theme.text_outline_ink));
    }

    std::printf("  toast alphas at rest: title %d disc %d outline %d\n", title[0], disc[0],
                outline[0]);
    std::printf("  toast alphas mid-entrance: title %d disc %d outline %d\n", title[1], disc[1],
                outline[1]);
    check(title[0] > 0 && disc[0] > 0 && outline[0] > 0,
          "the toast at rest draws title, placeholder and outline");
    if (title[0] <= 0 || disc[0] <= 0 || outline[0] <= 0) {
        return;
    }
    const float title_ratio = static_cast<float>(title[1]) / static_cast<float>(title[0]);
    const float disc_ratio = static_cast<float>(disc[1]) / static_cast<float>(disc[0]);
    const float outline_ratio = static_cast<float>(outline[1]) / static_cast<float>(outline[0]);
    check(title_ratio < 0.9f, "control: the title's ink is fading mid-entrance");
    check(std::fabs(disc_ratio - title_ratio) < 0.1f,
          "the placeholder disc rides the same fade as the glyphs");
    check(std::fabs(outline_ratio - title_ratio) < 0.1f,
          "the text outline rides the same fade as the glyphs");
}

// Which corner the toast actually lands in, for all four values of the
// setting. The numbering is spelled in four places (config.h's "0 TL, 1 TR,
// 2 BL, 3 BR", the drawing's right/bottom pairs, the window's two QML tables)
// and the suite measured only corner 3 -- the one value where right and
// bottom are both true, so swapping "top right" and "bottom left" would have
// passed every existing check. What is asserted is the setting's documented
// meaning against the drawn window's centre, not panel.cpp's arithmetic.
void toast_corners() {
    for (int corner = 0; corner < 4; ++corner) {
        Config config;
        config.notification_corner = corner;
        config.notification_colour = kToastSentinel;
        const float pixels = font_pixel_size(1080, config.scale, config.font_size);
        ensure_fonts(pixels, config.font_size, config.font_path.c_str(),
                     config.font_path_strong.c_str());
        configure_style(config);
        run_frames(make_snapshot(1, "Voice channel"), config, 1920, 1080, true);
        ImGuiWindow* toast = ImGui::FindWindowByName("##vocem_toast");
        const std::string where = "toast corner " + std::to_string(corner);
        check(toast != nullptr, where + ": the toast is drawn");
        if (!toast) {
            continue;
        }
        const float centre_x = toast->Pos.x + toast->Size.x * 0.5f;
        const float centre_y = toast->Pos.y + toast->Size.y * 0.5f;
        const bool wants_right = corner == 1 || corner == 3;  // config.h: 0 TL, 1 TR, 2 BL, 3 BR
        const bool wants_bottom = corner == 2 || corner == 3;
        check((centre_x > 960.0f) == wants_right,
              where + ": the box sits in the " + (wants_right ? "right" : "left") + " half");
        check((centre_y > 540.0f) == wants_bottom,
              where + ": and in the " + (wants_bottom ? "bottom" : "top") + " half");
    }
}

// The picture of somebody who is not talking is quieter, as their name is.
// Every version up to 0.1.9 greyed the name and left the face at full strength,
// which is not what Discord's overlay does; the owner saw it beside Discord's.
//
// Measured on the placeholder disc, which is the picture whenever there is no
// downloaded image -- here, always -- and relatively: the idle disc against the
// speaker's in the same frame, so nothing restates what alpha the theme gives
// the disc. The fixture's first participant speaks, the second is quiet, the
// third muted. Against the drawing before the setting existed the two discs
// come out equal and the first check fails.
void idle_picture_quieter() {
    Config config;
    config.notifications_enabled = false;
    config.panel_colour = kPanelSentinel;
    config.speaking_colour = kSpeakingSentinel;
    const float pixels = font_pixel_size(1080, config.scale, config.font_size);
    ensure_fonts(pixels, config.font_size, config.font_path.c_str(),
                 config.font_path_strong.c_str());
    configure_style(config);
    const Theme theme = theme_for(config);

    struct Strengths {
        std::vector<int> discs;
        int scrim = -1;
        int badge = -1;
    };
    auto settle = [&](const Config& c) {
        run_frames(make_snapshot(3, "Voice channel"), c, 1920, 1080, false);
        ImGuiWindow* panel = ImGui::FindWindowByName("##vocem");
        const ImDrawList* list = panel ? panel->DrawList : nullptr;
        Strengths out;
        out.discs = cluster_alphas(list, ink(theme.avatar_placeholder));
        out.scrim = max_alpha(list, ink(theme.avatar_scrim));
        out.badge = max_alpha(list, ink(theme.badge_fill));
        return out;
    };

    const Strengths quiet = settle(config);
    Config lit = config;
    lit.avatar_idle_opacity = 1.0f;
    const Strengths full = settle(lit);

    check(quiet.discs.size() >= 3 && full.discs.size() >= 3,
          "quiet pictures: three placeholder discs are drawn");
    if (quiet.discs.size() < 3 || full.discs.size() < 3 || quiet.discs[0] <= 0 ||
        full.scrim <= 0) {
        return;
    }
    const float idle_ratio =
        static_cast<float>(quiet.discs[1]) / static_cast<float>(quiet.discs[0]);
    const float scrim_ratio = static_cast<float>(quiet.scrim) / static_cast<float>(full.scrim);
    std::printf("  quiet pictures: speaker %d, quiet %d (%.3f of it), scrim %d of %d, "
                "badge %d of %d\n",
                quiet.discs[0], quiet.discs[1], idle_ratio, quiet.scrim, full.scrim, quiet.badge,
                full.badge);
    // One part in 255 on either alpha is the tolerance of the byte they are
    // stored in; 0.01 is well over it at these strengths and well under any
    // step the slider can take.
    check(std::fabs(idle_ratio - config.avatar_idle_opacity) < 0.01f,
          "a quiet participant's picture is drawn at the idle opacity of the speaker's");
    check(std::abs(full.discs[1] - full.discs[0]) <= 1,
          "at 100% the quiet picture is as strong as the speaker's, as up to 0.1.9");
    check(quiet.discs[0] == full.discs[0], "the speaker's picture is not touched by the setting");
    check(std::fabs(scrim_ratio - config.avatar_idle_opacity) < 0.02f,
          "a muted participant's scrim is quieted with the picture it lies on");
    check(quiet.badge == full.badge, "the badge is not: a quiet state is still true");

    // Timing: the picture follows the ring, not the flag. Discord's speaking
    // flag drops between two words; a picture that dimmed at each one would
    // flicker, so it holds with the ring and falls with it.
    Snapshot one = make_snapshot(1, "Voice channel");
    run_frames(one, config, 1920, 1080, false);  // speaking, settled
    one.users[0].flags = 0;
    const ImDrawList* list = panel_frame(one, config, 0.05);  // the flag drops
    list = panel_frame(one, config, 0.05);                    // inside the hold
    const std::vector<int> held = cluster_alphas(list, ink(theme.avatar_placeholder));
    list = panel_frame(one, config, 0.10);  // past the hold, into the fall
    const std::vector<int> falling = cluster_alphas(list, ink(theme.avatar_placeholder));
    const int ring = max_alpha(list, to_rgb(kSpeakingSentinel));
    run_frames(one, config, 1920, 1080, false);
    ImGuiWindow* panel = ImGui::FindWindowByName("##vocem");
    const std::vector<int> rested =
        cluster_alphas(panel ? panel->DrawList : nullptr, ink(theme.avatar_placeholder));
    if (held.empty() || falling.empty() || rested.empty()) {
        check(false, "quiet pictures: the disc is drawn in every frame of the fall");
        return;
    }
    std::printf("  quiet pictures over time: held %d, falling %d (ring %d), rested %d\n",
                held[0], falling[0], ring, rested[0]);
    check(held[0] == quiet.discs[0], "a pause inside the ring's hold does not dim the picture");
    check(ring > 1 && ring < 255, "control: the ring is mid-fall at that moment");
    check(falling[0] < held[0] && falling[0] > rested[0],
          "the picture falls with the ring, between lit and quiet");
    check(rested[0] == quiet.discs[1], "and comes to rest at the quiet strength");
}

void self_check() {
    distinct_tokens();
    placeholder_inside_disc();
    toast_fade_carries_raw_colours();
    toast_corners();
    idle_picture_quieter();

    const uint32_t modes[][2] = {{3840, 2160}, {1920, 1080}, {1280, 720}, {640, 480}};

    for (const auto& mode : modes) {
        Config config;
        verify(config, mode[0], mode[1], 3);
        Config sideways = config;
        sideways.panel_layout = Config::kLayoutHorizontal;
        verify(sideways, mode[0], mode[1], 3);
    }

    // What the horizontal layout *is*, as a measurement rather than as a
    // description: a second person costs the panel width and no height at all,
    // where upright they cost height and no width beyond the longest name. Two
    // measurements of the same channel, one per layout, on a display wide enough
    // for both to fit whole.
    {
        Config upright;
        const Measurement one_up = measure(upright, 1920, 1080, 1);
        const Measurement two_up = measure(upright, 1920, 1080, 2);
        Config sideways;
        sideways.panel_layout = Config::kLayoutHorizontal;
        const Measurement one_side = measure(sideways, 1920, 1080, 1);
        const Measurement two_side = measure(sideways, 1920, 1080, 2);

        // A person's own width -- picture, gap and name -- is what a sideways
        // panel pays for each of them and an upright one pays for none of them.
        // The bar is two lines of text: comfortably more than the couple of
        // units an upright box moves by when the second name's glyphs are a
        // shade wider than the first's (they are: "User 2" measures three units
        // past "User 1"), and comfortably less than a whole cell.
        const float a_person = one_up.line_height * 2.0f;
        check(two_up.panel.height() > one_up.panel.height() + 1.0f,
              "upright, a second person makes the panel taller");
        check(two_up.panel.width() < one_up.panel.width() + a_person,
              "upright, a second person does not cost the panel a person's width");
        check(two_side.panel.width() > one_side.panel.width() + a_person,
              "sideways, a second person costs the panel a person's width");
        check_close(two_side.panel.height(), one_side.panel.height(), 1.0f,
                    "sideways, a second person does not make the panel taller");
    }

    // And it runs out of room in the direction it grows in: a channel of 24 on a
    // narrow display draws what fits on the line and says how many are missing,
    // exactly as the column does when it reaches the bottom of the screen.
    {
        Config sideways;
        sideways.panel_layout = Config::kLayoutHorizontal;
        const Measurement m = measure(sideways, 640, 480, 24);
        check(m.overflow.valid(), "sideways, a full channel says how many did not fit");
        check(m.panel.width() + sideways.screen_margin * m.ui_scale * 2.0f <= 640.0f + 1.0f,
              "sideways, the line of people stays on the display");
    }

    // Every setting at both ends of its range, one at a time, so a failure names
    // the setting that caused it.
    struct Variant {
        const char* name;
        void (*apply)(Config&);
    };
    const Variant variants[] = {
        {"scale 0.5", [](Config& c) { c.scale = 0.5f; }},
        {"scale 3.0", [](Config& c) { c.scale = 3.0f; }},
        {"opacity 0", [](Config& c) { c.opacity = 0.0f; }},
        {"opacity 1", [](Config& c) { c.opacity = 1.0f; }},
        {"avatar 0.5", [](Config& c) { c.avatar_size = 0.5f; }},
        {"avatar 2.0", [](Config& c) { c.avatar_size = 2.0f; }},
        // An alpha, never a vertex: the quietest pictures sit where lit ones do.
        {"quiet pictures 0.1", [](Config& c) { c.avatar_idle_opacity = 0.1f; }},
        // Both distances, because each box has one of its own now.
        {"margin 0", [](Config& c) { c.screen_margin = 0.0f; c.notification_margin = 0.0f; }},
        {"margin 120",
         [](Config& c) { c.screen_margin = 120.0f; c.notification_margin = 120.0f; }},
        {"margins apart", [](Config& c) { c.screen_margin = 0.0f; c.notification_margin = 40.0f; }},
        // The anchor that had the defect: the middle of the left side.
        {"middle left", [](Config& c) { c.position_x = 0.0f; c.position_y = 0.5f; }},
        {"middle right", [](Config& c) { c.position_x = 1.0f; c.position_y = 0.5f; }},
        {"padding 0", [](Config& c) { c.box_padding_x = 0.0f; c.box_padding_y = 0.0f; }},
        {"padding 48", [](Config& c) { c.box_padding_x = 48.0f; c.box_padding_y = 48.0f; }},
        {"gap 0", [](Config& c) { c.avatar_gap = 0.0f; c.row_spacing = 0.0f; }},
        {"gap 48", [](Config& c) { c.avatar_gap = 48.0f; c.row_spacing = 48.0f; }},
        // The default hides the channel name, so the variants turn it *on*.
        // Without these, the channel block and the separator would only ever be
        // measured switched off.
        {"channel shown", [](Config& c) { c.show_channel_name = true; }},
        // The box around everything: the default up to 0.1.6, and now a chip.
        // Both modes have to be measured at both ends of the settings that
        // interact with them, which is why the pair below is a pair.
        {"boxed", [](Config& c) { c.panel_box = Config::kBoxPanel; c.opacity = 0.88f;
                                  c.show_channel_name = true; c.text_shadow = true; }},
        {"boxed, no padding", [](Config& c) { c.panel_box = Config::kBoxPanel;
                                              c.opacity = 0.88f; c.box_padding_x = 0.0f;
                                              c.box_padding_y = 0.0f; }},
        // The pill's own padding is claimed from the layout, and the setting
        // that would otherwise hide the claim is the window padding: at zero,
        // a box drawn outside its item is a box with its edges clipped off.
        {"names, no padding", [](Config& c) { c.box_padding_x = 0.0f; c.box_padding_y = 0.0f;
                                              c.show_channel_name = true; }},
        {"names, no gaps", [](Config& c) { c.avatar_gap = 0.0f; c.row_spacing = 0.0f;
                                           c.show_channel_name = true; }},
        {"names, small avatar", [](Config& c) { c.avatar_size = 0.5f; c.font_size = 32.0f;
                                                c.show_channel_name = true; }},
        {"names, outlined", [](Config& c) { c.text_shadow = true;
                                            c.show_channel_name = true; }},
        {"names, solid", [](Config& c) { c.opacity = 1.0f; c.show_channel_name = true; }},
        {"small text", [](Config& c) { c.font_size = 10.0f; }},
        {"large text", [](Config& c) { c.font_size = 32.0f; }},
        {"message 0.5", [](Config& c) { c.notification_scale = 0.5f; }},
        {"message 3.0", [](Config& c) { c.notification_scale = 3.0f; }},
        {"message faint", [](Config& c) { c.notification_opacity = 0.0f; }},
        {"bottom right", [](Config& c) { c.notification_corner = 3; }},
        {"far corner", [](Config& c) { c.position_x = 1.0f; c.position_y = 1.0f; }},
        // The other layout, and the settings whose meaning turns with it: the
        // row spacing is the distance between two people either way, and the
        // padding is what a line of people must stay inside.
        {"sideways", [](Config& c) { c.panel_layout = Config::kLayoutHorizontal; }},
        {"sideways, spaced", [](Config& c) { c.panel_layout = Config::kLayoutHorizontal;
                                            c.row_spacing = 48.0f; }},
        {"sideways, boxed", [](Config& c) { c.panel_layout = Config::kLayoutHorizontal;
                                           c.panel_box = Config::kBoxPanel;
                                           c.opacity = 0.88f; c.show_channel_name = true; }},
        {"sideways, names", [](Config& c) { c.panel_layout = Config::kLayoutHorizontal;
                                           c.show_channel_name = true; }},
        {"sideways, large", [](Config& c) { c.panel_layout = Config::kLayoutHorizontal;
                                           c.font_size = 32.0f; c.avatar_size = 2.0f; }},
        // Both ends of the setting that *is* the sideways layout's spacing: it
        // is handed to SameLine explicitly there rather than read out of the
        // style, so zero is a code path of its own and not a smaller number.
        {"sideways, touching", [](Config& c) { c.panel_layout = Config::kLayoutHorizontal;
                                              c.avatar_gap = 0.0f; c.row_spacing = 0.0f; }},
        // A wide box pushed against the corner it can least afford: the sideways
        // panel is as wide as the people in it, and the clamp that keeps it on
        // the display is the one thing its own width cap does not do.
        {"sideways, far corner", [](Config& c) { c.panel_layout = Config::kLayoutHorizontal;
                                                c.position_x = 1.0f; c.position_y = 1.0f; }},
    };

    for (const Variant& variant : variants) {
        for (uint32_t users : {1u, 20u}) {
            Config config;
            variant.apply(config);
            verify(config, 3840, 2160, users);
            Config small = config;
            verify(small, 1280, 720, users);
        }
    }

    // The motion contract: an ended animation leaves the resting geometry, so
    // measuring the same configuration twice -- with the animation clock well
    // advanced between the two -- gives byte-identical rectangles. If a phase
    // failed to settle, or an animation moved a vertex instead of an alpha, the
    // second measurement is the one that says so.
    {
        Config config;
        config.opacity = 0.88f;
        config.show_channel_name = true;
        const Measurement first = measure(config, 1920, 1080, 3);
        const Measurement second = measure(config, 1920, 1080, 3);
        const struct {
            const char* name;
            const Rect& a;
            const Rect& b;
        } pairs[] = {{"panel", first.panel, second.panel},
                     {"avatar", first.avatar, second.avatar},
                     {"ring", first.ring, second.ring},
                     {"badge", first.badge, second.badge},
                     {"first_name", first.first_name, second.first_name},
                     {"toast", first.toast, second.toast}};
        for (const auto& pair : pairs) {
            check_close(pair.b.x0, pair.a.x0, 0.01f,
                        std::string("settled motion: ") + pair.name + " does not drift");
            check_close(pair.b.y0, pair.a.y0, 0.01f,
                        std::string("settled motion: ") + pair.name + " does not drift down");
            check_close(pair.b.width(), pair.a.width(), 0.01f,
                        std::string("settled motion: ") + pair.name + " keeps its width");
            check_close(pair.b.height(), pair.a.height(), 0.01f,
                        std::string("settled motion: ") + pair.name + " keeps its height");
        }
    }

    // The switch that says "no voice panel" produces exactly that. It used to
    // produce nothing at all -- read, saved, toggled, and never consulted by any
    // drawing code -- so this asks ImGui's own bookkeeping whether the window
    // was submitted, which a stale rectangle from an earlier frame cannot fake.
    {
        Config off;
        off.panel_enabled = false;
        run_frames(make_snapshot(3, "Voice channel"), off, 1920, 1080, false);
        ImGuiWindow* panel = ImGui::FindWindowByName("##vocem");
        check(panel == nullptr || !panel->WasActive,
              "panel_enabled off: the voice panel is not drawn");
    }

    // row_spacing moves the rows and touches nothing else. It used to touch
    // everything: the avatar's radius was derived from the line-with-spacing
    // height, so the spacing slider resized every picture on the way past, and
    // the row carried the spacing inside its own height as well, so the pitch
    // counted it twice. Owner-reported, from dragging the slider live. The three
    // checks are the three ways it was wrong: the picture's diameter and a
    // single row's height are identical under any spacing, and the pitch grows
    // by exactly the spacing put in -- once.
    {
        Config tight;
        tight.row_spacing = 0.0f;
        tight.show_channel_name = false;
        Config loose = tight;
        loose.row_spacing = 48.0f;
        const Measurement a = measure(tight, 1920, 1080, 3);
        const Measurement b = measure(loose, 1920, 1080, 3);
        check_close(b.avatar.width(), a.avatar.width(), 0.01f,
                    "the picture's diameter does not follow row_spacing");
        check_close(b.panel_one_row_height, a.panel_one_row_height, 0.01f,
                    "a single row's height does not follow row_spacing");
        const float pitch_a = a.panel_two_rows_height - a.panel_one_row_height;
        const float pitch_b = b.panel_two_rows_height - b.panel_one_row_height;
        check_close(pitch_b - pitch_a, 48.0f, 0.75f,
                    "the spacing enters the pitch exactly once");
    }

    // The message is not a list, and `row_spacing` is the distance between one
    // person and the next.
    //
    // This is the one check here that compares two configurations rather than
    // holding one to a property, because the fault it guards against cannot be seen
    // in a single frame: the gap between the sender's name and the message used to
    // be `row_spacing`, so a slider on the panel's page moved a box on another one.
    // At 1080 lines it left eleven pixels of empty space under a name eleven pixels
    // tall -- reported as looking too far apart, and it was. What is left is the
    // leading a line of text carries inside its own height, which is a property of
    // the type and not of any setting, so the two figures below must be identical
    // and not merely close.
    {
        Config tight;
        tight.row_spacing = 0.0f;
        Config loose;
        loose.row_spacing = 48.0f;
        const Measurement a = measure(tight, 1920, 1080, 3);
        const Measurement b = measure(loose, 1920, 1080, 3);
        if (a.toast_title.valid() && a.toast_body.valid() && b.toast_title.valid() &&
            b.toast_body.valid()) {
            const float gap_a = a.toast_body.y0 - a.toast_title.y1;
            const float gap_b = b.toast_body.y0 - b.toast_title.y1;
            check_close(gap_b, gap_a, 0.01f,
                        "the sender-to-message gap does not follow the panel's row spacing");
            // And the panel's rows still do, or the setting would have stopped
            // doing the one thing it is for.
            check(b.panel_two_rows_height > a.panel_two_rows_height + 1.0f,
                  "but the panel's own rows still do");
        }
    }

    // Everything at once, both ways round: settings interact, and the interesting
    // failures are the ones no single slider produces.
    Config maximal;
    maximal.panel_box = Config::kBoxPanel;
    maximal.scale = 3.0f;
    maximal.avatar_size = 2.0f;
    maximal.opacity = 1.0f;
    maximal.show_channel_name = true;
    maximal.text_shadow = true;
    maximal.screen_margin = 120.0f;
    maximal.notification_margin = 120.0f;
    maximal.box_padding_x = 48.0f;
    maximal.box_padding_y = 48.0f;
    maximal.avatar_gap = 48.0f;
    maximal.row_spacing = 48.0f;
    maximal.notification_scale = 3.0f;
    verify(maximal, 3840, 2160, 20);
    verify(maximal, 1280, 720, 20);

    Config minimal;
    minimal.scale = 0.5f;
    minimal.avatar_size = 0.5f;
    minimal.screen_margin = 0.0f;
    minimal.notification_margin = 0.0f;
    minimal.box_padding_x = 0.0f;
    minimal.box_padding_y = 0.0f;
    minimal.avatar_gap = 0.0f;
    minimal.row_spacing = 0.0f;
    minimal.notification_scale = 0.5f;
    minimal.opacity = 0.0f;
    verify(minimal, 3840, 2160, 1);
    verify(minimal, 640, 480, 1);
}

// --- arguments --------------------------------------------------------------

bool set_field(Config& config, const std::string& key, const std::string& value) {
    const float number = static_cast<float>(std::atof(value.c_str()));
    if (key == "scale") config.scale = number;
    else if (key == "opacity") config.opacity = number;
    else if (key == "avatar_size") config.avatar_size = number;
    else if (key == "avatar_idle_opacity") config.avatar_idle_opacity = number;
    else if (key == "font_size") config.font_size = number;
    else if (key == "screen_margin") config.screen_margin = number;
    else if (key == "notification_margin") config.notification_margin = number;
    else if (key == "box_padding_x") config.box_padding_x = number;
    else if (key == "box_padding_y") config.box_padding_y = number;
    else if (key == "avatar_gap") config.avatar_gap = number;
    else if (key == "row_spacing") config.row_spacing = number;
    else if (key == "notification_scale") config.notification_scale = number;
    else if (key == "notification_opacity") config.notification_opacity = number;
    else if (key == "notification_corner") config.notification_corner = std::atoi(value.c_str());
    // Through Config's own reader, so the comparison spells the layout the way
    // the settings file does rather than as a number nobody would recognise.
    else if (key == "panel_layout")
        config.panel_layout = Config::to_layout(value.c_str(), config.panel_layout);
    // And where that panel's surface is drawn, spelled as the settings file
    // spells it, for the same reason.
    else if (key == "panel_box")
        config.panel_box = Config::to_box(value.c_str(), config.panel_box);
    // The typeface, so the comparison can be run in the font the user picked
    // rather than only in the carried one: the ratio between ImGui's size and
    // Qt's is a property of the file, and that is exactly what a preview drawn
    // in somebody else's font has to get right.
    else if (key == "font_family") config.font_family = value;
    else if (key == "font_path") config.font_path = value;
    else if (key == "font_path_strong") config.font_path_strong = value;
    else if (key == "position_x") config.position_x = number;
    else if (key == "position_y") config.position_y = number;
    else if (key == "show_channel_name") config.show_channel_name = value != "0";
    else if (key == "show_muted_state") config.show_muted_state = value != "0";
    else if (key == "only_speaking") config.only_speaking = value != "0";
    else if (key == "hide_self") config.hide_self = value != "0";
    else return false;
    return true;
}

}  // namespace

// The placement and its inverse have to be exactly each other, because a drag in
// the window maps pixels back to a fraction while the drawing maps the fraction to
// pixels: when the two disagreed by an inset the panel flashed under the pointer,
// which is how this got written. Pure arithmetic, so it is checked here rather
// than through a frame.
void check_placement_round_trip() {
    const float boxes[] = {40.0f, 101.0f, 337.0f};
    const float extents[] = {480.0f, 720.0f, 1080.0f, 2160.0f};
    const float insets[] = {0.0f, 16.0f, 120.0f};
    const float fractions[] = {0.0f, 0.25f, 0.5f, 0.75f, 1.0f};
    int cases = 0;
    float worst = 0.0f;
    for (float box : boxes) {
        for (float extent : extents) {
            for (float inset : insets) {
                for (float f : fractions) {
                    const float at = place_within(f, box, extent, inset);
                    const float back = fraction_within(at, box, extent, inset);
                    // Where the box cannot fit between the margins there is no
                    // travel and every fraction means the same place; the round
                    // trip is only a claim when there is room to move.
                    if (extent - box - inset * 2.0f <= 0.0f) {
                        continue;
                    }
                    ++cases;
                    const float drift = back > f ? back - f : f - back;
                    if (drift > worst) {
                        worst = drift;
                    }
                }
            }
        }
    }
    // The count is printed because a drift of zero over no cases is not a result,
    // it is a test that did not run -- and this file is where that distinction is
    // made about somebody else's code.
    std::printf("    placement round trip: worst drift %.6f over %d cases\n", worst, cases);
    check(cases > 100, "the round trip was actually walked");
    check(worst < 0.0005f, "a fraction survives being placed and read back");

    // And the two ends are the margins themselves, which is what a drag is limited
    // to: getting these wrong is what left `drag.maximumX` undefined.
    check_close(place_within(0.0f, 101.0f, 1920.0f, 16.0f), 16.0f, 0.01f,
                "fraction 0 puts the box at the near margin");
    check_close(place_within(1.0f, 101.0f, 1920.0f, 16.0f), 1920.0f - 101.0f - 16.0f, 0.01f,
                "fraction 1 puts the box at the far margin");
    check_close(place_within(0.5f, 100.0f, 1000.0f, 16.0f), 450.0f, 0.01f,
                "and a half centres it");
}

int main(int argc, char** argv) {
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.DisplaySize = ImVec2(1920.0f, 1080.0f);
    io.DeltaTime = 1.0f / 60.0f;
    unsigned char* pixels = nullptr;
    int atlas_width = 0, atlas_height = 0;

    Config config;
    uint32_t width = 3840, height = 2160, users = 3;
    bool dump = false;

    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const size_t equals = argument.find('=');
        if (equals == std::string::npos) {
            std::fprintf(stderr, "unrecognised argument: %s\n", argument.c_str());
            return 2;
        }
        const std::string key = argument.substr(0, equals);
        const std::string value = argument.substr(equals + 1);
        dump = true;
        if (key == "width") width = static_cast<uint32_t>(std::atoi(value.c_str()));
        else if (key == "height") height = static_cast<uint32_t>(std::atoi(value.c_str()));
        else if (key == "users") users = static_cast<uint32_t>(std::atoi(value.c_str()));
        else if (!set_field(config, key, value)) {
            std::fprintf(stderr, "unrecognised setting: %s\n", key.c_str());
            return 2;
        }
    }

    // The atlas has to exist before the first frame; ensure_fonts() builds it and
    // the null backend only needs the pixels to have been rasterised.
    ensure_fonts(font_pixel_size(height, config.scale, config.font_size), config.font_size,
                 config.font_path.c_str(), config.font_path_strong.c_str());
    io.Fonts->GetTexDataAsRGBA32(&pixels, &atlas_width, &atlas_height);
    io.Fonts->SetTexID(static_cast<ImTextureID>(1));

    if (dump) {
        print_json(measure(config, width, height, users));
        ImGui::DestroyContext();
        return 0;
    }

    check_placement_round_trip();
    self_check();
    ImGui::DestroyContext();
    if (failures == 0) {
        std::printf("panel geometry: every invariant holds\n");
        return 0;
    }
    std::printf("panel geometry: %d failures\n", failures);
    return 1;
}

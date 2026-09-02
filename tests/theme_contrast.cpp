// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Whether the overlay's text can actually be read on the box it is drawn on.
//
// The panel had a palette that nobody had ever measured. On the default background
// -- Discord's blurple, which is what the overlay opens as -- the name of somebody
// who is not speaking came out at 1.64:1 against it, and the name of somebody who
// is muted at 1.02:1, which is to say invisible. Both were arrived at honestly:
// they are Discord's own greys, and Discord draws them on a near-black surface
// rather than on the blurple. Nothing in the tree could have said so, because a
// colour is not a distance and `tests/panel_geometry.cpp` measures distances.
//
// So this measures the colours, by the standard's own formula: WCAG 2.2 relative
// luminance, and the ratio between two of them. It runs against theme_for() and
// nothing else -- no ImGui, no font atlas, no window -- because the palette is a
// pure function of the settings and the point is to hold it to something before it
// reaches a screen.
//
// Two kinds of check live here, and the difference matters:
//
//   * The properties, asserted. A pale box gets dark text and a dark box gets pale
//     text; the three states of a name are ordered, so whoever is speaking always
//     reads more strongly than whoever is not, who always reads more strongly than
//     whoever is muted. These hold today and have to keep holding.
//   * The absolute floor -- 4.5:1 for text, measured on the composite -- asserted
//     on every *boxed* surface this project itself offers: the presets that draw
//     one. It used to be printed rather than asserted, because the old default
//     failed it; the surfaces that meet it arrived, and the assertion arrived
//     with them, which is what the earlier note here promised. The default draws
//     no box at all (the owner's choice), so it has no composite to hold; a
//     surface the user picks freely is still only printed: no palette can
//     promise a floor on an arbitrary box, and the table is what tells them what
//     their choice measured.
//     The 3:1 non-text floor stays printed, not asserted: who is speaking is said
//     twice -- the ring and the weight of the name -- so the ring is never the
//     sole carrier, and on a pale surface no green that still reads as "speaking"
//     clears 3:1 (measured: the default green is 2.79:1 on Breeze Light over
//     snow, and Breeze's own Positive is worse).
//
// The light-box palette has no other coverage anywhere: the geometry harness
// forces the panel colour to a near-black sentinel so that shapes can be told
// apart by colour, so `is_light()` is never taken through the drawing. Here it is
// exercised directly.

#include <cmath>
#include <cstdio>
#include <string>

#include "vocem/config.h"
#include "vocem/theme.h"

using namespace vocem;

namespace {

int failures = 0;

void check(bool condition, const std::string& what) {
    if (!condition) {
        std::printf("  FAIL  %s\n", what.c_str());
        ++failures;
    }
}

// WCAG 2.2, "relative luminance": each channel to linear light, then the standard
// weights. https://www.w3.org/WAI/WCAG22/Understanding/contrast-minimum.html
double channel(uint8_t value) {
    const double c = static_cast<double>(value) / 255.0;
    return c <= 0.04045 ? c / 12.92 : std::pow((c + 0.055) / 1.055, 2.4);
}

double luminance(Colour colour) {
    return 0.2126 * channel(colour.r) + 0.7152 * channel(colour.g) +
           0.0722 * channel(colour.b);
}

double contrast(Colour a, Colour b) {
    const double la = luminance(a);
    const double lb = luminance(b);
    const double high = la > lb ? la : lb;
    const double low = la > lb ? lb : la;
    return (high + 0.05) / (low + 0.05);
}

// The panel is drawn over a game, so the surface the text really sits on is the
// panel colour composited over whatever happens to be behind it. Measuring against
// the token alone would flatter every translucent setting -- which is the mistake
// the accessibility guidance for translucent interfaces exists to name: the ratio
// belongs to the composited result, not to the pair of colours.
Colour over(Colour surface, float opacity, uint32_t backdrop) {
    const auto mix = [opacity](uint8_t front, uint8_t back) {
        const float value = static_cast<float>(front) * opacity +
                            static_cast<float>(back) * (1.0f - opacity);
        return static_cast<uint8_t>(value + 0.5f);
    };
    return Colour{mix(surface.r, static_cast<uint8_t>((backdrop >> 16) & 0xff)),
                  mix(surface.g, static_cast<uint8_t>((backdrop >> 8) & 0xff)),
                  mix(surface.b, static_cast<uint8_t>(backdrop & 0xff)), 255};
}

// The three scenes that decide whether an overlay is legible: a dark interior, a
// mid-tone outdoors, and snow. The last is the one that matters -- it is where a
// translucent dark box comes closest to the text drawn on it.
struct Scene {
    const char* name;
    uint32_t colour;
};
constexpr Scene kScenes[] = {
    {"dark", 0x0a0a0a},
    {"mid", 0x707070},
    {"snow", 0xf0f0f0},
};

// Beyond the presets -- which are read from theme.h itself, so this file cannot
// measure a copy of them -- the extremes a colour picker can reach. Never
// promised: no palette can hold a floor on an arbitrary box, and the table is
// what tells whoever picked one what their choice measured.
struct Surface {
    const char* name;
    uint32_t colour;
    float opacity;
};
constexpr Surface kSurfaces[] = {
    {"near-black", 0x1e1f22, 0.88f},
    {"black", 0x000000, 1.0f},
    {"white", 0xffffff, 1.0f},
};

const char* verdict(double ratio, double floor) { return ratio >= floor ? "ok " : "FAILS"; }

void report(const Config& config, const char* surface_name, bool promised = false) {
    const Theme theme = theme_for(config);
    const std::string where = std::string(surface_name);

    // --- the properties, asserted -----------------------------------------
    //
    // A pale box has to get dark text, and a dark box pale text. Stated as a
    // comparison against the surface rather than against a threshold, because that
    // is the actual requirement: the text has to be on the other side of the box
    // from wherever the box is.
    //
    // There used to be a known defect registered here -- on the old blurple
    // default the muted grey crossed *under* the box it sat on, 1.02:1 -- held by
    // a check that failed the day the case started holding. It started holding
    // when the dark ramp was rechosen against the surface it actually sits on,
    // and the machinery left with it: an empty list of known defects is the only
    // good length for one.
    const double surface_luma = luminance(theme.panel_surface);
    const bool light = is_light(config.panel_colour);
    for (const auto& named : {std::pair<const char*, Colour>{"channel", theme.text_channel},
                              {"speaking", theme.text_speaking},
                              {"idle", theme.text_idle},
                              {"muted", theme.text_muted},
                              {"overflow", theme.text_overflow}}) {
        const double text_luma = luminance(named.second);
        const bool holds = light ? (text_luma < surface_luma) : (text_luma > surface_luma);
        check(holds, where + ": the " + named.first + " name is on the wrong side of its box");
    }

    // Whoever is speaking reads more strongly than whoever is not, who reads more
    // strongly than whoever is muted. The panel says who is talking twice -- the
    // ring and the weight of the name -- and this is the half that survives being
    // looked at out of the corner of an eye. An inversion here would be a palette
    // that says the quiet people are the important ones.
    const double speaking = contrast(theme.text_speaking, theme.panel_surface);
    const double idle = contrast(theme.text_idle, theme.panel_surface);
    const double muted = contrast(theme.text_muted, theme.panel_surface);
    check(speaking > idle, where + ": a speaking name does not read more strongly than an idle one");
    check(idle > muted, where + ": an idle name does not read more strongly than a muted one");

    // The badge sits on the picture, not on the box, and its whole job is to be
    // seen against a photograph nobody chose. Its glyph has to clear its own fill.
    check(contrast(theme.badge_glyph, theme.badge_fill) >= 3.0,
          where + ": the badge glyph does not clear 3:1 against the badge");

    // --- the floor, measured -- and held, where it is promised -------------
    std::printf("\n  %s, at the opacity it is drawn with (%.0f%%)%s\n", surface_name,
                config.opacity * 100.0f, promised ? "  [asserted]" : "");
    std::printf("      %-10s %-9s %-9s %-9s %-9s\n", "scene", "channel", "speaking", "idle",
                "muted");
    for (const Scene& scene : kScenes) {
        const Colour composite = over(theme.panel_surface, config.opacity, scene.colour);
        const struct {
            const char* role;
            double ratio;
        } text[] = {{"channel", contrast(theme.text_channel, composite)},
                    {"speaking", contrast(theme.text_speaking, composite)},
                    {"idle", contrast(theme.text_idle, composite)},
                    {"muted", contrast(theme.text_muted, composite)}};
        std::printf("      %-10s %5.2f %s %5.2f %s %5.2f %s %5.2f %s\n", scene.name,
                    text[0].ratio, verdict(text[0].ratio, 4.5), text[1].ratio,
                    verdict(text[1].ratio, 4.5), text[2].ratio, verdict(text[2].ratio, 4.5),
                    text[3].ratio, verdict(text[3].ratio, 4.5));
        if (promised) {
            for (const auto& entry : text) {
                check(entry.ratio >= 4.5,
                      where + ": the " + entry.role + " name misses 4.5:1 over " + scene.name);
            }
        }
    }
    // The ring and the badge carry meaning without carrying text, which is the
    // 3:1 case rather than the 4.5:1 one.
    const Colour worst = over(theme.panel_surface, config.opacity, kScenes[2].colour);
    std::printf("      %-10s ring %5.2f %s   badge %5.2f %s   (3:1, non-text)\n", "snow",
                contrast(theme.speaking_ring, worst), verdict(contrast(theme.speaking_ring, worst), 3.0),
                contrast(theme.badge_fill, worst), verdict(contrast(theme.badge_fill, worst), 3.0));
}

}  // namespace

int main() {
    std::printf("Contrast of the overlay's palette, WCAG 2.2 relative luminance.\n");
    std::printf("Text is held to 4.5:1 and a meaningful shape to 3:1, measured on the\n");
    std::printf("surface composited over the scene rather than on the token alone.\n");

    // The first preset and the defaults are two spellings of one decision, and
    // two spellings drift. Held together here, since config.h cannot include
    // theme.h to say so itself. Three fields now: a preset says where its
    // surface is drawn as well as what it is, and two chips that share a colour
    // and an opacity are still two different pictures.
    check(kPresets[0].colour == Config{}.panel_colour &&
              std::fabs(kPresets[0].opacity - Config{}.opacity) < 0.001f &&
              kPresets[0].box == Config{}.panel_box,
          "the first preset is the default the overlay opens with");

    // Every preset the window offers, from the same table the window reads.
    // Shipping a preset is promising it reads, and a promise this file does not
    // hold is a table nobody re-reads -- so the floor is asserted for all of them
    // except the two that cannot carry one: purple, which is the old look kept
    // for whoever wants the client's own blurple, and transparent, where there is
    // no surface to composite and the text outline carries the names instead.
    for (const Preset& preset : kPresets) {
        if (preset.opacity <= 0.0f) {
            continue;
        }
        Config config;
        config.panel_colour = preset.colour;
        config.notification_colour = preset.colour;
        config.opacity = preset.opacity;
        // Where the surface goes does not change what it composites to -- a pill
        // behind a name is the same colour at the same opacity as a box around
        // everything, which is exactly why the default can move onto the pill
        // and keep the floor the dark box measured. Carried through anyway, so
        // that a preset which ever did change the palette by where it draws is
        // measured as it draws.
        config.panel_box = preset.box;
        // The purple preset is the old look kept for whoever wants it, and it
        // cannot hold the floor: measured, its idle grey reads 2.36:1 on that
        // surface. It is printed rather than promised, as it always was under its
        // previous name -- the promise belongs to the presets the defaults are
        // built on.
        const bool promised = std::string(preset.id) != "purple";
        const std::string name = "preset " + std::string(preset.id);
        report(config, name.c_str(), promised);
    }

    for (const Surface& surface : kSurfaces) {
        Config config;
        config.panel_colour = surface.colour;
        config.notification_colour = surface.colour;
        config.opacity = surface.opacity;
        report(config, surface.name, false);
    }

    // The message box, at the defaults. Unlike the panel it draws its surface
    // out of the box -- a message is read once, at a glance -- so its floor is a
    // promise of the defaults themselves. The body is the palette's idle grey
    // again (theme.h says why it stopped being white), so the sender and what
    // the sender said are two colours as well as two weights. Three things are
    // held here: the floor on both, and a body that never reads more strongly
    // than the sender -- which is the property the grey exists for.
    {
        Config config;
        const Theme theme = theme_for(config);
        std::printf("\n  the message box, default surface at %.0f%%  [asserted]\n",
                    config.notification_opacity * 100.0f);
        for (const Scene& scene : kScenes) {
            const Colour composite =
                over(theme.toast_surface, config.notification_opacity, scene.colour);
            const double title = contrast(theme.toast_title, composite);
            const double body = contrast(theme.toast_body, composite);
            std::printf("      %-10s title %5.2f %s   body %5.2f %s\n", scene.name, title,
                        verdict(title, 4.5), body, verdict(body, 4.5));
            const std::string where = std::string("message box over ") + scene.name;
            check(title >= 4.5, where + ": the sender misses 4.5:1");
            check(body >= 4.5, where + ": the message misses 4.5:1");
            check(title >= body, where + ": the body reads more strongly than the sender");
        }
    }

    // The three pinned text colours (kColourAuto is every default): a pinned
    // value is the user's own choice and gets no floor, like a pinned surface --
    // but it must never make two roles of one draw list the same colour, or the
    // geometry measurement returns somebody else's rectangle. The nudge that
    // guarantees that lives in theme_for(); the assertion that it worked lives
    // with the other colour-distinctness checks, in tests/panel_geometry.cpp
    // (distinct_tokens). Here only the replacement itself is held: a pinned
    // colour actually lands on its role.
    {
        Config config;
        config.text_idle_colour = 0x123456;
        config.text_speaking_colour = 0x654321;
        config.notification_text_colour = 0x112233;
        const Theme theme = theme_for(config);
        const auto rgb = [](Colour colour) {
            return (static_cast<uint32_t>(colour.r) << 16) |
                   (static_cast<uint32_t>(colour.g) << 8) | colour.b;
        };
        check(rgb(theme.text_idle) == 0x123456, "a pinned idle colour does not land on the role");
        check(rgb(theme.text_speaking) == 0x654321,
              "a pinned speaking colour does not land on the role");
        check(rgb(theme.toast_body) == 0x112233,
              "a pinned message colour does not land on the role");
    }

    // The default colour with the background raised halfway: the slider's middle
    // ground between the transparent default and the Dark preset. Printed, never
    // promised: at partial opacities the floor belongs to the text outline,
    // which is a treatment this composite model cannot measure -- there is no
    // one background behind an outlined glyph on a raw scene.
    {
        Config config;
        config.opacity = 0.5f;
        report(config, "the default colour at half opacity", false);
    }

    if (failures == 0) {
        std::printf("\ntheme contrast: every property holds\n");
        return 0;
    }
    std::printf("\ntheme contrast: %d failures\n", failures);
    return 1;
}

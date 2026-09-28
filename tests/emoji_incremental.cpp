// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// An emoji nobody had seen must not cost the whole atlas.
//
// This is the measurement behind the owner's report that a game freezes for
// about half a second "when somebody joins the call or a message arrives". Both
// of those are the same thing to this module: a frame whose text carries a
// codepoint the atlas does not hold yet. Until this test, that made
// ensure_fonts() call atlas->Clear() and atlas->Build() -- 14,954 glyphs in two
// weights rasterised again to make room for one 32x32 square, measured at 125,
// 127, 129, 131, 137 and 139 ms on this machine, and again at 125 to 145 on a
// second run. Six new emoji in six frames is three quarters of a second inside
// somebody's game, and a channel filling up meets far more than six.
//
// What is held here is a COUNT and not a clock, for the reason entry 145 gives
// and entry 185 paid for: a build's wall time is partly the machine's, and the
// claim worth making is that the rasteriser DOES NOT RUN. Against the module as
// it stood every one of these emoji adds a build, and the first check below
// fails with 2 where it wants 1.
//
// The other half, and the one an optimisation has to earn: the emoji must still
// arrive, in both weights, with colour in its atlas pixels. A fold that quietly
// drew nothing would pass a count-only test perfectly.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cstring>
#include <vector>

#include "imgui.h"
#include "vocem/fonts.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

// The biggest |r-g| over the glyph's opaque atlas pixels: zero for a white
// monochrome shape, large for a real colour emoji. emoji_atlas.cpp's helper,
// which is the one spelling of "did this arrive in colour".
int glyph_chroma(ImFont* font, unsigned int codepoint) {
    const ImFontGlyph* glyph = font->FindGlyphNoFallback(static_cast<ImWchar>(codepoint));
    if (!glyph) {
        return -1;
    }
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    if (!pixels) {
        return -1;
    }
    const int x0 = static_cast<int>(glyph->U0 * static_cast<float>(width));
    const int x1 = static_cast<int>(glyph->U1 * static_cast<float>(width));
    const int y0 = static_cast<int>(glyph->V0 * static_cast<float>(height));
    const int y1 = static_cast<int>(glyph->V1 * static_cast<float>(height));
    int best = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            const unsigned char* p = pixels + (y * width + x) * 4;
            if (p[3] < 128) {
                continue;
            }
            const int spread = p[0] > p[1] ? p[0] - p[1] : p[1] - p[0];
            if (spread > best) {
                best = spread;
            }
        }
    }
    return best;
}

// Ordinary emoji, each a codepoint the bank carries, none of them related: a
// name's decoration and the sort of thing a message arrives with.
struct Arrival {
    const char* utf8;
    unsigned int codepoint;
    const char* said;
};

const Arrival kArrivals[] = {
    {"\xF0\x9F\x98\x80", 0x1F600, "a grin"},
    {"\xF0\x9F\x98\x82", 0x1F602, "tears of joy"},
    {"\xF0\x9F\x91\x8D", 0x1F44D, "a thumbs up"},
    {"\xF0\x9F\x94\xA5", 0x1F525, "a fire"},
    {"\xF0\x9F\x8E\xAE", 0x1F3AE, "a gamepad"},
    {"\xF0\x9F\x9A\x80", 0x1F680, "a rocket"},
};

}  // namespace

int main() {
    if (!getenv("VOCEM_EMOJI_BANK")) {
        printf("skip meant to run with VOCEM_EMOJI_BANK pointing at the repo's bank\n");
        return 77;
    }

    // The context the injected paths create: with the fonts module's own atlas,
    // which is what the dead band is a promise about at all (vocem/fonts.h).
    ImGui::CreateContext(vocem::fonts_atlas());

    // The atlas a game already drawing has. Nobody has been seen yet, which is
    // the ordinary case: a game started before anyone joined the channel.
    check(vocem::ensure_fonts(16.0f, 16.0f), "the first frame builds the atlas");
    check(vocem::fonts_build_count() == 1, "rasterised once");

    // Refetched after every call, never cached across one: a rebuild deletes
    // every ImFont, and a pointer taken before one is freed memory afterwards
    // (entry 37, and the bug emoji_atlas.cpp itself once had).
    const auto body_now = [] { return const_cast<ImFont*>(vocem::fonts().body); };
    const auto strong_now = [] { return const_cast<ImFont*>(vocem::fonts().strong); };

    // The glyphs a fold adds, counted: one per weight per emoji, and nothing
    // else. A fold that rebuilt the whole lookup table appended a second TAB
    // glyph each time (BuildLookupTable's "FIXME: Flaky" branch), which is the
    // countable trace of the rebuild entry 209 took out -- 984,119 entries a
    // weight with a sequence key in the font, 1.6 to 4.2 ms a fold.
    const int glyphs_before = body_now()->Glyphs.Size;
    const unsigned arrivals = sizeof(kArrivals) / sizeof(kArrivals[0]);

    // Now people arrive, one per frame, each with an emoji nobody had seen.
    for (unsigned i = 0; i < arrivals; ++i) {
        const Arrival& arrival = kArrivals[i];
        char name[128];
        snprintf(name, sizeof(name), "somebody %s", arrival.utf8);
        vocem::fonts_prepare_text(name, sizeof(name));

        char said[192];
        snprintf(said, sizeof(said), "%s reaches the atlas", arrival.said);
        check(vocem::ensure_fonts(16.0f, 16.0f), said);

        snprintf(said, sizeof(said), "  and the rasteriser has still run once (%u)",
                 vocem::fonts_build_count());
        check(vocem::fonts_build_count() == 1, said);
    }
    printf("     the body font grew by %d glyphs for %u folded emoji\n",
           body_now()->Glyphs.Size - glyphs_before, arrivals);
    check(body_now()->Glyphs.Size - glyphs_before == static_cast<int>(arrivals),
          "each fold adds its glyph and nothing else: the lookup table is not rebuilt");

    // The fold is not an excuse to draw nothing: every one of them, in both
    // weights, with colour in the pixels.
    for (unsigned i = 0; i < sizeof(kArrivals) / sizeof(kArrivals[0]); ++i) {
        const Arrival& arrival = kArrivals[i];
        char said[192];
        const int chroma = glyph_chroma(body_now(), arrival.codepoint);
        snprintf(said, sizeof(said), "%s is coloured in the body weight (chroma %d)",
                 arrival.said, chroma);
        check(chroma > 40, said);

        const ImFontGlyph* strong = strong_now()->FindGlyphNoFallback(
            static_cast<ImWchar>(arrival.codepoint));
        snprintf(said, sizeof(said), "  and present in the heavier one");
        check(strong != nullptr && strong->Colored, said);
    }

    // The atlas once its first whole upload is done: the texture holds it, and
    // the module hands its RGBA copy back and keeps the alpha8 image and the
    // colour squares. A fold then must not widen the atlas again (a copy
    // nobody would hand back), its squares must come with their own pixels,
    // and the next whole upload -- a new context, a second device -- must find
    // every colour square back in the widened copy: ImGui's own widening knows
    // nothing of them.
    vocem::fonts_atlas_uploaded();
    check(ImGui::GetIO().Fonts->TexPixelsRGBA32 == nullptr &&
              ImGui::GetIO().Fonts->TexPixelsAlpha8 != nullptr,
          "after the whole upload the atlas keeps its alpha8 image and no RGBA copy");
    vocem::fonts_take_folded(nullptr, 0);
    {
        char name[128];
        snprintf(name, sizeof(name), "confetti \xF0\x9F\x8E\x89");  // U+1F389
        vocem::fonts_prepare_text(name, sizeof(name));
        check(vocem::ensure_fonts(16.0f, 16.0f), "an emoji folds into an atlas that is alpha8 only");
        check(vocem::fonts_build_count() == 1, "  without rasterising");
        check(ImGui::GetIO().Fonts->TexPixelsRGBA32 == nullptr, "  and without widening it");
        vocem::AtlasRegion regions[vocem::kMaxFoldedRegions];
        const uint32_t count = vocem::fonts_take_folded(regions, vocem::kMaxFoldedRegions);
        int best = -1;
        for (uint32_t r = 0; r < count; ++r) {
            if (!regions[r].pixels) {
                best = -1;
                break;
            }
            for (int p = 0; p < regions[r].width * regions[r].height; ++p) {
                const unsigned char* px = regions[r].pixels + p * 4;
                const int spread = px[0] > px[1] ? px[0] - px[1] : px[1] - px[0];
                if (px[3] >= 128 && spread > best) {
                    best = spread;
                }
            }
        }
        char said[192];
        snprintf(said, sizeof(said),
                 "  its %u square(s) carry their own coloured pixels (chroma %d)", count, best);
        check(count == 2 && best > 40, said);
    }
    // The Vulkan layer's whole upload widens straight into its staging buffer
    // (fonts_atlas_widen_into): without making the RGBA copy, and into the
    // very pixels fonts_atlas_rgba() gives, colour squares included.
    std::vector<unsigned char> staged;
    {
        const int width = ImGui::GetIO().Fonts->TexWidth;
        const int height = ImGui::GetIO().Fonts->TexHeight;
        staged.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 4, 0xAB);
        check(vocem::fonts_atlas_widen_into(staged.data(), width, height),
              "the atlas widens into a caller's buffer");
        check(ImGui::GetIO().Fonts->TexPixelsRGBA32 == nullptr,
              "  without making its own RGBA copy");
        check(!vocem::fonts_atlas_widen_into(staged.data(), width, height + 1),
              "  and refuses a buffer of another size");
    }
    {
        unsigned char* pixels = nullptr;
        int width = 0;
        int height = 0;
        check(vocem::fonts_atlas_rgba(&pixels, &width, &height) && pixels != nullptr,
              "the next whole upload widens the atlas again");
        check(staged.size() == static_cast<size_t>(width) * static_cast<size_t>(height) * 4 &&
                  std::memcmp(staged.data(), pixels, staged.size()) == 0,
              "  to the same pixels the caller's buffer got");
        bool all = glyph_chroma(body_now(), 0x1F389) > 40 &&
                   glyph_chroma(strong_now(), 0x1F389) > 40;
        for (unsigned i = 0; i < arrivals; ++i) {
            all = all && glyph_chroma(body_now(), kArrivals[i].codepoint) > 40 &&
                  glyph_chroma(strong_now(), kArrivals[i].codepoint) > 40;
        }
        check(all, "  with every colour square back in it, in both weights");
    }

    // Nothing new: the dead band still holds, and no caller is told to re-upload.
    check(!vocem::ensure_fonts(16.0f, 16.0f), "a quiet frame folds nothing");
    check(vocem::fonts_build_count() == 1, "and rasterises nothing");

    // A codepoint the bank does not carry is still not a reason for anything.
    vocem::fonts_note_emoji("\xE2\x80\x99");  // typographic apostrophe
    check(!vocem::ensure_fonts(16.0f, 16.0f), "a non-emoji codepoint folds nothing");
    check(vocem::fonts_build_count() == 1, "and rasterises nothing");

    // The rebuild the module still owes: a size change is a different atlas, and
    // removing the emoji rebuild must not have removed that one. The emoji the
    // session collected have to survive it -- they are what the text still says.
    check(vocem::ensure_fonts(24.0f, 16.0f), "a size change rebuilds");
    check(vocem::fonts_build_count() == 2, "rasterised a second time, and only a second");
    {
        const int chroma = glyph_chroma(body_now(), kArrivals[0].codepoint);
        char said[192];
        snprintf(said, sizeof(said), "the grin survived the rebuild in colour (chroma %d)",
                 chroma);
        check(chroma > 40, said);
        check(glyph_chroma(body_now(), kArrivals[5].codepoint) > 40,
              "and so did the rocket, which arrived by folding");
    }

    // And after a rebuild the folding still works: the slots were handed out
    // again by the new build, not carried over from the dead atlas.
    vocem::fonts_prepare_text(const_cast<char*>(""), 0);
    {
        char name[128];
        snprintf(name, sizeof(name), "late \xF0\x9F\x8D\x95");  // pizza
        vocem::fonts_prepare_text(name, sizeof(name));
        check(vocem::ensure_fonts(24.0f, 16.0f), "an emoji after the rebuild folds in");
        check(vocem::fonts_build_count() == 2, "without rasterising a third time");
        check(glyph_chroma(body_now(), 0x1F355) > 40, "and the pizza is coloured");
    }

    printf("%s\n", failures == 0 ? "all ok" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

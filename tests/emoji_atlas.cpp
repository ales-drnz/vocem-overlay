// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A colour emoji seen in a name must reach the atlas in colour.
//
// The atlas drew every emoji as a white monochrome glyph -- the honest best
// while decoding colour emoji at runtime meant FreeType and libpng inside
// somebody's game. The bank moved the decoding offline; this holds the other
// half: fonts_note_emoji() remembers what the frame showed, the next
// ensure_fonts() folds it into the atlas as a coloured glyph in BOTH weights
// (a merged glyph belongs to the font it was merged into -- entry 26), and the
// glyph's atlas pixels actually carry colour. Against the module as it stood,
// the glyph exists, is monochrome, and every check below the first one fails.

#include <stdio.h>
#include <stdlib.h>

#include "imgui.h"
#include "vocem/fonts.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

// The glyph's rectangle in the atlas, sampled for chroma: the biggest |r-g|
// over its opaque pixels. Zero for the monochrome font's rendering.
int glyph_chroma(ImFont* font, unsigned int codepoint) {
    const ImFontGlyph* glyph = font->FindGlyphNoFallback(static_cast<ImWchar>(codepoint));
    if (!glyph) {
        return -1;
    }
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    const int x0 = static_cast<int>(glyph->U0 * static_cast<float>(width));
    const int u1 = static_cast<int>(glyph->U1 * static_cast<float>(width));
    const int v0 = static_cast<int>(glyph->V0 * static_cast<float>(height));
    const int v1 = static_cast<int>(glyph->V1 * static_cast<float>(height));
    int best = 0;
    for (int y = v0; y < v1; ++y) {
        for (int x = x0; x < u1; ++x) {
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

}  // namespace

int main() {
    if (!getenv("VOCEM_EMOJI_BANK")) {
        printf("skip meant to run with VOCEM_EMOJI_BANK pointing at the repo's bank\n");
        return 77;
    }

    ImGui::CreateContext();

    // The frame shows a name with the sushi in it; the next build must carry it.
    vocem::fonts_note_emoji("Reix \xF0\x9F\x8D\xA3");
    check(vocem::ensure_fonts(16.0f, 16.0f), "the first build builds");

    // Refetched after EVERY rebuild, never cached across one: ensure_fonts
    // calls ImFontAtlas::Clear(), which deletes every ImFont, so a pointer
    // taken before a rebuild is freed memory afterwards. This test read one
    // across two rebuilds and passed on stale bytes -- ASan says
    // heap-use-after-free -- which is entry 37 committed inside the test
    // written to hold entry 37's lesson. Address-only reads; the const_cast
    // is the same one fonts_lifecycle makes.
    const auto body_now = [] { return const_cast<ImFont*>(vocem::fonts().body); };
    const auto strong_now = [] { return const_cast<ImFont*>(vocem::fonts().strong); };
    ImFont* body_font = body_now();
    ImFont* strong_font = strong_now();
    const ImFontGlyph* body = body_font->FindGlyphNoFallback(0x1F363);
    const ImFontGlyph* strong = strong_font->FindGlyphNoFallback(0x1F363);
    check(body != nullptr, "the sushi is in the body weight");
    check(strong != nullptr, "and in the heavier one (a merged glyph belongs to its font)");
    check(body && body->Colored, "the body glyph is marked coloured");
    check(strong && strong->Colored, "the strong glyph is marked coloured");

    const int chroma_body = glyph_chroma(body_font, 0x1F363);
    printf("     atlas chroma for the sushi: %d\n", chroma_body);
    check(chroma_body > 40, "and its atlas pixels carry colour, not a white shape");

    // A new emoji arriving later -- somebody renames, somebody joins -- must
    // trigger a rebuild at the same size, exactly as a size change does.
    vocem::fonts_note_emoji("\xF0\x9F\xA5\xA2");  // chopsticks
    check(vocem::ensure_fonts(16.0f, 16.0f), "a new emoji rebuilds at the same size");
    check(body_now()->FindGlyphNoFallback(0x1F962) != nullptr,
          "and the chopsticks arrive");
    check(!vocem::ensure_fonts(16.0f, 16.0f), "with nothing new, the dead band holds");

    // A codepoint the bank does not carry keeps the old behaviour.
    vocem::fonts_note_emoji("\xE2\x80\x99");  // typographic apostrophe
    check(!vocem::ensure_fonts(16.0f, 16.0f), "a non-emoji codepoint rebuilds nothing");

    // A keycap is drawn by one font or by the other, never half by each.
    //
    // `1️⃣` is three codepoints. The digit is below the noter's floor
    // -- deliberately, because colouring U+0032 would put a keycap inside
    // "User 2" -- while U+20E3, the box the sequence draws *around* the digit,
    // is above it and IS in the bank. So the digit came out of the monochrome
    // font and the box came out of the bank: a grey 1 inside a blue tile, on
    // screen, measured in a captured frame. Half an emoji coloured is worse
    // than none.
    vocem::fonts_note_emoji("1\xEF\xB8\x8F\xE2\x83\xA3");
    vocem::ensure_fonts(16.0f, 16.0f);
    const ImFontGlyph* keycap = body_now()->FindGlyphNoFallback(0x20E3);
    check(!(keycap && keycap->Colored),
          "the keycap's box is not coloured on its own, so a keycap is one font throughout");

    // The emoji budget is not spent by things that are not emoji.
    //
    // The seen table remembers a verdict for every codepoint from U+2000 up, in
    // the bank or not, so that a name with a typographic apostrophe costs one
    // bank lookup and not one per frame. That memory shared its size with the
    // atlas's rectangle budget, and the atlas budget is the one with a cost
    // behind it -- so a channel of decorated and CJK names filled it with
    // characters that were never going to be coloured, and every emoji arriving
    // afterwards stayed monochrome for the life of the process. Which is
    // "sometimes they are coloured and sometimes they are not", from a chair.
    //
    // Two hundred ideographs is a plausible evening in a channel with Japanese
    // names in it, and comfortably past the ninety-six the two budgets used to
    // share.
    {
        char utf8[4] = {0};
        for (uint32_t codepoint = 0x4E00; codepoint < 0x4E00 + 200; ++codepoint) {
            utf8[0] = static_cast<char>(0xE0 | (codepoint >> 12));
            utf8[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            utf8[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
            vocem::fonts_note_emoji(utf8);
        }
    }
    vocem::ensure_fonts(16.0f, 16.0f);
    check(vocem::fonts_emoji_status() == nullptr,
          "two hundred non-emoji codepoints are not a full emoji table");
    vocem::fonts_note_emoji("\xF0\x9F\x8D\x95");  // pizza, U+1F355, in the bank
    vocem::ensure_fonts(16.0f, 16.0f);
    const ImFontGlyph* pizza = body_now()->FindGlyphNoFallback(0x1F355);
    check(pizza && pizza->Colored,
          "and an emoji arriving after them is still coloured");

    // The cap. The seen table holds 96 codepoints; the mechanism past it --
    // cap check above the bank lookup, the refusal said out loud by
    // fonts_emoji_status() -- had no witness until here: nothing failed
    // against a build that mishandled the table being full. Emoticons and
    // transport, densely allocated single-codepoint emoji the bank carries.
    {
        char utf8[5] = {0};
        for (uint32_t codepoint = 0x1F600; codepoint <= 0x1F64F + 0x40; ++codepoint) {
            utf8[0] = static_cast<char>(0xF0 | (codepoint >> 18));
            utf8[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
            utf8[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
            utf8[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
            vocem::fonts_note_emoji(utf8);
        }
    }
    check(vocem::fonts_emoji_status() != nullptr,
          "past the cap, the refusal is said rather than silent");
    check(vocem::ensure_fonts(16.0f, 16.0f), "the flood of new emoji rebuilds once");
    // A fresh bank emoji arriving after the cap stays monochrome: noted-and-
    // refused costs no lookup and triggers no rebuild.
    vocem::fonts_note_emoji("\xF0\x9F\x9A\xB2");  // bicycle, U+1F6B2, in the bank
    check(!vocem::ensure_fonts(16.0f, 16.0f),
          "an emoji past the cap triggers no rebuild");
    const ImFontGlyph* bicycle = body_now()->FindGlyphNoFallback(0x1F6B2);
    check(!(bicycle && bicycle->Colored),
          "and it draws monochrome, not with a colour glyph");

    ImGui::DestroyContext();
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

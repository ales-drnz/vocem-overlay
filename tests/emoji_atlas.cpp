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
#include <string.h>

#include "imgui.h"
#include "vocem/emoji_bank.h"
#include "vocem/fonts.h"
#include "vocem/shared_state.h"

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

    // The context the injected paths create: with the fonts module's own
    // atlas, which is what makes the rebuild dead band a promise at all
    // (vocem/fonts.h).
    ImGui::CreateContext(vocem::fonts_atlas());

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
    // reach the atlas at the same size, and ensure_fonts must say so, because
    // its answer is what makes the caller re-upload the font texture. It is
    // FOLDED into reserved space rather than rasterised now; the count that
    // holds that apart is emoji_incremental.cpp's, and what matters here is
    // that the glyph arrives either way.
    vocem::fonts_note_emoji("\xF0\x9F\xA5\xA2");  // chopsticks
    check(vocem::ensure_fonts(16.0f, 16.0f), "a new emoji reaches the atlas at the same size");
    check(body_now()->FindGlyphNoFallback(0x1F962) != nullptr,
          "and the chopsticks arrive");
    check(!vocem::ensure_fonts(16.0f, 16.0f), "with nothing new, the dead band holds");

    // A codepoint the bank does not carry keeps the old behaviour.
    vocem::fonts_note_emoji("\xE2\x80\x99");  // typographic apostrophe
    check(!vocem::ensure_fonts(16.0f, 16.0f), "a non-emoji codepoint rebuilds nothing");

    // A sequence (entry 142): the lime is lemon + ZWJ + green square, and the
    // font reaches its glyph only through a ligature. Noted as codepoints it
    // drew as its parts -- two coloured glyphs and a blank -- which is what the
    // owner saw in a name; prepared, the name carries one key, one glyph wide.
    // Against the module as it stood, fonts_prepare_text does not exist; against
    // a bank with no table beside it the name comes back unchanged and the
    // width check fails.
    {
        char name[vocem::kNameCapacity];
        snprintf(name, sizeof(name), "Fazen\xF0\x9F\x8D\x8B\xE2\x80\x8D\xF0\x9F\x9F\xA9\xF0\x9F\x8D\x8B");
        char raw[vocem::kNameCapacity];
        snprintf(raw, sizeof(raw), "%s", name);
        // The parts first, as the frame before this feature drew them.
        vocem::fonts_note_emoji(raw);
        vocem::ensure_fonts(16.0f, 16.0f);
        const float raw_width = body_now()->CalcTextSizeA(16.0f, 1e9f, 0.0f, raw).x;

        vocem::fonts_prepare_text(name, sizeof(name));
        check(strlen(name) < strlen(raw), "the lime's three codepoints became one key in the name");
        uint32_t key = 0;
        uint32_t codepoints = 0;
        vocem::utf8_each(name, [&](uint32_t cp) {
            ++codepoints;
            if (cp >= vocem::kEmojiSequenceKeyFirst) {
                key = cp;
            }
        });
        check(key != 0 && codepoints == 7, "Fazen, the lime's key and the lemon: seven codepoints");
        check(vocem::ensure_fonts(16.0f, 16.0f), "the key is folded into the atlas like any new emoji");
        const ImFontGlyph* lime_body = body_now()->FindGlyphNoFallback(static_cast<ImWchar>(key));
        const ImFontGlyph* lime_strong = strong_now()->FindGlyphNoFallback(static_cast<ImWchar>(key));
        check(lime_body && lime_body->Colored, "the lime is a coloured glyph in the body weight");
        check(lime_strong && lime_strong->Colored, "and in the heavier one");
        const int lime_chroma = glyph_chroma(body_now(), key);
        printf("     atlas chroma for the lime: %d\n", lime_chroma);
        check(lime_chroma > 40, "with colour in its atlas pixels");
        const float width = body_now()->CalcTextSizeA(16.0f, 1e9f, 0.0f, name).x;
        printf("     name width: %.1f px as parts, %.1f px as one glyph\n", raw_width, width);
        check(width < raw_width - 10.0f, "and the name is about one emoji narrower than as parts");
        // Prepared again -- every frame re-reads the snapshot and prepares it
        // again -- the collapsed name is left exactly as it is.
        char again[vocem::kNameCapacity];
        snprintf(again, sizeof(again), "%s", name);
        vocem::fonts_prepare_text(again, sizeof(again));
        check(strcmp(again, name) == 0, "preparing the prepared name changes nothing");
        // A name with no sequence in it, malformed bytes and all, is untouched.
        char plain[vocem::kNameCapacity];
        snprintf(plain, sizeof(plain), "Re\xC0\x80ix \xF0\x9F\x8D\xA3 \xE2\x80\x99");
        char plain_before[vocem::kNameCapacity];
        memcpy(plain_before, plain, sizeof(plain));
        vocem::fonts_prepare_text(plain, sizeof(plain));
        check(memcmp(plain, plain_before, sizeof(plain)) == 0,
              "a name without a sequence is byte-for-byte what it was, bad bytes included");
        // The keycap as a whole is one coloured glyph now; the box alone below
        // stays uncoloured, so "1" in "User 1" stays a digit.
        char keycap[8];
        snprintf(keycap, sizeof(keycap), "1\xEF\xB8\x8F\xE2\x83\xA3");
        vocem::fonts_prepare_text(keycap, sizeof(keycap));
        uint32_t keycap_key = 0;
        vocem::utf8_each(keycap, [&](uint32_t cp) { keycap_key = cp; });
        check(keycap_key >= vocem::kEmojiSequenceKeyFirst && strlen(keycap) == 4,
              "1 + FE0F + the box collapses into the keycap's key");
        vocem::ensure_fonts(16.0f, 16.0f);
        const ImFontGlyph* keycap_glyph =
            body_now()->FindGlyphNoFallback(static_cast<ImWchar>(keycap_key));
        check(keycap_glyph && keycap_glyph->Colored, "and the keycap draws as one coloured glyph");
    }

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

    // The cap. The atlas takes 96 bank glyphs (the seen table, which only
    // remembers verdicts, holds 512 -- entry 118); the mechanism past it --
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
    check(vocem::ensure_fonts(16.0f, 16.0f), "the flood of new emoji is folded in at once");
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

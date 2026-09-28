// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// One emoji sequence must not cost every drawing process ten megabytes.
//
// ImGui's glyph index is two arrays sized by the highest codepoint a font
// holds -- IndexLookup (2 bytes an entry) and IndexAdvanceX (4) -- and the fold
// grows them to the codepoint it registers (index_folded_glyph). The sequence
// keys were registered at their on-disk numbers, U+F0000 up (entry 142), where
// the rest of the atlas ends at U+1FAFF: the first flag, keycap or ZWJ
// sequence a name showed grew both arrays in both weights to ~984,000 entries.
// Measured against the module as it stood: the two weights' index 1,520 kB
// after the build and 11,530 kB after one flag was folded, and the process's
// resident set 11.4 MB larger -- in every process that draws, for the life of
// the process.
//
// The keys are numbered at run time in the Basic Multilingual Plane's private
// use area now (U+E000..U+F8FF), inside the index the atlas already has; the
// files on disk keep their numbers (the bank and its table are a pair written
// by one run of scripts/make-emoji-bank.py, entry 142). A name that carries a
// codepoint of that area itself must not be looked up as the key of that
// number, so incoming text in the area is replaced -- the second half of this
// test -- except the number of a key already folded, which a text prepared
// twice has to keep.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "imgui.h"
#include "vocem/emoji_bank.h"
#include "vocem/fonts.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

size_t index_bytes(const ImFont* font) {
    return static_cast<size_t>(font->IndexLookup.Capacity) * sizeof(ImU16) +
           static_cast<size_t>(font->IndexAdvanceX.Capacity) * sizeof(float);
}

size_t both_weights_kb() {
    return (index_bytes(vocem::fonts().body) + index_bytes(vocem::fonts().strong)) / 1024;
}

long rss_kb() {
    FILE* status = fopen("/proc/self/status", "r");
    if (!status) {
        return -1;
    }
    char line[256];
    long kb = -1;
    while (fgets(line, sizeof(line), status)) {
        if (strncmp(line, "VmRSS:", 6) == 0) {
            kb = atol(line + 6);
        }
    }
    fclose(status);
    return kb;
}

// The one codepoint of `prepared` that `original` does not contain: the key.
uint32_t key_in(const char* original, const char* prepared) {
    uint32_t key = 0;
    vocem::utf8_each(prepared, [&](uint32_t cp) {
        bool found = false;
        vocem::utf8_each(original, [&](uint32_t other) { found = found || other == cp; });
        if (!found) {
            key = cp;
        }
    });
    return key;
}

}  // namespace

int main() {
    if (!getenv("VOCEM_EMOJI_BANK")) {
        printf("skip meant to run with VOCEM_EMOJI_BANK pointing at the repo's bank\n");
        return 77;
    }
    ImGui::CreateContext(vocem::fonts_atlas());
    vocem::ensure_fonts(32.0f, 16.0f);
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    vocem::fonts_atlas()->GetTexDataAsRGBA32(&pixels, &width, &height);
    const size_t built_kb = both_weights_kb();
    const long built_rss = rss_kb();
    printf("     after the build: index %zu kB (both weights), highest %d, RSS %ld kB\n", built_kb,
           vocem::fonts().body->IndexLookup.Size, built_rss);

    // One flag, the way a frame meets it: noted, folded after the present,
    // collapsed on the next frame.
    static const char kFlag[] = "flag \xF0\x9F\x87\xAE\xF0\x9F\x87\xB9";  // IT
    char name[64];
    snprintf(name, sizeof(name), "%s", kFlag);
    vocem::fonts_prepare_text(name, sizeof(name));
    vocem::ensure_fonts(32.0f, 16.0f);
    snprintf(name, sizeof(name), "%s", kFlag);
    vocem::fonts_prepare_text(name, sizeof(name));
    const uint32_t key = key_in(kFlag, name);
    const ImFontGlyph* glyph = key ? vocem::fonts().body->FindGlyphNoFallback(
                                         static_cast<ImWchar>(key))
                                   : nullptr;
    printf("     the flag's key in the text: U+%X, glyph %s\n", key,
           glyph && glyph->Colored ? "coloured" : "ABSENT");
    check(glyph && glyph->Colored,
          "the flag was folded and collapsed into one coloured key (the positive control)");

    const size_t folded_kb = both_weights_kb();
    const long folded_rss = rss_kb();
    printf("     after one flag folded: index %zu kB (both weights), highest %d, RSS %ld kB "
           "(+%ld)\n", folded_kb, vocem::fonts().body->IndexLookup.Size, folded_rss,
           folded_rss - built_rss);
    check(folded_kb <= built_kb + 64,
          "and the glyph index did not grow for it: the key is inside the index the atlas has");

    // A name that spells the number of a key nobody has shown -- the flag's
    // neighbour, some other sequence's key -- is not taken for that key: the
    // codepoint is replaced before anything reads it, and nothing is folded
    // for it. (The number of a key already folded passes, because a text
    // prepared twice must come out the same; fonts.cpp says what that leaves.)
    if (key < 0x800 || key >= 0xFFFF) {
        check(false, "the key is a three-byte codepoint, so a name can spell its neighbour");
    } else {
        const uint32_t other = key + 1;
        char spelled[16];
        spelled[0] = 'x';
        spelled[1] = static_cast<char>(0xE0 | (other >> 12));
        spelled[2] = static_cast<char>(0x80 | ((other >> 6) & 0x3F));
        spelled[3] = static_cast<char>(0x80 | (other & 0x3F));
        spelled[4] = '\0';
        vocem::fonts_prepare_text(spelled, sizeof(spelled));
        bool in_area = false;
        vocem::utf8_each(spelled, [&](uint32_t cp) {
            in_area = in_area || (cp >= vocem::kSequenceKeyFirst && cp <= vocem::kSequenceKeyLast);
        });
        vocem::ensure_fonts(32.0f, 16.0f);
        const bool folded =
            vocem::fonts().body->FindGlyphNoFallback(static_cast<ImWchar>(other)) != nullptr;
        printf("     a name spelling U+%X: %s in the text, %s in the atlas\n", other,
               in_area ? "still" : "replaced", folded ? "FOLDED" : "nothing");
        check(!in_area && !folded,
              "a name that spells an unshown key's number does not reach that key's picture");
    }
    // And the collapsed name prepared again is the same name.
    char again[64];
    snprintf(again, sizeof(again), "%s", name);
    vocem::fonts_prepare_text(again, sizeof(again));
    check(strcmp(again, name) == 0, "preparing the collapsed name again changes nothing");

    printf("%s\n", failures == 0 ? "all ok" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

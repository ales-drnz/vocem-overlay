// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The symbols a Discord server decorates its names with must have glyphs.
//
// Reported from the field as "?? ... ??" in a toast: the owner's server names
// its channels from a symbol-picker site (coolsymbol.com), and the atlas
// covered 1674 of that site's 3199 distinct codepoints -- every miss a
// question mark in somebody's game. Three symbol fonts (Noto Sans Math,
// Symbols, Symbols 2) and widened Inter/JP subsets took the measured coverage
// to 3100 of 3199; what stays out is out by decision (scripts that need
// shaping, whole CJK, the Private Use Area).
//
// This pins one representative codepoint per block that the change added, in
// BOTH weights (a merged glyph belongs to the font it was merged into --
// entry 26). Against the tree as it shipped 0.1.5, every pinned glyph below
// is absent and the checks fail. The full-corpus number is a measurement,
// not an assertion: the corpus is another site's page and may move; these
// blocks are this project's own decision and stay.

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

struct Pinned {
    unsigned int codepoint;
    const char* what;
};

}  // namespace

int main() {
    ImGui::CreateContext();
    if (!vocem::ensure_fonts(16.0f, 16.0f)) {
        printf("FAIL the atlas did not build\n");
        return 1;
    }

    static const Pinned pins[] = {
        {0x2219, "bullet operator (the channel-name dot that started this)"},
        {0x25AC, "black rectangle (the category bars)"},
        {0x2500, "box drawing light horizontal"},
        {0x2554, "box drawing double down-and-right"},
        {0x2591, "light shade"},
        {0x24B6, "circled latin capital A"},
        {0x2741, "eight-petalled flower (dingbats)"},
        {0x27A4, "black rightwards arrowhead"},
        {0x21F6, "three rightwards arrows"},
        {0x2368, "APL tilde diaeresis"},
        {0x1D400, "mathematical bold capital A (the fancy alphabets)"},
        {0x1D538, "mathematical double-struck capital A"},
        {0x1D670, "mathematical monospace capital A"},
        {0x0259, "latin small letter schwa (IPA)"},
        {0x1D43, "modifier letter small a (superscript alphabets)"},
        {0x1F00, "greek small alpha with psili (Greek Extended)"},
        {0x3042, "hiragana A"},
        {0x30A2, "katakana A"},
        {0x314B, "hangul letter khieukh (compatibility jamo)"},
        {0x3239, "parenthesized ideograph representative (enclosed CJK)"},
        {0xFE4F, "wavy low line (compatibility forms)"},
        {0x0660, "arabic-indic digit zero (digits shape alone)"},
    };

    const ImFont* weights[] = {vocem::fonts().body, vocem::fonts().strong};
    const char* names[] = {"body", "strong"};
    for (int w = 0; w < 2; ++w) {
        for (const Pinned& pin : pins) {
            char message[160];
            snprintf(message, sizeof(message), "%s: U+%04X %s", names[w], pin.codepoint,
                     pin.what);
            check(weights[w] && const_cast<ImFont*>(weights[w])
                                        ->FindGlyphNoFallback(
                                            static_cast<ImWchar>(pin.codepoint)) != nullptr,
                  message);
        }
    }

    if (failures == 0) {
        printf("symbol coverage: every pinned block has its glyph, both weights\n");
        return 0;
    }
    printf("symbol coverage: %d failures\n", failures);
    return 1;
}

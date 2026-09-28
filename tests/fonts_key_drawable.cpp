// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A sequence is never rewritten into a key the atlas cannot draw.
//
// fonts_prepare_text collapses an emoji sequence the bank's table knows -- the
// lime, a flag, a keycap -- into the one key that draws it (entry 142). It did
// that as soon as the bank said the key was wanted, which is BEFORE the fold
// that puts the key's glyph in the atlas. The OpenGL path folds between the
// noting and the drawing; the Vulkan layer notes and draws inside one draw()
// and folds after the present, so the first frame a new sequence appeared in
// drew a key no glyph answered to: ImGui's fallback, a '?'. Measured against
// the module as it stood: the lime's key absent from both weights after the
// rewrite, FindGlyph answering U+003F.
//
// The claim, as the Vulkan frame makes it: after fonts_prepare_text and NO
// ensure_fonts, every key left in the text has a glyph in both weights. Its
// positive control is the other half -- after the fold, the same text IS
// collapsed -- because a module that never rewrote anything would pass the
// first half perfectly.

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

struct Sequence {
    const char* utf8;
    const char* said;
};

// Each a sequence the table carries, none shown to the module before.
const Sequence kSequences[] = {
    {"lime \xF0\x9F\x8D\x8B\xE2\x80\x8D\xF0\x9F\x9F\xA9", "the lime (lemon, ZWJ, green square)"},
    {"flag \xF0\x9F\x87\xAE\xF0\x9F\x87\xB9", "a flag (two regional indicators)"},
    {"key 1\xEF\xB8\x8F\xE2\x83\xA3", "a keycap (1, FE0F, the box)"},
    {"fire \xE2\x9D\xA4\xEF\xB8\x8F\xE2\x80\x8D\xF0\x9F\x94\xA5", "the heart on fire"},
};

// The codepoints of `prepared` that `original` does not contain: the keys the
// rewrite put in. Asked this way rather than by a key range, so the check does
// not depend on where the keys are numbered.
int keys_in(const char* original, const char* prepared, uint32_t* out, int capacity) {
    uint32_t seen[128];
    int seen_count = 0;
    vocem::utf8_each(original, [&](uint32_t cp) {
        if (seen_count < 128) {
            seen[seen_count++] = cp;
        }
    });
    int count = 0;
    vocem::utf8_each(prepared, [&](uint32_t cp) {
        for (int i = 0; i < seen_count; ++i) {
            if (seen[i] == cp) {
                return;
            }
        }
        if (count < capacity) {
            out[count++] = cp;
        }
    });
    return count;
}

}  // namespace

int main() {
    if (!getenv("VOCEM_EMOJI_BANK")) {
        printf("skip meant to run with VOCEM_EMOJI_BANK pointing at the repo's bank\n");
        return 77;
    }
    ImGui::CreateContext(vocem::fonts_atlas());
    vocem::ensure_fonts(16.0f, 16.0f);

    for (const Sequence& sequence : kSequences) {
        char text[64];
        snprintf(text, sizeof(text), "%s", sequence.utf8);
        // The Vulkan frame: noted and drawn, the fold still to come.
        vocem::fonts_prepare_text(text, sizeof(text));
        uint32_t keys[8];
        const int count = keys_in(sequence.utf8, text, keys, 8);
        bool drawable = true;
        for (int i = 0; i < count; ++i) {
            const ImWchar key = static_cast<ImWchar>(keys[i]);
            const ImFontGlyph* body = vocem::fonts().body->FindGlyphNoFallback(key);
            const ImFontGlyph* strong = vocem::fonts().strong->FindGlyphNoFallback(key);
            const ImFontGlyph* drawn = vocem::fonts().body->FindGlyph(key);
            printf("     %s: key U+%X in the text before the fold, glyph %s; drawn as U+%X\n",
                   sequence.said, keys[i], body && strong ? "present" : "ABSENT",
                   drawn ? static_cast<unsigned>(drawn->Codepoint) : 0u);
            drawable = drawable && body && strong;
        }
        char said[160];
        snprintf(said, sizeof(said), "%s: no key the atlas cannot draw is left in the text (%d)",
                 sequence.said, count);
        check(drawable, said);

        // The fold, after the present; then the next frame's copy of the text.
        vocem::ensure_fonts(16.0f, 16.0f);
        char next[64];
        snprintf(next, sizeof(next), "%s", sequence.utf8);
        vocem::fonts_prepare_text(next, sizeof(next));
        const int after = keys_in(sequence.utf8, next, keys, 8);
        bool folded = after == 1;
        for (int i = 0; i < after; ++i) {
            const ImFontGlyph* glyph =
                vocem::fonts().body->FindGlyphNoFallback(static_cast<ImWchar>(keys[i]));
            folded = folded && glyph && glyph->Colored;
        }
        snprintf(said, sizeof(said),
                 "  and the frame after the fold collapses it into one coloured key (%d)", after);
        check(folded, said);
    }

    printf("%s\n", failures == 0 ? "all ok" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

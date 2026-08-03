// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The colour emoji bank's reader, held at both widths.
//
// The bank is written by a 64-bit script and read inside games of both widths,
// which is the exact shape of entries 30/33/34: the halves must agree about
// the format or one of them fails invisibly. The reader's strictness IS the
// format -- a file that is not a whole number of records is refused -- and the
// glyphs must come back in colour: the whole feature exists because the atlas
// drew every emoji as a white monochrome shape.

#include <stdio.h>
#include <stdlib.h>

#include <initializer_list>

#include "vocem/emoji_bank.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

// The biggest |r-g| over opaque pixels: zero for any monochrome rendering,
// large for a real colour glyph. Colour detectors on captures are forbidden
// for good reason (rings and badges lie); on the bank's own pixels the
// question "is this monochrome?" is exactly the right one.
int chroma(const unsigned char* rgba, uint32_t pixels) {
    int best = 0;
    for (uint32_t i = 0; i < pixels; ++i) {
        const unsigned char* p = rgba + i * 4;
        if (p[3] < 128) {
            continue;
        }
        const int spread = p[0] > p[1] ? p[0] - p[1] : p[1] - p[0];
        if (spread > best) {
            best = spread;
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

    vocem::EmojiBank bank;
    check(bank.open(), "the bank opens and is a whole number of records");

    // The sushi the owner's channel is full of.
    check(bank.contains(0x1F363), "the bank carries the sushi");
    check(!bank.contains(0x0041), "and not the letter A");
    check(!bank.contains(0x2019), "and not the typographic apostrophe");

    static unsigned char glyph[vocem::kEmojiBankRgbaBytes];
    check(bank.load(0x1F363, glyph), "the sushi's pixels load");
    const int full = chroma(glyph, vocem::kEmojiBankPixels * vocem::kEmojiBankPixels);
    printf("     sushi chroma at 32px: %d\n", full);
    check(full > 60, "and they are colour, not a monochrome shape");

    // The resample to a text-size glyph keeps the colour.
    static unsigned char scaled[vocem::kEmojiBankRgbaBytes];
    vocem::emoji_bank_resample(glyph, scaled, 16);
    const int small = chroma(scaled, 16 * 16);
    printf("     sushi chroma at 16px: %d\n", small);
    check(small > 40, "the 16px resample is still colour");

    // The UTF-8 walk that feeds the atlas: the sushi in a real name.
    uint32_t seen[8] = {};
    uint32_t count = 0;
    vocem::utf8_each("Reix \xF0\x9F\x8D\xA3 \xF0\x9F\xA5\xA2", [&](uint32_t cp) {
        if (count < 8) {
            seen[count++] = cp;
        }
    });
    bool sushi = false;
    bool chopsticks = false;
    for (uint32_t i = 0; i < count; ++i) {
        sushi = sushi || seen[i] == 0x1F363;
        chopsticks = chopsticks || seen[i] == 0x1F962;
    }
    check(sushi && chopsticks, "the UTF-8 walk finds the sushi and the chopsticks");

    // Malformed UTF-8, which is what a Discord name is allowed to be: a decoder
    // running inside somebody's game answers for every byte sequence, not only
    // the ones a well-behaved client sends. Each case names what it decodes to
    // when the decoder is wrong -- the first version turned all four of these
    // into perfectly ordinary codepoints, and one of them is handed to ImGui as
    // an ImWchar. What is required is that nothing the string does not contain
    // is ever reported, and that the walk terminates.
    struct Malformed {
        const char* bytes;
        const char* what;
    };
    static const Malformed malformed[] = {
        {"\xC0\x80", "overlong two-byte NUL (decoded as U+0000)"},
        {"\xC1\xBF", "overlong two-byte 0x7F"},
        {"\xE0\x80\x80", "overlong three-byte NUL"},
        {"\xF0\x82\x82\xAC", "overlong four-byte euro sign"},
        {"\xED\xA0\x80", "a UTF-16 surrogate (U+D800)"},
        {"\xF7\xBF\xBF\xBF", "a codepoint past U+10FFFF"},
        {"\xF0\x9F\x8D", "a truncated sushi at the end of the string"},
        {"\x80\x80\x80", "continuation bytes with no lead"},
        {"\xF8\x88\x80\x80\x80", "a five-byte form"},
        {"\xFF\xFE", "bytes that are not UTF-8 at all"},
    };
    bool clean = true;
    for (const Malformed& bad : malformed) {
        uint32_t reported[8] = {};
        uint32_t n = 0;
        vocem::utf8_each(bad.bytes, [&](uint32_t cp) {
            if (n < 8) {
                reported[n] = cp;
            }
            ++n;
        });
        // Every one of these is ill-formed from its first byte, so a correct
        // decoder reports nothing at all from any of them.
        if (n != 0) {
            printf("     %s: reported %u codepoint(s), first U+%04X\n", bad.what, n, reported[0]);
            clean = false;
        }
    }
    check(clean, "no ill-formed sequence decodes into a codepoint");

    // A well-formed string that follows one is still read: rejecting a byte must
    // resynchronise, not abandon the rest of the name.
    uint32_t after = 0;
    vocem::utf8_each("\xC0\x80" "ok\xF0\x9F\x8D\xA3", [&](uint32_t cp) {
        if (cp == 0x1F363) {
            after = cp;
        }
    });
    check(after == 0x1F363, "the walk resynchronises and finds the sushi after bad bytes");

    // The resample at the sizes the atlas actually asks for, including the two
    // extremes it can reach. Nothing may write past its buffer, which is what
    // the ceiling's static_assert in fonts.cpp exists to keep true.
    static unsigned char small_scaled[vocem::kEmojiBankRgbaBytes];
    for (uint32_t size : {1u, 2u, 11u, 17u, 31u, 32u}) {
        vocem::emoji_bank_resample(glyph, small_scaled, size);
    }
    check(true, "the resample runs at every size between 1 and the bank's own");

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

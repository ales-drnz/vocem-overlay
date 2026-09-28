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
#include <string>

#include "vocem/emoji_bank.h"
#include "vocem_check.h"

using vocem_test::check;
using vocem_test::failures;

namespace {

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
    //
    // Measured, not merely run: this was `check(true, ...)`, which only a crash
    // could fail, and a write past the end of a stack or static buffer does not
    // crash (DESIGN 193). The output sits in a buffer with a guard behind it,
    // filled with a sentinel before every size: whatever the resample writes
    // past size*size*4 bytes is in the guard afterwards, and a resample that
    // writes nothing leaves its own region all sentinel. At one pixel the answer
    // is known exactly -- the whole glyph's mean, channel by channel -- and at
    // the bank's own size it is the glyph itself.
    constexpr size_t kGuard = 256;
    static unsigned char small_scaled[vocem::kEmojiBankRgbaBytes + kGuard];
    constexpr unsigned char kSentinel = 0xA5;
    bool within = true;
    bool wrote = true;
    for (uint32_t size : {1u, 2u, 11u, 17u, 31u, 32u}) {
        memset(small_scaled, kSentinel, sizeof(small_scaled));
        vocem::emoji_bank_resample(glyph, small_scaled, size);
        const size_t used = static_cast<size_t>(size) * size * 4;
        for (size_t i = used; i < sizeof(small_scaled); ++i) {
            within = within && small_scaled[i] == kSentinel;
        }
        bool any = false;
        for (size_t i = 0; i < used; ++i) {
            any = any || small_scaled[i] != kSentinel;
        }
        wrote = wrote && any;
        if (size == 1) {
            for (int channel = 0; channel < 4; ++channel) {
                uint64_t sum = 0;
                for (uint32_t p = 0; p < vocem::kEmojiBankPixels * vocem::kEmojiBankPixels; ++p) {
                    sum += glyph[p * 4 + channel];
                }
                const uint64_t mean = sum / (vocem::kEmojiBankPixels * vocem::kEmojiBankPixels);
                check(small_scaled[channel] == mean,
                      "at one pixel the resample is the glyph's mean, channel by channel");
            }
        }
        if (size == vocem::kEmojiBankPixels) {
            check(memcmp(small_scaled, glyph, vocem::kEmojiBankRgbaBytes) == 0,
                  "at the bank's own size the resample is the glyph itself");
        }
    }
    check(within, "the resample writes nothing past size*size pixels, at every size from 1 to "
                  "the bank's own");
    check(wrote, "and writes its own pixels at every one of them");

    // The sequence table beside the bank (entry 142). The lime is 🍋 + ZWJ +
    // 🟩, and the font reaches its glyph only through a ligature: without the
    // table it drew as a lemon beside a green square, which is what the owner
    // saw. Against the bank shipped in 0.1.8 -- no table beside it -- the
    // count is zero and every check on it fails.
    printf("     %u sequences in the table beside the bank%s%s\n", bank.sequence_count(),
           bank.sequences_reason() ? ": " : "", bank.sequences_reason() ? bank.sequences_reason() : "");
    check(bank.sequences_reason() == nullptr, "the table beside the bank loads without complaint");
    check(bank.sequence_count() > 4000, "and holds the font's sequences, thousands of them");
    uint32_t used = 0;
    const uint32_t lime_then_lemon[] = {0x1F34B, 0x200D, 0x1F7E9, 0x1F34B};
    const uint32_t lime = bank.sequence_key(lime_then_lemon, 4, &used);
    check(lime >= vocem::kEmojiSequenceKeyFirst && used == 3,
          "the lime is one key covering its three codepoints, and the lemon after it is not swept up");
    check(bank.contains(lime), "the bank carries the lime's glyph under that key");
    check(bank.load(lime, glyph) && chroma(glyph, vocem::kEmojiBankPixels * vocem::kEmojiBankPixels) > 60,
          "and its pixels are colour");
    check(bank.sequence_key(lime_then_lemon + 3, 1, &used) == 0 && used == 0,
          "a lemon alone is no sequence");
    const uint32_t two_lemons[] = {0x1F34B, 0x1F34B};
    check(bank.sequence_key(two_lemons, 2, &used) == 0, "two lemons with no joiner are two lemons");
    const uint32_t dangling[] = {0x1F34B, 0x200D};
    check(bank.sequence_key(dangling, 2, &used) == 0, "a lemon with a dangling joiner is left alone");
    const uint32_t flags[] = {0x1F1EE, 0x1F1F9, 0x1F1EB, 0x1F1F7};  // 🇮🇹🇫🇷
    const uint32_t italy = bank.sequence_key(flags, 4, &used);
    check(italy >= vocem::kEmojiSequenceKeyFirst && used == 2,
          "a flag is its pair of regional indicators, and the next flag's first letter is not taken");
    check(bank.sequence_key(flags + 2, 2, &used) != italy && used == 2, "and the second flag is another key");
    // The presentation selector: the font's cmap has none, a name has plenty.
    const uint32_t keycap[] = {0x31, 0xFE0F, 0x20E3};
    const uint32_t bare_keycap[] = {0x31, 0x20E3};
    const uint32_t one = bank.sequence_key(keycap, 3, &used);
    check(one >= vocem::kEmojiSequenceKeyFirst && used == 3,
          "1 + FE0F + keycap is the keycap's key, the selector consumed inside the match");
    check(bank.sequence_key(bare_keycap, 2, &used) == one && used == 2, "and without the selector it is the same key");
    const uint32_t digits[] = {0x31, 0x32};
    check(bank.sequence_key(digits, 2, &used) == 0, "while two digits stay two digits");
    const uint32_t heart_on_fire[] = {0x2764, 0xFE0F, 0x200D, 0x1F525};
    check(bank.sequence_key(heart_on_fire, 4, &used) >= vocem::kEmojiSequenceKeyFirst && used == 4,
          "a heart on fire spelled with the selector, as Discord spells it, is one key");
    const uint32_t trailing[] = {0x1F1EE, 0x1F1F9, 0xFE0F, 0x41};
    check(bank.sequence_key(trailing, 4, &used) == italy && used == 3,
          "a selector right after a match goes with it, and the letter after does not");
    const uint32_t family[] = {0x1F468, 0x200D, 0x1F469, 0x200D, 0x1F467, 0x200D, 0x1F466};
    check(bank.sequence_key(family, 7, &used) >= vocem::kEmojiSequenceKeyFirst && used == 7,
          "a family of four is one key seven codepoints long");
    const uint32_t selector_first[] = {0xFE0F, 0x1F34B};
    check(bank.sequence_key(selector_first, 2, &used) == 0, "a selector starts nothing");

    // The reader's strictness on the table is the format, as it is on the
    // bank: a table that is not a whole number of records, or none at all, is
    // said and not guessed at, and the bank beside it still works. Scratch
    // copies, tried LAST: every bank in a process shares the table's storage.
    {
        char root[] = "/tmp/vocem-emoji-bank-XXXXXX";
        if (mkdtemp(root)) {
            const std::string dir = root;
            const std::string here = getenv("VOCEM_EMOJI_BANK");
            const std::string table_here =
                here.substr(0, here.find_last_of('/') + 1) + vocem::kBridgeEmojiSequencesName;
            const std::string bank_copy = dir + "/emoji_bank.rgba";
            const std::string table_copy = dir + "/" + vocem::kBridgeEmojiSequencesName;
            std::string cmd = "ln -s '" + here + "' '" + bank_copy + "'";
            const bool linked = system(cmd.c_str()) == 0;
            setenv("VOCEM_EMOJI_BANK", bank_copy.c_str(), 1);
            {
                vocem::EmojiBank without;
                check(linked && without.open(), "a bank with no table beside it still opens");
                check(without.sequence_count() == 0 && without.sequences_reason() != nullptr,
                      "and says there is no table rather than staying silent");
                printf("     %s\n", without.sequences_reason() ? without.sequences_reason() : "(no reason)");
            }
            cmd = "head -c 100 '" + table_here + "' > '" + table_copy + "'";
            if (system(cmd.c_str()) == 0) {
                vocem::EmojiBank truncated;
                check(truncated.open(), "a bank beside a truncated table still opens");
                check(truncated.sequence_count() == 0 && truncated.sequences_reason() != nullptr,
                      "and a table that is not a whole number of records is refused, with the reason said");
                printf("     %s\n", truncated.sequences_reason() ? truncated.sequences_reason() : "(no reason)");
            }
            setenv("VOCEM_EMOJI_BANK", here.c_str(), 1);
            cmd = "rm -rf '" + dir + "'";
            system(cmd.c_str());
        }
    }

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

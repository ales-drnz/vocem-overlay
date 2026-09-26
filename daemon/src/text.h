// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What this daemon does to a piece of text from Discord before it trusts it.
//
// Two things, which is one more than the file's name suggests and is said here
// rather than left to be found: `sanitise_text` takes characters OUT of a
// string before it is shown, logged or written into the segment, and
// `json_within` at the bottom refuses a whole message before it is
// parsed. What they have in common is where they live rather than what they
// do -- header-only and dependency-free, so a test compiles either alone,
// which is the reason entry 133 put the first one here and entry 183 the
// second.
//
// The characters first. Two kinds go, for two reasons:
//
//   * The bidirectional formatting characters. Discord wraps every name it
//     interpolates into a sentence in Unicode's directional isolates, so that
//     a right-to-left name cannot scramble the text around it: a notification
//     title arrives as "⁨Lele⁩ (⁨Chilling⁩, ⁨Canali vocali⁩)". Those characters
//     are invisible by definition and no font in the overlay's atlas has a
//     glyph for one -- measured: Inter, Inter SemiBold and Noto Sans JP have
//     none in their cmap, and ImGui draws its FallbackChar, which is '?'. Six
//     isolates in that title were six question marks in the box on the
//     owner's screen (DESIGN entry 66). They are removed rather than given
//     blank glyphs because the atlas lives inside somebody's game, and because
//     they exist to drive the bidirectional algorithm, which ImGui does not
//     implement: here they can never do anything but take up a glyph. Listed
//     by hand rather than by Unicode category: "every default-ignorable code
//     point" would sweep up the zero-width joiner and the variation selectors
//     that entry 27 put in the atlas on purpose.
//   * The C0 controls and DEL. A newline in a nickname reaches the panel as a
//     second row and the daemon's own log as a second line -- and the journal
//     is what the Debug section shows, so a name Discord let somebody choose
//     was a way to write lines into it. The Flatpak bridge already refuses
//     control characters in what a sandbox writes (`printable()` in
//     flatpak_bridge.cpp); names, channel names and titles come from the other
//     direction and had no such rule (entry 133). Tab included: nothing here
//     is columnar. The C1 controls, U+0080..U+009F, for the same reason one
//     block up: U+009B is the single-character CSI, an escape sequence to a
//     terminal that honours 8-bit controls, and `vocem` prints names straight
//     to a terminal; U+0085 is NEXT LINE. Only below U+0020 went through
//     0.1.10 (the review's c1 probe). And U+2028/U+2029, the line and
//     paragraph separators, which are line breaks by Unicode's own definition
//     (UAX #14 class BK): a newline spelled in three bytes is still one.
//
// This lived inside main.cpp's anonymous namespace as `without_bidi_marks`,
// where nothing could test it; entry 66's second defect had no test of its
// own for exactly that reason.

#ifndef VOCEM_DAEMON_TEXT_H
#define VOCEM_DAEMON_TEXT_H

#include <cstddef>
#include <cstdint>
#include <string>

namespace vocem {

// True for the code points that go. U+061C ARABIC LETTER MARK; U+200E..U+200F
// the two directional marks; U+202A..U+202E the embeddings and overrides;
// U+2066..U+2069 the isolates; U+2028..U+2029 the line and paragraph
// separators; everything below U+0020, U+007F, and the C1 controls
// U+0080..U+009F.
inline bool text_drops(uint32_t code) {
    static const uint32_t kMarks[] = {0x061C, 0x200E, 0x200F, 0x2028, 0x2029, 0x202A, 0x202B,
                                      0x202C, 0x202D, 0x202E, 0x2066, 0x2067, 0x2068, 0x2069};
    if (code < 0x20 || (code >= 0x7F && code <= 0x9F)) {
        return true;
    }
    for (uint32_t mark : kMarks) {
        if (code == mark) {
            return true;
        }
    }
    return false;
}

// The text with those characters removed. Not a UTF-8 validator: the daemon's
// JSON parser already refuses a message whose strings are not valid UTF-8,
// so what arrives here is well-formed, and a truncated sequence -- which the
// parser cannot produce -- is copied through byte by byte rather than read
// past the end.
inline std::string sanitise_text(const std::string& source) {
    std::string out;
    out.reserve(source.size());
    for (size_t i = 0; i < source.size();) {
        const unsigned char lead = static_cast<unsigned char>(source[i]);
        size_t length = 1;
        uint32_t code = lead;
        if ((lead & 0xE0) == 0xC0) {
            length = 2;
            code = lead & 0x1Fu;
        } else if ((lead & 0xF0) == 0xE0) {
            length = 3;
            code = lead & 0x0Fu;
        } else if ((lead & 0xF8) == 0xF0) {
            length = 4;
            code = lead & 0x07u;
        }
        // A truncated sequence, or a byte that starts none (a stray
        // continuation byte): copy it and move on rather than read past the
        // end, or read the byte's value as a code point -- 0x81 alone is not
        // U+0081. Nothing here is the place to repair broken UTF-8.
        if (i + length > source.size() || (length == 1 && lead >= 0x80)) {
            out.push_back(source[i]);
            ++i;
            continue;
        }
        for (size_t k = 1; k < length; ++k) {
            code = (code << 6) | (static_cast<unsigned char>(source[i + k]) & 0x3Fu);
        }
        if (!text_drops(code)) {
            out.append(source, i, length);
        }
        i += length;
    }
    return out;
}

// Whether a JSON text is within a shape this daemon can afford to parse:
// nested at most `depth_ceiling` levels and made of at most `token_ceiling`
// structural tokens (every `[`, `{`, `,` and `:` outside a string), without
// parsing it.
//
// `json::parse(raw, nullptr, false)` bounds parse ERRORS and nothing else:
// nlohmann has no depth or element limit, so the cost of a message is the
// peer's to choose, up to `kMaxMessage` -- 8 MiB, the reassembly cap entry 72
// put on a frame. Measured through nlohmann alone, 8 MiB of each shape:
// **nested `[` 624 MB** (entry 183), and flat, which a depth ceiling cannot
// see (entry 199): `[{},{},...]` **221 MB**, strings 171 MB, `{"a":0}`
// objects 168 MB, empty arrays 136 MB, numbers 65 MB. The unit says
// `MemoryMax=128M`, and measured under exactly that with this machine's zram
// swap, the parse is not killed: it survives with ~436 MB pushed into swap,
// the daemon stalled while it happens. Without swap it is a SIGKILL
// (`MemorySwapMax=0`: exit 137, measured) -- which runs no destructor and
// leaves `/dev/shm/vocem-<uid>`, the note's words and every Flatpak mirror
// behind, the leftover entry 81 forbids.
//
// The ceilings come from what Discord sends: the largest of the 539 real
// payloads phase 0b kept (a GET_CHANNEL with its messages) is 17 KB, 1342
// tokens and 8 levels deep. 64 levels and 131072 tokens are far above that and
// bound the worst shape to ~10 MB.
//
// One pass over the bytes, no allocation, stopping at a ceiling: strings are
// skipped so a name full of brackets or commas is not counted, and `\`
// escapes the next byte inside one.
constexpr int kJsonDepthCeiling = 64;
constexpr size_t kJsonTokenCeiling = 131072;

inline bool json_within(const std::string& text, int depth_ceiling, size_t token_ceiling) {
    int depth = 0;
    size_t tokens = 0;
    bool in_string = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (in_string) {
            if (c == '\\') {
                ++i;  // whatever follows is data, including a quote
            } else if (c == '"') {
                in_string = false;
            }
            continue;
        }
        if (c == '"') {
            in_string = true;
            continue;
        }
        if (c == '[' || c == '{') {
            if (++depth > depth_ceiling) {
                return false;
            }
        } else if (c == ']' || c == '}') {
            --depth;
            continue;
        } else if (c != ',' && c != ':') {
            continue;
        }
        if (++tokens > token_ceiling) {
            return false;
        }
    }
    return true;
}

}  // namespace vocem

#endif  // VOCEM_DAEMON_TEXT_H

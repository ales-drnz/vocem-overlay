// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What this daemon does to a piece of text from Discord before it trusts it.
//
// Two things: `sanitise_text` takes characters OUT of a string before it is
// shown, logged or written into the segment, and `json_within` at the bottom
// refuses a whole message before it is parsed. Both are header-only and
// dependency-free so a test compiles either alone.
//
// Two kinds of character go:
//
//   * The bidirectional formatting characters. Discord wraps every name it
//     interpolates into a sentence in directional isolates ("⁨Lele⁩ (⁨Chilling⁩,
//     ⁨Canali vocali⁩)"); no font in the overlay's atlas has a glyph for them,
//     so ImGui draws its '?' fallback, and ImGui does not implement the
//     bidirectional algorithm they exist to drive. Listed by hand
//     rather than by Unicode category: "every default-ignorable code point"
//     would sweep up the zero-width joiner and the variation selectors that
//     the atlas carries on purpose.
//   * The C0 controls, DEL and the C1 controls. A newline in a nickname would
//     be a second row in the panel and a second line in the daemon's journal,
//     which the Debug section shows. Tab included: nothing here is
//     columnar. U+009B is the single-character CSI and `vocem` prints names
//     straight to a terminal; U+0085 is NEXT LINE. U+2028/U+2029 go too: they
//     are line breaks by Unicode's own definition (UAX #14 class BK).

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

// The text with those characters removed. Not a UTF-8 repairer, but not blind
// to broken UTF-8 either: main.cpp logs the `name=` of a peer's /.flatpak-info
// through here, a file any unprivileged bwrap can write. A sequence is decoded
// only when its continuation bytes are 10xxxxxx and all there; otherwise its
// lead is a stray byte, copied through alone, and the bytes after it are looked
// at on their own -- so a control byte hiding where a continuation should be
// ("\xC3\x1B") is dropped like any other.
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
        // A truncated sequence, a byte that starts none (a stray continuation
        // byte), or a lead whose continuations are not continuation bytes:
        // copy the one byte and move on, rather than read past the end, read
        // the byte's value as a code point -- 0x81 alone is not U+0081 -- or
        // swallow the next bytes whatever they are. Nothing here is the place
        // to repair broken UTF-8.
        bool whole = i + length <= source.size() && !(length == 1 && lead >= 0x80);
        for (size_t k = 1; whole && k < length; ++k) {
            const unsigned char next = static_cast<unsigned char>(source[i + k]);
            whole = (next & 0xC0) == 0x80;
            code = (code << 6) | (next & 0x3Fu);
        }
        if (!whole) {
            out.push_back(source[i]);
            ++i;
            continue;
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
// nlohmann has no depth or element limit, so without this the cost of a
// message is the peer's to choose, up to `kMaxMessage` (8 MiB): nested `[`
// costs hundreds of MB to parse, and flat shapes nearly as much
// (entries 183 and 199). Under the unit's `MemoryMax=128M` that is a stall in swap or a
// SIGKILL, which runs no destructor and leaves the segment and every Flatpak
// mirror behind.
//
// The ceilings come from what Discord sends: the largest real payload seen is
// 17 KB, 1342 tokens and 8 levels deep. 64 levels and 131072 tokens are far
// above that and bound the worst shape to ~10 MB.
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

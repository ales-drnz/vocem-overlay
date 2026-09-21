// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What this daemon does to a piece of text from Discord before it trusts it.
//
// Two things, which is one more than the file's name suggests and is said here
// rather than left to be found: `sanitise_text` takes characters OUT of a
// string before it is shown, logged or written into the segment, and
// `json_depth_within` at the bottom refuses a whole message before it is
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
//     is columnar.
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
// U+2066..U+2069 the isolates; and everything below U+0020 plus U+007F.
inline bool text_drops(uint32_t code) {
    static const uint32_t kMarks[] = {0x061C, 0x200E, 0x200F, 0x202A, 0x202B, 0x202C,
                                      0x202D, 0x202E, 0x2066, 0x2067, 0x2068, 0x2069};
    if (code < 0x20 || code == 0x7F) {
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
        if (i + length > source.size()) {
            // A truncated sequence: copy the byte and move on rather than read
            // past the end. Nothing here is the place to repair broken UTF-8.
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

// How deeply a JSON text nests, up to `ceiling`, without parsing it.
//
// `json::parse(raw, nullptr, false)` bounds parse ERRORS and nothing else:
// nlohmann has no depth or element limit, so the cost of a message is the
// peer's to choose. Measured twice: **8 MiB of `[`** -- which is exactly
// `kMaxMessageBytes`, the reassembly cap entry 72 put on a frame -- costs
// **624 MB** of resident memory and then comes back `discarded`, because it is
// invalid. The unit says `MemoryMax=128M`, so what really happens is the
// cgroup killing the daemon: a SIGKILL runs no destructor, which leaves
// `/dev/shm/vocem-<uid>`, the note's words and every Flatpak mirror behind --
// the leftover entry 81 forbids -- and `Restart=on-failure` then does it
// again. Entry 72 capped the reassembly buffer and never re-derived the cap
// for what happens to the bytes next.
//
// One pass over the bytes, no allocation, stopping at the ceiling: strings are
// skipped so a name full of brackets is not counted, and `\` escapes the next
// byte inside one. Discord's RPC messages nest about five levels; the ceiling
// is far above anything the client sends and far below anything that costs
// memory worth noticing.
inline bool json_depth_within(const std::string& text, int ceiling) {
    int depth = 0;
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
        } else if (c == '[' || c == '{') {
            if (++depth > ceiling) {
                return false;
            }
        } else if (c == ']' || c == '}') {
            --depth;
        }
    }
    return true;
}

}  // namespace vocem

#endif  // VOCEM_DAEMON_TEXT_H

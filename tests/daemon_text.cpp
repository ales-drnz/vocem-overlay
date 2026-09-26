// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What the daemon takes out of a name before it reaches the segment, the log
// and the journal (daemon/src/text.h).
//
// Entry 66's second defect -- Discord's directional isolates drawn as question
// marks -- was fixed in a function inside main.cpp's anonymous namespace, where
// no test could reach it; daemon_moved drives it through the whole daemon, which
// is a test of the daemon and not of the rule. And the rule was one item short:
// it removed the bidirectional marks and nothing else, so a newline in a
// nickname reached the panel as a second row and the daemon's journal -- which
// the Debug section shows -- as a second line (entry 133). Both halves here,
// against the function alone.

#include <cstdio>
#include <string>

#include "text.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

}  // namespace

int main() {
    using vocem::json_within;
    using vocem::sanitise_text;

    // Entry 66: the title as the owner's journal recorded it, isolates and all.
    // Split literals: "\xA8C" would read as one hex escape.
    check(sanitise_text("\xE2\x81\xA8" "Lele\xE2\x81\xA9 (\xE2\x81\xA8" "Chilling\xE2\x81\xA9)") ==
              "Lele (Chilling)",
          "the isolates Discord wraps every interpolated name in are removed");
    check(sanitise_text("a\xE2\x80\x8E" "b\xE2\x80\x8F" "c") == "abc",
          "and the two directional marks");
    check(sanitise_text("x\xE2\x80\xAAy\xE2\x80\xAE" "z") == "xyz", "and the embeddings and overrides");
    check(sanitise_text("\xD8\x9C" "name") == "name", "and the Arabic letter mark");

    // Entry 133: the control characters.
    check(sanitise_text("first\nsecond") == "firstsecond", "a newline in a name is not a second row");
    check(sanitise_text("a\rb\tc\x1b[31md\x7f") == "abc[31md",
          "nor is a carriage return, a tab, an escape or DEL anything");
    check(sanitise_text("name = other\n") == "name = other",
          "a name shaped like a record line stays one line");

    // What stays.
    check(sanitise_text("Zo\xC3\xAB \xF0\x9F\x8E\xAE") == "Zo\xC3\xAB \xF0\x9F\x8E\xAE",
          "accents and emoji are untouched");
    check(sanitise_text("\xE2\x80\x8D\xEF\xB8\x8F") == "\xE2\x80\x8D\xEF\xB8\x8F",
          "the zero-width joiner and the variation selector -- which entry 27 put in the atlas "
          "on purpose -- are kept");
    // The C1 controls, U+0080..U+009F, two bytes each in UTF-8. U+009B is the
    // one-character CSI -- "\xC2\x9B" "31m" is ESC [ 31 m to a terminal that
    // honours 8-bit controls -- and the CLI prints names raw to a terminal;
    // U+0085 is NEXT LINE. Kept through 0.1.10 (the review's c1 probe printed
    // c2 9b and c2 85 back out), because only below U+0020 was dropped.
    check(sanitise_text("a\xC2\x9B" "31mb\xC2\x85" "c") == "a31mbc",
          "the C1 controls go: the one-byte CSI and NEXT LINE are not text");
    check(sanitise_text("\xC2\x80\xC2\x9F") == "", "all of U+0080..U+009F, both ends");
    check(sanitise_text("\xC2\xA0" "x") == "\xC2\xA0" "x",
          "and the no-break space just past them is kept");
    check(sanitise_text("a\x85" "b") == "a\x85" "b",
          "a stray byte 0x85 is copied through, not read as the code point U+0085");
    // U+2028 and U+2029, the line and paragraph separators: a line break by
    // Unicode's own definition (UAX #14 class BK), which a name has no more
    // business carrying than a newline.
    check(sanitise_text("d\xE2\x80\xA8" "e\xE2\x80\xA9" "f") == "def",
          "and so do the line and paragraph separators");
    check(sanitise_text("") == "", "nothing in, nothing out");
    check(sanitise_text("\xE2\x81") == "\xE2\x81",
          "a truncated sequence is copied through, never read past");

    // --- what shape a message may have before it is parsed at all ---------
    //
    // nlohmann bounds neither depth nor element count, so what a message costs
    // is the peer's to choose, up to the 8 MiB kMaxMessage lets through: 624
    // MB nested, 221 MB as a flat array of empty objects, against a unit whose
    // MemoryMax is 128M (text.h has every shape's figure and what the cgroup
    // then does). The scan below is what stops either reaching the parser, at
    // the ceilings the daemon itself uses.
    const int depth = vocem::kJsonDepthCeiling;
    const size_t tokens = vocem::kJsonTokenCeiling;
    check(json_within(R"({"cmd":"DISPATCH","evt":"SPEAKING_START",)"
                      R"("data":{"user_id":"1"}})", depth, tokens),
          "a real RPC message is nowhere near either ceiling");
    check(json_within(std::string(depth, '[') + std::string(depth, ']'), depth, tokens),
          "a message exactly at the depth ceiling is allowed");
    check(!json_within(std::string(depth + 1, '[') + std::string(depth + 1, ']'), depth, tokens),
          "and one level past it is not");
    check(!json_within(std::string(8u * 1024 * 1024, '['), depth, tokens),
          "8 MiB of nesting is refused without being parsed");
    // Flat, which the depth ceiling cannot see (entry 199).
    std::string flat = "[";
    while (flat.size() + 4 < 8u * 1024 * 1024) {
        flat += "{},";
    }
    flat += "{}]";
    check(!json_within(flat, depth, tokens),
          "8 MiB of empty objects in one flat array is refused without being parsed");
    // The largest message phase 0b ever saw from Discord: a GET_CHANNEL with
    // its messages, 1342 tokens and 8 levels. Built to that count, and
    // allowed with the ceiling a hundred times over it.
    std::string channel = "{\"messages\":[";
    for (int i = 0; i < 1342 / 4; ++i) {
        channel += "{\"a\":1,\"b\":2},";
    }
    channel += "{}]}";
    check(json_within(channel, depth, tokens), "and Discord's largest message measured is allowed");
    std::string at = "[";
    at.reserve(2 * tokens + 4);
    for (size_t i = 0; i + 1 < tokens; ++i) {
        at += "0,";
    }
    at += "0]";
    check(json_within(at, depth, tokens), "a message exactly at the token ceiling is allowed");
    check(!json_within("[0," + at.substr(1), depth, tokens), "and one token past it is not");
    // A name is data, and a name full of brackets is a name. This is why the
    // scan skips strings rather than counting every byte that looks like
    // structure -- a channel decorated from a symbol site (entry 128) is
    // exactly the shape that would trip a naive counter.
    check(json_within(R"({"nick":"[[[[[[[[[[ hello, ]]]]]]]]]]:"})", 4, 2),
          "brackets, commas and colons inside a name are not structure");
    check(json_within(R"({"n":"say \" [[[[["})", 4, 2),
          "and a backslash-escaped quote does not end the string early");

    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

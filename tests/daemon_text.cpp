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
    check(sanitise_text("") == "", "nothing in, nothing out");
    check(sanitise_text("\xE2\x81") == "\xE2\x81",
          "a truncated sequence is copied through, never read past");

    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

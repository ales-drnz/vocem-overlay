// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A name the lists cannot hold is refused, not stored as two other names.
//
// hidden_apps and shown_apps are comma-separated, and every reader -- the
// overlay inside a game at both widths, the window, vocem-why -- splits on the
// comma and trims each entry. list_with() stored whatever name it was given,
// so a process called "Foo, Bar" (a comm or an executable name may carry any
// byte but '/' and NUL) went into hidden_apps as "Foo, Bar": the switch for
// "Foo, Bar" did nothing -- listed("Foo, Bar") is false -- and the two
// applications called Foo and Bar were hidden instead. Measured on 0.1.10:
// list_with("", "Foo, Bar") = "Foo, Bar", listed(.., "Foo, Bar") = 0,
// listed(.., "Foo") = listed(.., "Bar") = 1. A name with spaces around it has
// the same fault in miniature: stored as " Foo", read back as "Foo".
//
// Refused rather than escaped: an escape would have to be read the same way by
// the overlay already inside every running game, at both widths, and by every
// older overlay still installed -- which it is not. Run at both widths because
// the injected code carries this header.

#include <cstdio>
#include <string>

#include "vocem/apps.h"

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
    const std::string comma = vocem::list_with("", "Foo, Bar");
    std::printf("--  list_with(\"\", \"Foo, Bar\") = \"%s\"\n", comma.c_str());
    check(!vocem::listed(comma, "Foo"), "adding \"Foo, Bar\" does not list Foo");
    check(!vocem::listed(comma, "Bar"), "nor Bar");
    check(comma.empty(), "the list is left as it was");
    check(vocem::list_with("Game", "Foo,Bar") == "Game", "a comma without a space is refused too");
    check(vocem::list_with("Game", " Foo") == "Game", "a name with a space before it is refused");
    check(vocem::list_with("Game", "Foo\t") == "Game", "and one with a tab after it");
    check(!vocem::list_entry_fits("Foo, Bar") && !vocem::list_entry_fits("") &&
              vocem::list_entry_fits("Foo Bar"),
          "list_entry_fits says which names can be held");

    // What still works: the ordinary names, spaces inside included.
    const std::string ordinary = vocem::list_with(vocem::list_with("", "Game"), "Foo Bar");
    check(ordinary == "Game,Foo Bar", "ordinary names are added");
    check(vocem::listed(ordinary, "Foo Bar") && vocem::listed(ordinary, "Game"),
          "and read back as themselves");
    check(vocem::list_with(ordinary, "Game") == ordinary, "and once");
    check(vocem::list_without(ordinary, "Game") == "Foo Bar", "and taken out");

    std::printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

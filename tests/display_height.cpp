// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The DRM reader behind the overlay's size, against a fabricated /sys tree.
//
// display_height_under() is what decides how large the overlay is on every
// machine, and the real /sys/class/drm on the machine the tests run on is
// whatever it is -- one connected output today, another tomorrow. The parser is
// held here against a tree built to order: disconnected connectors with no
// modes (the common case), a connected one, a larger one, entries that are not
// connectors at all, and a tree with nothing connected -- which must answer
// zero, because zero is what tells the reader to size from the drawable.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "vocem/display.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

void connector(const char* root, const char* name, const char* status, const char* modes,
               const char* enabled = nullptr) {
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", root, name);
    mkdir(path, 0700);
    snprintf(path, sizeof(path), "%s/%s/status", root, name);
    if (FILE* f = fopen(path, "w")) {
        fputs(status, f);
        fclose(f);
    }
    if (enabled) {
        snprintf(path, sizeof(path), "%s/%s/enabled", root, name);
        if (FILE* f = fopen(path, "w")) {
            fputs(enabled, f);
            fclose(f);
        }
    }
    if (modes) {
        snprintf(path, sizeof(path), "%s/%s/modes", root, name);
        if (FILE* f = fopen(path, "w")) {
            fputs(modes, f);
            fclose(f);
        }
    }
}

}  // namespace

int main() {
    char root[] = "/tmp/vocem-drm-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }

    // Empty tree: no connectors at all.
    check(vocem::display_height_under(root) == 0, "an empty tree answers zero");

    // The machine this was written on, in miniature: one connected output among
    // disconnected ones, plus the entries that are not connectors.
    connector(root, "card1-DP-1", "disconnected\n", nullptr);
    connector(root, "card1-DP-2", "connected\n", "3840x2160\n1920x1080\n");
    connector(root, "card1-HDMI-A-1", "disconnected\n", nullptr);
    char not_a_connector[600];
    snprintf(not_a_connector, sizeof(not_a_connector), "%s/renderD128", root);
    mkdir(not_a_connector, 0700);
    snprintf(not_a_connector, sizeof(not_a_connector), "%s/version", root);
    if (FILE* f = fopen(not_a_connector, "w")) {
        fputs("drm 1.1.0\n", f);
        fclose(f);
    }
    check(vocem::display_height_under(root) == 2160,
          "one connected 4K output answers 2160, from its first mode");

    // A second, larger display: the largest wins, because from here nobody can
    // know which output the game will land on.
    connector(root, "card1-DP-3", "connected\n", "5120x2880\n");
    check(vocem::display_height_under(root) == 2880, "with two connected, the largest wins");

    // A connector that says connected but has no modes file must not spoil what
    // the others answered.
    connector(root, "card1-DP-4", "connected\n", nullptr);
    check(vocem::display_height_under(root) == 2880, "a connected output with no modes is skipped");

    // Connected is not switched on. A display disabled in the desktop's own
    // settings still says `connected`; `enabled` is what says it is scanned
    // out to. The window's reader asked both and this one asked one, so a
    // monitor switched off sized the overlay in the game and not in the
    // preview of it. The largest of the ENABLED outputs wins; one that says
    // nothing about it (the trees above) is taken as on.
    connector(root, "card1-DP-5", "connected\n", "7680x4320\n", "disabled\n");
    check(vocem::display_height_under(root) == 2880,
          "a connected display that is switched off does not size the overlay");
    connector(root, "card1-DP-6", "connected\n", "6016x3384\n", "enabled\n");
    check(vocem::display_height_under(root) == 3384, "and one switched on does");

    char cleanup[700];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    system(cleanup);

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

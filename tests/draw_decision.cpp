// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The per-application decision must follow the lists while the process runs.
//
// The Applications page promises that the switch beside each row "takes effect
// in a running game within a couple of seconds". The OpenGL path kept that
// promise; the Vulkan layer decided once, in a function-local static, at the
// first present -- so the same switch acted live in one path and only at the
// next game start in the other. The policy is one object now,
// vocem::DrawDecision, and this holds the property the static could not have:
// an edited list changes the answer on the very next ask, in the same process.
//
// The verdict half is pinned too: with a scrubbed environment and no desktop
// entry, a test binary is not a game (probes are not games; the detection is
// right to ignore them), so everything here moves through the lists alone.

#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#include "vocem/draw_decision.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

}  // namespace

int main() {
    // No launcher, no Flatpak, no Steam: the game verdict must come out false so
    // the lists are the only voice.
    unsetenv("SteamAppId");
    unsetenv("SteamGameId");
    unsetenv("GAMEID");
    unsetenv("LUTRIS_GAME_UUID");
    unsetenv("HEROIC_APP_NAME");
    unsetenv("INST_MC_DIR");
    unsetenv("ENABLE_GAMESCOPE_WSI");
    unsetenv("FLATPAK_ID");
    unsetenv("GIO_LAUNCHED_DESKTOP_FILE");

    char root[] = "/tmp/vocem-draw-decision-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL mkdtemp\n");
        return 1;
    }
    setenv("XDG_CACHE_HOME", root, 1);
    setenv("XDG_CONFIG_HOME", root, 1);

    vocem::DrawDecision decision;

    vocem::Config config;
    check(decision.refresh(config), "the first ask computes, and says so once");
    check(!decision.allowed(), "a probe with empty lists is not drawn in");
    check(!decision.refresh(config), "the same lists are not walked again");

    // The user flips the switch on: the running process must follow, not the
    // next one. The full binary name, because /proc/self/comm truncates at 15
    // characters and the lists match either spelling.
    config.shown_apps = "vocem_draw_decision";
    check(!decision.refresh(config), "an edited list recomputes without claiming first");
    check(decision.allowed(), "and shown_apps lets the running process in");

    // And off again, through the other list.
    config.shown_apps.clear();
    config.hidden_apps = "vocem_draw_decision";
    decision.refresh(config);
    check(!decision.allowed(), "hidden_apps takes it back out, same process");

    char cleanup[600];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    if (system(cleanup) != 0) {
        // Scratch cleanup is best-effort; the directory is in /tmp.
    }

    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

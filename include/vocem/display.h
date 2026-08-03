// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What resolution the machine's display actually runs at, from the kernel.
//
// The overlay's size used to be derived from the drawable it was drawing into,
// which is the right answer for a fullscreen game and the wrong one for a
// window: every resize rebuilt the atlas at a new size and every distance in
// the panel -- text, pictures, spacing -- rubber-banded with the window's edge.
// The size the overlay should keep is the *display's*, and the display's mode
// is in /sys/class/drm, which is also where the configuration window reads its
// caption from, for the same reason: on a fractionally scaled Wayland session
// every toolkit answer is logical and rounded, and the kernel's mode is what a
// game renders at.
//
// The daemon reads this and publishes it in the shared state; the injected code
// never touches /sys (a file syscall per connector has no place near a present
// hook, and sandboxes have opinions about /sys). With several displays the
// largest mode height wins -- there is no way to know from here which output a
// game will land on, and an overlay sized for the largest display is slightly
// large on a smaller one, which beats illegibly small on the larger. A machine
// with no readable mode (a VM, a headless run) answers zero and the reader
// falls back to sizing from the drawable, which is the old behaviour.

#ifndef VOCEM_DISPLAY_H
#define VOCEM_DISPLAY_H

#include <dirent.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace vocem {

// The largest connected output's mode height under `drm_root`, or 0 when none
// can be read. Parameterised for the tests; callers use display_height().
inline uint32_t display_height_under(const char* drm_root) {
    DIR* drm = ::opendir(drm_root);
    if (!drm) {
        return 0;
    }
    uint32_t best = 0;
    while (dirent* entry = ::readdir(drm)) {
        // Connectors are card<N>-<name>; the bare card<N> and renderD* are not.
        if (std::strncmp(entry->d_name, "card", 4) != 0 || !std::strchr(entry->d_name, '-')) {
            continue;
        }
        char path[512];
        std::snprintf(path, sizeof(path), "%s/%s/status", drm_root, entry->d_name);
        FILE* status = ::fopen(path, "r");
        if (!status) {
            continue;
        }
        char state[16] = {0};
        const bool connected =
            ::fgets(state, sizeof(state), status) && std::strncmp(state, "connected", 9) == 0;
        ::fclose(status);
        if (!connected) {
            continue;
        }
        std::snprintf(path, sizeof(path), "%s/%s/modes", drm_root, entry->d_name);
        FILE* modes = ::fopen(path, "r");
        if (!modes) {
            continue;
        }
        // The first line is the PREFERRED mode, "3840x2160" -- sysfs does not
        // mark the mode actually scanned out, so a panel deliberately run
        // below its native mode still reads as native here and the overlay is
        // sized for the native height. Known and accepted: the error direction
        // is "slightly large", the same trade largest-wins already makes.
        unsigned width = 0;
        unsigned height = 0;
        if (std::fscanf(modes, "%ux%u", &width, &height) == 2 && height > best) {
            best = height;
        }
        ::fclose(modes);
    }
    ::closedir(drm);
    return best;
}

inline uint32_t display_height() { return display_height_under("/sys/class/drm"); }

}  // namespace vocem

#endif  // VOCEM_DISPLAY_H

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

// One connected, enabled output and its preferred mode.
struct DisplayModeInfo {
    char name[64];  // the connector, card<N>- prefix stripped: "DP-2", "HDMI-A-1"
    uint32_t width;
    uint32_t height;
};

// Every connected, enabled connector with a readable mode under `drm_root`, in
// directory order, at most `capacity` of them; returns how many. The one
// reader of the tree: the daemon takes its height from it and the settings
// window its list of displays -- the window carried a second reader in Qt
// (gui/src/environment.h), which asked `enabled` while this one did not,
// until entry 135 made the two agree by hand; one spelling now.
inline int read_display_modes(const char* drm_root, DisplayModeInfo* out, int capacity) {
    DIR* drm = ::opendir(drm_root);
    if (!drm) {
        return 0;
    }
    int count = 0;
    while (dirent* entry = ::readdir(drm)) {
        if (count >= capacity) {
            break;
        }
        // Connectors are card<N>-<name>; the bare card<N> and renderD* are not.
        const char* dash = std::strchr(entry->d_name, '-');
        if (std::strncmp(entry->d_name, "card", 4) != 0 || !dash) {
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
        // Connected is not switched on: a display disabled in the desktop's
        // own settings still says `connected`, and only `enabled` says whether
        // anything is scanned out to it. Absent -- an old kernel, a fabricated
        // tree -- reads as enabled.
        std::snprintf(path, sizeof(path), "%s/%s/enabled", drm_root, entry->d_name);
        if (FILE* enabled = ::fopen(path, "r")) {
            char answer[16] = {0};
            const bool on = ::fgets(answer, sizeof(answer), enabled) &&
                            std::strncmp(answer, "enabled", 7) == 0;
            ::fclose(enabled);
            if (!on) {
                continue;
            }
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
        const bool read = std::fscanf(modes, "%ux%u", &width, &height) == 2;
        ::fclose(modes);
        if (!read || width == 0 || height == 0) {
            continue;
        }
        DisplayModeInfo& mode = out[count++];
        std::snprintf(mode.name, sizeof(mode.name), "%s", dash + 1);
        mode.width = width;
        mode.height = height;
    }
    ::closedir(drm);
    return count;
}

// How many connectors a tree can have before the enumeration stops counting.
// A machine has a handful; sixteen is more than any desktop board carries.
inline constexpr int kMaxDisplayModes = 16;

// The largest connected output's mode height under `drm_root`, or 0 when none
// can be read. Parameterised for the tests; callers use display_height().
inline uint32_t display_height_under(const char* drm_root) {
    DisplayModeInfo modes[kMaxDisplayModes];
    const int count = read_display_modes(drm_root, modes, kMaxDisplayModes);
    uint32_t best = 0;
    for (int i = 0; i < count; ++i) {
        if (modes[i].height > best) {
            best = modes[i].height;
        }
    }
    return best;
}

inline uint32_t display_height() { return display_height_under("/sys/class/drm"); }

}  // namespace vocem

#endif  // VOCEM_DISPLAY_H

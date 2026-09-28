// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// What resolution the machine's display actually runs at, from the kernel.
//
// "Actually" is the kernel's CRTC, asked with read-only mode ioctls on
// /dev/dri/card<N> (GETCONNECTOR without a probe, GETENCODER, GETCRTC; no DRM
// master needed). sysfs cannot answer it: /sys/class/drm/*/modes lists the
// modes a display CAN run, preferred first, and does not mark the one scanned
// out. Where the ioctls cannot answer (no access to the device, a
// fabricated tree under VOCEM_DRM_ROOT, a connector with no CRTC), the
// preferred mode stands in and `current` says so.
//
// The overlay keeps the display's size, not the drawable's, so a windowed
// game's panel does not follow the window's edge; the kernel's mode is what a
// game renders at, where a fractionally scaled toolkit answers logical and
// rounded sizes. The daemon reads this and publishes it; the
// injected code never touches /sys or /dev/dri. With several displays the
// largest mode height wins: the game's output is unknown here, and slightly
// large beats illegibly small. No readable mode (a VM, headless) answers zero
// and the reader sizes from the drawable.

#ifndef VOCEM_DISPLAY_H
#define VOCEM_DISPLAY_H

#include <dirent.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace vocem {

// One connected, enabled output and its mode.
struct DisplayModeInfo {
    char name[64];  // the connector, card<N>- prefix stripped: "DP-2", "HDMI-A-1"
    uint32_t width;
    uint32_t height;
    // Millihertz of the mode being scanned out, 0 when unknown (the preferred
    // mode stood in: sysfs does not carry a rate).
    uint32_t refresh_mhz;
    // True when width and height are the CRTC's running mode; false when they
    // are sysfs's preferred mode standing in for it.
    bool current;
};

// Where the running mode's device is for a sysfs tree: only the machine's own
// tree has one. A fabricated root (the tests' VOCEM_DRM_ROOT) has no device
// beside it and reads sysfs alone, which keeps those tests deterministic.
inline constexpr const char* kSysDrmRoot = "/sys/class/drm";

// The mode the CRTC driving `connector_id` on `card` ("card1") is scanning
// out, into `mode`; false when there is none to ask or it drives nothing.
// Read-only ioctls, a descriptor opened and closed here. GETCONNECTOR is asked
// with room for one mode: with none (count_modes == 0) the kernel PROBES the
// connector (an EDID read, tens of milliseconds), which libdrm's
// drmModeGetConnectorCurrent avoids the same way.
inline bool read_running_mode(const char* card, uint32_t connector_id, DisplayModeInfo& mode) {
    char device[64];
    std::snprintf(device, sizeof(device), "/dev/dri/%s", card);
    const int fd = ::open(device, O_RDONLY | O_CLOEXEC | O_NOCTTY);
    if (fd < 0) {
        return false;
    }
    bool found = false;
    drm_mode_modeinfo room {};
    drm_mode_get_connector connector {};
    connector.connector_id = connector_id;
    connector.count_modes = 1;
    connector.modes_ptr = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(&room));
    if (::ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) == 0 && connector.encoder_id != 0) {
        drm_mode_get_encoder encoder {};
        encoder.encoder_id = connector.encoder_id;
        if (::ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &encoder) == 0 && encoder.crtc_id != 0) {
            drm_mode_crtc crtc {};
            crtc.crtc_id = encoder.crtc_id;
            if (::ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) == 0 && crtc.mode_valid &&
                crtc.mode.hdisplay != 0 && crtc.mode.vdisplay != 0) {
                mode.width = crtc.mode.hdisplay;
                mode.height = crtc.mode.vdisplay;
                // The rate from the timings, which `vrefresh` rounds to a
                // whole hertz: clock is in kHz, so mHz is clock * 1e6 / total.
                uint64_t total = static_cast<uint64_t>(crtc.mode.htotal) * crtc.mode.vtotal;
                if (crtc.mode.flags & DRM_MODE_FLAG_DBLSCAN) {
                    total *= 2;
                }
                if (crtc.mode.vscan > 1) {
                    total *= crtc.mode.vscan;
                }
                uint64_t millihertz =
                    total ? static_cast<uint64_t>(crtc.mode.clock) * 1000000ull / total : 0;
                if (crtc.mode.flags & DRM_MODE_FLAG_INTERLACE) {
                    millihertz *= 2;
                }
                mode.refresh_mhz = static_cast<uint32_t>(millihertz);
                found = true;
            }
        }
    }
    ::close(fd);
    return found;
}

// Every connected, enabled connector with a readable mode under `drm_root`, in
// directory order, at most `capacity` of them; returns how many. Under the
// machine's own /sys/class/drm each carries the mode its CRTC runs; under any
// other root, and where the device cannot be asked, sysfs's preferred mode
// with `current` false. The one reader of the tree: the daemon takes its
// height from it and the settings window its list of displays.
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
        // A display disabled in the desktop's settings still says `connected`;
        // only `enabled` says whether anything is scanned out to it. Absent (an
        // old kernel, a fabricated tree) reads as enabled.
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
        // The first line is the PREFERRED mode, "3840x2160"; it stands in only
        // when the CRTC cannot be asked below.
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
        mode.refresh_mhz = 0;
        mode.current = false;
        // The running mode, from the device, on the machine's own tree: the
        // connector's id is in sysfs beside its modes, the card is the name's
        // prefix.
        if (std::strcmp(drm_root, kSysDrmRoot) == 0) {
            std::snprintf(path, sizeof(path), "%s/%s/connector_id", drm_root, entry->d_name);
            unsigned connector_id = 0;
            if (FILE* id = ::fopen(path, "r")) {
                if (std::fscanf(id, "%u", &connector_id) != 1) {
                    connector_id = 0;
                }
                ::fclose(id);
            }
            char card[16];
            const size_t card_length = static_cast<size_t>(dash - entry->d_name);
            if (connector_id != 0 && card_length < sizeof(card)) {
                std::memcpy(card, entry->d_name, card_length);
                card[card_length] = '\0';
                mode.current = read_running_mode(card, connector_id, mode);
            }
        }
    }
    ::closedir(drm);
    return count;
}

// How many connectors a tree can have before the enumeration stops counting.
// A machine has a handful; sixteen is more than any desktop board carries.
inline constexpr int kMaxDisplayModes = 16;

// The largest connected output's mode height under `drm_root` -- the running
// mode where it can be asked, the preferred one otherwise -- or 0 when none
// can be read (entry 39). Parameterised for the tests; callers use
// display_height().
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

inline uint32_t display_height() { return display_height_under(kSysDrmRoot); }

}  // namespace vocem

#endif  // VOCEM_DISPLAY_H

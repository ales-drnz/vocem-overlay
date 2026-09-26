// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The display's mode is the one being scanned out, not the one the panel
// prefers.
//
// display.h said "what resolution the display actually runs at" and read the
// first line of /sys/class/drm/*/modes, which is the PREFERRED mode: nothing
// in sysfs marks the running one. This machine runs 3840x2160 at 143.99 Hz
// and sysfs's first line is 3840x2160 at 60 -- the same height, so the
// difference in what the overlay is sized for cannot be shown here; a panel
// run below its native size would show it, and nobody changes the owner's
// display mode to measure it. What is held is the witness: every connector
// read_display_modes() reports under the machine's own tree carries the mode
// and the rate an independent walk of the kernel's CRTCs finds for the same
// connector id (GETRESOURCES, then each CRTC -- a different road from the
// header's connector -> encoder -> CRTC), and says it is current. And a
// fabricated tree, which has no device, says it is not.

#include <dirent.h>
#include <drm/drm.h>
#include <drm/drm_mode.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "probe_alarm.h"
#include "vocem/display.h"

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    std::printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

struct Running {
    uint32_t connector_id;
    uint32_t width;
    uint32_t height;
    uint32_t vrefresh;  // whole hertz, the kernel's own rounding
};

// Every connector with an active CRTC, found from the CRTC side: the resources'
// CRTC list, each CRTC's mode, and the connectors whose encoder drives it.
std::vector<Running> independent_walk(const char* device) {
    std::vector<Running> found;
    const int fd = open(device, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        return found;
    }
    drm_mode_card_res res {};
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &res) != 0) {
        close(fd);
        return found;
    }
    std::vector<uint32_t> crtcs(res.count_crtcs), connectors(res.count_connectors),
        encoders(res.count_encoders);
    drm_mode_card_res fill {};
    fill.count_crtcs = res.count_crtcs;
    fill.crtc_id_ptr = reinterpret_cast<uintptr_t>(crtcs.data());
    fill.count_connectors = res.count_connectors;
    fill.connector_id_ptr = reinterpret_cast<uintptr_t>(connectors.data());
    fill.count_encoders = res.count_encoders;
    fill.encoder_id_ptr = reinterpret_cast<uintptr_t>(encoders.data());
    if (ioctl(fd, DRM_IOCTL_MODE_GETRESOURCES, &fill) != 0) {
        close(fd);
        return found;
    }
    for (uint32_t crtc_id : crtcs) {
        drm_mode_crtc crtc {};
        crtc.crtc_id = crtc_id;
        if (ioctl(fd, DRM_IOCTL_MODE_GETCRTC, &crtc) != 0 || !crtc.mode_valid) {
            continue;
        }
        for (uint32_t encoder_id : encoders) {
            drm_mode_get_encoder encoder {};
            encoder.encoder_id = encoder_id;
            if (ioctl(fd, DRM_IOCTL_MODE_GETENCODER, &encoder) != 0 || encoder.crtc_id != crtc_id) {
                continue;
            }
            for (uint32_t connector_id : connectors) {
                drm_mode_modeinfo room {};
                drm_mode_get_connector connector {};
                connector.connector_id = connector_id;
                connector.count_modes = 1;  // never a probe
                connector.modes_ptr = reinterpret_cast<uintptr_t>(&room);
                if (ioctl(fd, DRM_IOCTL_MODE_GETCONNECTOR, &connector) == 0 &&
                    connector.encoder_id == encoder_id) {
                    found.push_back({connector_id, crtc.mode.hdisplay, crtc.mode.vdisplay,
                                     crtc.mode.vrefresh});
                }
            }
        }
    }
    close(fd);
    return found;
}

uint32_t sysfs_connector_id(const char* name) {
    DIR* drm = opendir("/sys/class/drm");
    uint32_t id = 0;
    if (!drm) {
        return 0;
    }
    while (const dirent* entry = readdir(drm)) {
        const char* dash = std::strchr(entry->d_name, '-');
        if (dash && std::strncmp(entry->d_name, "card", 4) == 0 && std::strcmp(dash + 1, name) == 0) {
            char path[512];
            std::snprintf(path, sizeof(path), "/sys/class/drm/%s/connector_id", entry->d_name);
            if (FILE* file = std::fopen(path, "r")) {
                if (std::fscanf(file, "%u", &id) != 1) {
                    id = 0;
                }
                std::fclose(file);
            }
        }
    }
    closedir(drm);
    return id;
}

}  // namespace

int main() {
    vocem_test::set_alarm(30, "reading the display's running mode");

    // The machine's own displays.
    std::vector<Running> running;
    if (DIR* dri = opendir("/dev/dri")) {
        while (const dirent* entry = readdir(dri)) {
            if (std::strncmp(entry->d_name, "card", 4) == 0) {
                const std::vector<Running> more =
                    independent_walk((std::string("/dev/dri/") + entry->d_name).c_str());
                running.insert(running.end(), more.begin(), more.end());
            }
        }
        closedir(dri);
    }
    if (running.empty()) {
        std::printf("skip no CRTC is scanning out here, or /dev/dri cannot be opened: "
                    "there is no running mode to compare with\n");
        return 77;
    }

    vocem::DisplayModeInfo modes[vocem::kMaxDisplayModes];
    const int count = vocem::read_display_modes(vocem::kSysDrmRoot, modes, vocem::kMaxDisplayModes);
    check(count > 0, "the machine's tree reports a display");
    for (int i = 0; i < count; ++i) {
        const uint32_t id = sysfs_connector_id(modes[i].name);
        const Running* truth = nullptr;
        for (const Running& r : running) {
            if (r.connector_id == id) {
                truth = &r;
            }
        }
        std::printf("     %s: reported %ux%u at %.3f Hz (%s); the CRTC walk says %ux%u at %u Hz\n",
                    modes[i].name, modes[i].width, modes[i].height, modes[i].refresh_mhz / 1000.0,
                    modes[i].current ? "current" : "preferred", truth ? truth->width : 0,
                    truth ? truth->height : 0, truth ? truth->vrefresh : 0);
        char what[160];
        std::snprintf(what, sizeof(what), "%s is reported at the mode its CRTC runs", modes[i].name);
        check(truth && modes[i].current && modes[i].width == truth->width &&
                  modes[i].height == truth->height,
              what);
        std::snprintf(what, sizeof(what), "%s carries the running rate, not the preferred one's",
                      modes[i].name);
        check(truth && (modes[i].refresh_mhz + 500) / 1000 == truth->vrefresh, what);
    }

    // A fabricated tree has no device: the preferred mode, and it says so.
    char scratch_template[] = "/tmp/vocem-display-XXXXXX";
    const char* scratch = mkdtemp(scratch_template);
    if (scratch) {
        const std::string connector = std::string(scratch) + "/card7-DP-9";
        mkdir(connector.c_str(), 0700);
        for (const char* file : {"status", "enabled", "modes", "connector_id"}) {
            if (FILE* f = std::fopen((connector + "/" + file).c_str(), "w")) {
                std::fputs(!std::strcmp(file, "status")    ? "connected\n"
                           : !std::strcmp(file, "enabled") ? "enabled\n"
                           : !std::strcmp(file, "modes")   ? "2560x1440\n1920x1080\n"
                                                           : "837\n",
                           f);
                std::fclose(f);
            }
        }
        vocem::DisplayModeInfo fabricated[vocem::kMaxDisplayModes];
        const int n = vocem::read_display_modes(scratch, fabricated, vocem::kMaxDisplayModes);
        check(n == 1 && fabricated[0].height == 1440 && !fabricated[0].current &&
                  fabricated[0].refresh_mhz == 0,
              "a fabricated tree reads its preferred mode and says it is not the running one");
        std::system((std::string("rm -rf '") + scratch + "'").c_str());
    }
    std::printf("%s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}

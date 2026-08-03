// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The panel itself: what the overlay looks like, independent of how it reaches
// the screen. Both the Vulkan layer and the OpenGL interposer compile this file,
// so there is exactly one implementation of the appearance.
//
// Anything renderer-specific stays behind AvatarProvider: the Vulkan side hands
// back descriptor sets, the OpenGL side hands back texture names, and this code
// never knows the difference.

#ifndef VOCEM_PANEL_H
#define VOCEM_PANEL_H

#include <cstdint>

#include "imgui.h"
#include "vocem/config.h"
#include "vocem/shared_state.h"

namespace vocem {

class AvatarProvider {
public:
    virtual ~AvatarProvider() = default;

    // Returns a texture usable by the active ImGui backend, or 0 while the image
    // is not available yet. Must never block: a miss just draws a plain disc.
    virtual ImTextureID texture(uint64_t user_id, const char* avatar_hash) = 0;
};

// Applies the shared look: rounded boxes, and the spacing the user has chosen.
// Call after every font atlas rebuild and whenever the spacing settings change --
// it starts from the defaults each time, so calling it again is harmless.
void configure_style(const Config& config);

// Whether each feature has anything to put on screen this frame. Both injection
// paths ask this before spending a frame on ImGui -- and they must ask for BOTH
// features: the guard used to be "in a voice channel with somebody", written
// once per path, which made a toast outside a voice channel unreachable in
// exactly the situation a toast is for. One spelling here, so the two paths and
// the build functions below cannot drift apart about what "something to draw"
// means.
inline bool panel_wanted(const Snapshot& snapshot, const Config& config) {
    return config.panel_enabled && snapshot.in_channel && snapshot.user_count > 0;
}

inline bool notification_wanted(const Snapshot& snapshot, const Config& config,
                                double now_seconds) {
    if (!config.notifications_enabled || snapshot.notification.serial == 0) {
        return false;
    }
    const double age = now_seconds - snapshot.notification.received;
    return age >= 0.0 && age <= config.notification_seconds;
}

// Emits the voice panel. Must be called between ImGui::NewFrame() and
// ImGui::Render().
//
// `now_seconds` is the same monotonic clock build_notification has always taken:
// the panel's few animations -- the ring's rise and fall, a joining row's fade,
// the crossfade when a picture arrives -- are functions of it, and their state
// between frames lives in a fixed array inside panel.cpp, never allocated on the
// present path. When every animation has settled the geometry is identical to
// the resting one, which tests/panel_geometry.cpp measures twice to hold.
void build_panel(const Snapshot& snapshot, const Config& config, uint32_t width, uint32_t height,
                 AvatarProvider* avatars, double now_seconds);

// Emits the notification toast, if there is a recent one and the user wants it.
// Separate from the voice panel because they are separate features: different
// placement, different lifetime, and either can be switched off without the other.
void build_notification(const Snapshot& snapshot, const Config& config, uint32_t width,
                        uint32_t height, AvatarProvider* avatars, double now_seconds);

}  // namespace vocem

#endif  // VOCEM_PANEL_H

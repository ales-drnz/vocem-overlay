// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// ImGui integration for the in-game side.
//
// Kept separate from the layer plumbing on purpose: everything here is about
// *what* gets drawn, everything in vocem_layer.cpp is about *where* and *when*.
//
// ImGui keeps one context and one backend per process, so this is a process-wide
// singleton that follows whichever swapchain is currently presenting. Games with
// two simultaneous swapchains are out of scope for now; the layer already passes
// those through untouched.

#ifndef VOCEM_OVERLAY_RENDERER_H
#define VOCEM_OVERLAY_RENDERER_H

#include <vulkan/vulkan.h>

#include "texture_cache.h"
#include "vocem/live_config.h"
#include "vocem/note.h"
#include "vocem/shared_state.h"

namespace vocem {

struct RendererTarget {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queue_family = 0;
    uint32_t image_count = 2;
    // The swapchain's height, which decides the size the font atlas is built at.
    uint32_t height = 1080;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    // The loader's pfnSetDeviceLoaderData for this device: every dispatchable
    // object created below the loader has to be registered through it (rule 4).
    PFN_vkSetDeviceLoaderData set_loader_data = nullptr;
};

class OverlayRenderer {
public:
    // Initialises on first call; the caller only asks while ready() is false.
    // A replaced device arrives as shutdown() then a fresh prepare(); a
    // recreated swapchain never comes back here -- its new render pass is
    // compatible with the pipeline by Vulkan's rules. Returns false if the
    // overlay cannot be drawn, in which case the caller must skip it.
    bool prepare(const RendererTarget& target);

    // Records draw commands into an already-begun render pass. `pipeline` is
    // the swapchain's HDR-converting pipeline, or VK_NULL_HANDLE to draw with
    // ImGui's stock one (every SDR swapchain).
    void draw(VkCommandBuffer command_buffer, const Snapshot& snapshot, uint32_t width,
              uint32_t height, VkPipeline pipeline);

    void shutdown();

    // Called after the present returns: does the queued avatar uploads, which
    // block on a fence and therefore cannot happen on the present path, and
    // rebuilds the font atlas when the output size or the user's scale changed.
    void process_uploads();

    bool ready() const { return backend_ready_; }

    // The settings as this process currently sees them. The present hook asks
    // whether either feature wants the frame before recording anything; the
    // LiveConfig behind this is the same one draw() reads, so the two cannot
    // disagree, and its cost is one stat() every couple of seconds.
    const Config& current_config() { return config_.current(); }

private:
    bool load_vulkan_functions(const RendererTarget& target);

    bool context_ready_ = false;
    bool backend_ready_ = false;
    bool functions_loaded_ = false;

    double last_frame_seconds_ = 0.0;
    // Presents seen with the backend up, and frames the overlay actually
    // painted: the Debug section's counters, written to the journal's stat
    // file from process_uploads() -- post-present, never inside the present.
    long frames_seen_ = 0;
    long frames_drawn_ = 0;
    double last_stat_seconds_ = 0.0;
    // Written by draw(), acted on by process_uploads(): the atlas size this output
    // wants. Rebuilding an atlas mid-frame would pull the texture out from under
    // the command buffer being recorded.
    float wanted_font_size_ = 0.0f;
    // The font_size setting the atlas should be laid out against, recorded
    // beside the size for the same reason: draw() may not rebuild.
    float wanted_reference_ = 16.0f;
    // The last colour-emoji status this renderer logged. A literal's address, so
    // the comparison is the "say it once" and a change says it again.
    const char* emoji_status_said_ = nullptr;
    TextureCache textures_;
    LiveConfig config_;
    // The message's words: which toast draw() wants them for, the reader that
    // fetches them post-present, and this process's copy, wiped as soon as the
    // toast is over (vocem/note.h).
    uint64_t wanted_note_serial_ = 0;
    uint64_t said_empty_note_ = 0;
    NoteReader note_;
    char note_body_[kNotificationBodyCapacity] = {0};
    // Defined in the .cpp: the panel takes an AvatarProvider, the cache is Vulkan
    // specific, and this bridges the two without leaking either into the other.
    class Adapter;
    Adapter* avatar_adapter_ = nullptr;
};

// The single instance. Lives for the lifetime of the process.
OverlayRenderer& renderer();

}  // namespace vocem

#endif  // VOCEM_OVERLAY_RENDERER_H

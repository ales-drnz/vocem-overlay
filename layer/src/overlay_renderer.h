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

#include <mutex>
#include <string>

#include "texture_cache.h"
#include "vocem/live_config.h"
#include "vocem/overlay_session.h"
#include "vocem/shared_state.h"

namespace vocem {

struct RendererTarget {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkRenderPass render_pass = VK_NULL_HANDLE;
    // The format that render pass was built for. The backend's own pipeline is
    // compatible with it and with nothing else (see draw()'s `pipeline`).
    VkFormat format = VK_FORMAT_UNDEFINED;
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
    // How many vertex/index buffer slots the backend cycles through. The
    // backend advances one slot per RenderDrawData call whatever image the
    // frame lands on, and a slot is reused after this many draws -- so the
    // ring has to be at least as long as the swapchain has images, or a
    // slot is rewritten while a command buffer for an older image still
    // reads it. It used to be sized from the FIRST swapchain's image count
    // and never revisited: a swapchain recreated with more images (a game
    // switching from double to triple buffering) overran it. ImGui offers no
    // way to grow the ring after init, so it is sized once for more images
    // than any swapchain has; one that has more passes through undrawn
    // (vocem_layer.cpp says so in the log).
    static constexpr uint32_t kRingSlots = 8;

    // Initialises on first call; the caller only asks while ready() is false.
    // A replaced device arrives as shutdown() then a fresh prepare(). Returns
    // false if the overlay cannot be drawn, and remembers that: a backend
    // that failed to come up against this device is not tried again on every
    // present (the pool, the pipeline and the shaders were being rebuilt per
    // frame). A new device -- shutdown() -- starts afresh.
    bool prepare(const RendererTarget& target);

    // Records draw commands into an already-begun render pass. `pipeline` is
    // the swapchain's own pipeline, built against its own render pass. It
    // may be VK_NULL_HANDLE only for a swapchain whose format is the one
    // prepare() was given -- the backend's stock pipeline was built against
    // that format's render pass and is compatible with no other; the layer
    // checks (format()) before handing a null here.
    void draw(VkCommandBuffer command_buffer, const Snapshot& snapshot, uint32_t width,
              uint32_t height, VkPipeline pipeline);

    void shutdown();

    // The device the backend was built for, and the swapchain format its stock
    // pipeline is compatible with. VK_NULL_HANDLE / UNDEFINED before prepare().
    VkDevice device() {
        std::lock_guard<std::mutex> guard(lock_);
        return device_;
    }
    VkFormat format() {
        std::lock_guard<std::mutex> guard(lock_);
        return format_;
    }

    // Called after the present returns: does the queued avatar uploads, which
    // block on a fence and therefore cannot happen on the present path, and
    // rebuilds the font atlas when the output size or the user's scale changed.
    void process_uploads();

    bool ready() const { return backend_ready_; }

    // The settings as this process currently sees them. The present hook asks
    // whether either feature wants the frame before recording anything; the
    // LiveConfig behind this is the same one draw() reads, so the two cannot
    // disagree, and its cost is one stat() every couple of seconds. A copy
    // rather than a reference: the reparse happens under the renderer's own
    // lock, and a reference handed out would outlive it.
    Config current_config() {
        std::lock_guard<std::mutex> guard(lock_);
        return config_.current();
    }

private:
    bool load_vulkan_functions(const RendererTarget& target);
    void shutdown_locked();

    // One lock over the renderer's state. draw() runs inside the present
    // under the layer's g_lock; prepare() and process_uploads() run after the
    // present, deliberately outside it (rule 9). With two swapchains presented
    // from two threads, one thread's post-present upload -- which rebuilds
    // the atlas and mutates the texture cache -- overlapped the other's
    // draw(). Taken after g_lock where both are held, never the other way.
    std::mutex lock_;

    bool context_ready_ = false;
    bool backend_ready_ = false;
    bool functions_loaded_ = false;
    // prepare() failed against the current device; nothing will be retried
    // until shutdown() (a new device) clears it.
    bool failed_ = false;
    VkDevice device_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;

    // Written by draw(), acted on by process_uploads(): the atlas size this output
    // wants. Rebuilding an atlas mid-frame would pull the texture out from under
    // the command buffer being recorded.
    float wanted_font_size_ = 0.0f;
    // The font_size setting the atlas should be laid out against, recorded
    // beside the size for the same reason: draw() may not rebuild.
    float wanted_reference_ = 16.0f;
    // And the files it should be built from, copied here for the same reason
    // again: process_uploads() runs after the present, and the settings it
    // reads must be the ones the frame was laid out against. Copies rather than
    // pointers into the live config, which reparses on its own tick.
    std::string wanted_font_path_;
    std::string wanted_font_path_strong_;
    TextureCache textures_;
    LiveConfig config_;
    // The message's words: which toast draw() wants them for, and this
    // process's copy of them, fetched post-present through the session and
    // wiped as soon as the toast is over (vocem/note.h).
    uint64_t wanted_note_serial_ = 0;
    char note_body_[kNotificationBodyCapacity] = {0};
    // Defined in the .cpp: the panel takes an AvatarProvider, the cache is Vulkan
    // specific, and this bridges the two without leaking either into the other.
    class Adapter;
    Adapter* avatar_adapter_ = nullptr;
};

// The single instance. Lives for the lifetime of the process.
OverlayRenderer& renderer();

// The process's shared bookkeeping -- the bridge, the decision, the journal,
// the frame counters, the toast's words, the frame clock -- one spelling with
// the GL side (vocem/overlay_session.h). The layer calls the present-path
// pieces (decide) from vocem_QueuePresentKHR and the file-work pieces (the
// stat, the note, the journal) from this renderer's post-present phase.
OverlaySession& session();

}  // namespace vocem

#endif  // VOCEM_OVERLAY_RENDERER_H

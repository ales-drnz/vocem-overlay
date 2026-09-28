// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// ImGui integration for the in-game side: *what* gets drawn, while
// vocem_layer.cpp decides *where* and *when*.
//
// ImGui keeps one context and backend per process, so this is a singleton on
// ONE device and ONE queue (owns()). Presents of that device on that queue are
// drawn into; a single present of two swapchains is passed through; a present
// on another device or queue is passed through, said once, until the owner has
// been silent for the hand-over interval, then the backend moves
// (vocem_layer.cpp).

#ifndef VOCEM_OVERLAY_RENDERER_H
#define VOCEM_OVERLAY_RENDERER_H

#include <vulkan/vulkan.h>

#include <mutex>
#include <string>

#include "texture_cache.h"
#include "vocem/atlas_owner.h"
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
    // The font atlas's build height: sizing_height() of the published display
    // and the swapchain, as draw() computes it, so the first frame does not
    // rebuild what prepare() built.
    uint32_t height = 1080;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
    // The loader's pfnSetDeviceLoaderData for this device: every dispatchable
    // object created below the loader has to be registered through it (rule 5).
    PFN_vkSetDeviceLoaderData set_loader_data = nullptr;
};

class OverlayRenderer {
public:
    // Vertex/index buffer slots the backend cycles through, one per
    // RenderDrawData whatever the image, so the ring must be at least as long as
    // the swapchain or a slot is rewritten while an older image's command buffer
    // reads it. ImGui cannot grow it after init, so it is sized above any real
    // swapchain; one with more images passes through undrawn (said in the log).
    static constexpr uint32_t kRingSlots = 8;

    // Brings the renderer up across as many calls as the first atlas takes; the
    // caller asks only while ready() is false. False while the atlas is still
    // rasterising (asked again next frame), and false for a backend that failed on
    // this device -- remembered, so nothing is rebuilt per present, until
    // shutdown() and a fresh prepare() for a new device.
    bool prepare(const RendererTarget& target);

    // Records draw commands into an already-begun render pass. `pipeline` is
    // the swapchain's own; VK_NULL_HANDLE only for a swapchain whose format is
    // the one prepare() was given, the only one the backend's stock pipeline is
    // compatible with (the layer checks format()).
    //
    // Nothing is recorded unless owns(device, queue): asked again here under
    // lock_ because prepare() and shutdown() run on other threads and can move
    // the backend. A refused frame leaves the render pass empty, which keeps the
    // game's pixels.
    void draw(VkCommandBuffer command_buffer, const Snapshot& snapshot, uint32_t width,
              uint32_t height, VkPipeline pipeline, VkDevice device, VkQueue queue);

    // Tears the backend down. The texture cache waits for its own uploads; the
    // CALLER must already have waited for the overlay's per-image fences
    // (vocem_layer.cpp's release_renderer_locked). vkDeviceWaitIdle is not used:
    // it needs every queue of the device externally synchronised, and a present
    // holds only its own.
    void shutdown();

    // The device the backend was built for, and the swapchain format its stock
    // pipeline is compatible with. VK_NULL_HANDLE / UNDEFINED before prepare().
    VkDevice device() {
        std::lock_guard<std::mutex> guard(lock_);
        return device_;
    }
    // Whether a present on `queue` of `device` may be drawn and uploaded for:
    // the device the objects were made on and the queue uploads are ordered on.
    // False before the backend is ready.
    bool owns(VkDevice device, VkQueue queue) {
        std::lock_guard<std::mutex> guard(lock_);
        return backend_ready_ && device == device_ && queue == queue_;
    }
    VkFormat format() {
        std::lock_guard<std::mutex> guard(lock_);
        return format_;
    }

    // After the present returns: the queued avatar uploads (which submit, so stay
    // off the present path), the font atlas rebuild when the output size or
    // scale changed, and new colour emoji folded in. Only for the owned queue
    // (owns()): the present's external synchronisation covers no other.
    void process_uploads(VkDevice device, VkQueue queue);

    bool ready() const { return backend_ready_; }

    // Whether prepare() failed on the device it was last given, until
    // shutdown(). The layer then holds the overlay on that device and queue,
    // as it would a renderer that came up (vocem/atlas_owner.h).
    bool failed() {
        std::lock_guard<std::mutex> guard(lock_);
        return failed_;
    }

    // The settings as this process sees them: the same LiveConfig draw() reads,
    // one stat() every couple of seconds. A copy, because the reparse happens
    // under lock_ and a reference would outlive it.
    Config current_config() {
        std::lock_guard<std::mutex> guard(lock_);
        return config_.current();
    }

private:
    bool load_vulkan_functions(const RendererTarget& target);
    void shutdown_locked();

    // One lock over the renderer's state. draw() runs inside the present under
    // the layer's g_lock; prepare() and process_uploads() run after it, outside
    // (rule 9), and two swapchains on two threads overlap them. Taken after
    // g_lock where both are held, never the other way.
    std::mutex lock_;

    bool context_ready_ = false;
    // The ImGui context exists but the atlas is being rasterised on the
    // library's atlas worker (vocem/atlas_owner.h), so the game keeps
    // presenting meanwhile. Nothing on the game's thread touches ImGui or the
    // fonts module until then -- draw() and process_uploads() wait for
    // backend_ready_ -- and every teardown joins the worker first.
    bool context_created_ = false;
    bool backend_ready_ = false;
    bool functions_loaded_ = false;
    // prepare() failed against the current device; nothing will be retried
    // until shutdown() (a new device) clears it.
    bool failed_ = false;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkFormat format_ = VK_FORMAT_UNDEFINED;

    // Recorded by draw(), acted on by process_uploads(): the atlas size, layout
    // reference and font files this frame was laid out against. draw() may not
    // rebuild -- that would pull the texture from under the command buffer --
    // and copies, not pointers, because the live config reparses on its own tick.
    float wanted_font_size_ = 0.0f;
    float wanted_reference_ = 16.0f;
    std::string wanted_font_path_;
    std::string wanted_font_path_strong_;
    // Owns the font atlas's texture as well as the faces (entry 192); prepare()
    // brings no backend up without it.
    TextureCache textures_;
    // Owed while the GPU's font image no longer matches the CPU atlas: nothing
    // is drawn until it does, and the whole upload is retried once a second
    // (process_uploads).
    UploadRetry font_retry_;
    // Uploads the atlas through whichever of the two owns it, whole or only
    // the squares a fold wrote. False when neither managed.
    bool upload_font_texture(bool whole);
    LiveConfig config_;
    // The toast draw() wants words for, and this process's copy of them, fetched
    // post-present and wiped when the toast is over (vocem/note.h).
    uint64_t wanted_note_serial_ = 0;
    char note_body_[kNotificationBodyCapacity] = {0};
    // Bridges the panel's AvatarProvider to the Vulkan cache (defined in the .cpp).
    class Adapter;
    Adapter* avatar_adapter_ = nullptr;
};

// The single instance. Lives for the lifetime of the process.
OverlayRenderer& renderer();

// The process's shared bookkeeping, one spelling with the GL side
// (vocem/overlay_session.h): decide() on the present path, the stat, the note
// and the journal from this renderer's post-present phase.
OverlaySession& session();

}  // namespace vocem

#endif  // VOCEM_OVERLAY_RENDERER_H

// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.

#include "overlay_renderer.h"

#include <time.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "imgui.h"
#include "imgui_impl_vulkan.h"
#include "vocem/clock.h"
#include "vocem/journal.h"
#include "vocem/fonts.h"
#include "vocem/overlay_log.h"
#include "vocem/panel.h"

// Failures in here are silent by design in release builds -- the overlay simply
// does not appear -- so they must be traceable when VOCEM_DEBUG is set. The
// one logger both paths share (vocem/overlay_log.h), under this file's tag.
#define VOCEM_RLOG(...) VOCEM_OVERLAY_LOG("vocem/render", __VA_ARGS__)

namespace vocem {
namespace {

// Passed to ImGui's Vulkan backend so it resolves every entry point through our
// dispatch chain instead of the loader's exported symbols. Inside a layer this is
// not optional: the global symbols would re-enter the loader from the top and
// bypass the layers below us.
struct FunctionLoaderContext {
    VkInstance instance = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    PFN_vkGetDeviceProcAddr gdpa = nullptr;
};

FunctionLoaderContext g_loader_context;

// Set by the backend's CheckVkResultFn: the vendored ImGui backend reports
// every failure through that callback and through nothing else.
bool g_backend_failed = false;

PFN_vkVoidFunction resolve_function(const char* name, void* user_data) {
    auto* context = static_cast<FunctionLoaderContext*>(user_data);
    if (!context) {
        return nullptr;
    }
    // Device-level functions first: they are the hot ones and the dispatch is
    // cheaper. Instance-level lookups (memory properties, for example) fall back
    // to the instance chain.
    if (context->gdpa && context->device) {
        if (PFN_vkVoidFunction function = context->gdpa(context->device, name)) {
            return function;
        }
    }
    if (context->gipa) {
        return context->gipa(context->instance, name);
    }
    return nullptr;
}

}  // namespace

// The panel talks to AvatarProvider; the Vulkan cache has its own signature, so
// this bridges the two without leaking either into the other.
class OverlayRenderer::Adapter : public AvatarProvider {
public:
    explicit Adapter(TextureCache& cache) : cache_(cache) {}
    ImTextureID texture(uint64_t user_id, const char* avatar_hash) override {
        return cache_.get(user_id, avatar_hash);
    }

private:
    TextureCache& cache_;
};

OverlayRenderer& renderer() {
    static OverlayRenderer instance;
    return instance;
}

OverlaySession& session() {
    static OverlaySession instance("vulkan", "vocem");
    return instance;
}

bool OverlayRenderer::load_vulkan_functions(const RendererTarget& target) {
    // The context is refreshed on every call, ahead of the early return below.
    // resolve_function reads it at call time, so a stale device here is not a
    // stale cache: it is vkGetDeviceProcAddr on a destroyed handle. A device lost
    // to a GPU hang is destroyed and recreated -- the standard recovery -- and
    // this used to keep answering with the dead one for the rest of the session.
    g_loader_context.instance = target.instance;
    g_loader_context.device = target.device;
    g_loader_context.gipa = target.gipa;
    g_loader_context.gdpa = target.gdpa;

    if (functions_loaded_) {
        return true;
    }

    // ImGui stores these globally, so the first device wins. A game creating a
    // second logical device would need a second context; not supported yet.
    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_1, resolve_function,
                                       &g_loader_context)) {
        VOCEM_RLOG("ImGui_ImplVulkan_LoadFunctions failed");
        return false;
    }
    functions_loaded_ = true;
    return true;
}

bool OverlayRenderer::prepare(const RendererTarget& target) {
    std::lock_guard<std::mutex> guard(lock_);
    if (backend_ready_) {
        return true;
    }
    if (failed_) {
        return false;  // said once below; not retried per present
    }
    if (!target.device || !target.render_pass || !target.gdpa || !target.gipa) {
        VOCEM_RLOG("incomplete target: device=%p render_pass=%p gdpa=%p gipa=%p",
                   (void*)target.device, (void*)target.render_pass, (void*)target.gdpa,
                   (void*)target.gipa);
        failed_ = true;
        return false;
    }

    if (!load_vulkan_functions(target)) {
        failed_ = true;
        return false;
    }

    if (!context_ready_) {
        IMGUI_CHECKVERSION();
        // With the fonts module's atlas: this context dies with the device and
        // another takes its place in the same process, and the ImFont pointers
        // the module caches have to survive that (vocem/fonts.h).
        ImGui::CreateContext(fonts_atlas());
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;   // never write files from inside a game
        io.LogFilename = nullptr;
        io.BackendPlatformName = "vocem";
        io.DisplaySize = ImVec2(1.0f, 1.0f);

        // Built before the backend exists, so ImGui_ImplVulkan_Init's first
        // NewFrame uploads the atlas we want rather than the default bitmap and
        // then throws it away. The size comes from the swapchain we are attaching
        // to, which is why this cannot happen at library load time.
        ensure_fonts(font_pixel_size(target.height, config_.current().scale,
                                     config_.current().font_size),
                     config_.current().font_size);
        configure_style(config_.current());
        context_ready_ = true;
    }

    // No re-initialise branch here, and that is a fact worth stating: the one
    // caller only reaches prepare() when ready() is false, so a "target
    // changed" comparison in here could never run -- one lived here for months,
    // dead, under a comment promising swapchain-recreate handling. A new device
    // arrives through vocem_DestroyDevice -> shutdown(). A recreated swapchain
    // does NOT come back here, and its render pass is compatible with the
    // stock pipeline built below only while its format is the one recorded in
    // format_: the layer builds a pipeline of its own per swapchain and asks
    // format() before ever drawing with the stock one (a comment here used to
    // say "compatible by Vulkan's own rules", which is true of a resize and
    // false of a format change -- HDR switched off in a game's settings, an
    // sRGB swapchain replaced by a UNORM one).
    {
        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = VK_API_VERSION_1_1;
        info.Instance = target.instance;
        info.PhysicalDevice = target.physical_device;
        info.Device = target.device;
        info.QueueFamily = target.queue_family;
        info.Queue = target.queue;
        info.RenderPass = target.render_pass;
        // The ring, sized once for more images than any swapchain has rather
        // than for this swapchain's count (kRingSlots says why).
        info.MinImageCount = 2;
        info.ImageCount = kRingSlots;
        info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        // Let the backend own its descriptor pool: one less thing for the layer
        // to allocate and destroy alongside the swapchain. Sized from the avatar
        // cache's budget, NOT a small constant: every avatar holds one set, the
        // pool held 8, and the eighth face of a call was a dead game (entry 46)
        // -- AddTexture on an exhausted pool updates an uninitialised set
        // instead of failing.
        info.DescriptorPoolSize = TextureCache::kDescriptorPoolSets;

        // Without this the backend's every VkResult is discarded: its
        // check_vk_result() is a no-op when the callback is null, so
        // ImGui_ImplVulkan_Init returns true whatever happened inside it and
        // AddTexture returns an uninitialised descriptor set when the allocation
        // fails. Entry 46 closed the pool-exhaustion door; this is the same
        // crash through OUT_OF_HOST_MEMORY and FRAGMENTED_POOL.
        g_backend_failed = false;
        info.CheckVkResultFn = [](VkResult result) {
            if (result != VK_SUCCESS) {
                g_backend_failed = true;
                VOCEM_RLOG("Vulkan backend call failed: VkResult %d", static_cast<int>(result));
            }
        };

        if (!ImGui_ImplVulkan_Init(&info) || g_backend_failed) {
            VOCEM_RLOG("ImGui_ImplVulkan_Init failed");
            ImGui_ImplVulkan_Shutdown();
            failed_ = true;
            return false;
        }
        // The font atlas, uploaded HERE, in the post-present phase, and not
        // left to the backend's NewFrame. ImGui 1.90.1 moved the upload out of
        // Init and into the first NewFrame ("automatically called by NewFrame()
        // the first time"), and NewFrame runs inside draw() -- inside
        // vkQueuePresentKHR, under the layer's lock -- where the upload's
        // vkQueueSubmit and vkQueueWaitIdle are exactly what rule 10 keeps out
        // of the present. The comment at the top of draw_overlay() went on
        // saying initialisation happens after the present while the atlas was
        // being uploaded inside it; tests/vk_witness_layer.cpp is what sees a
        // queue wait inside a present now.
        if (!ImGui_ImplVulkan_CreateFontsTexture() || g_backend_failed) {
            VOCEM_RLOG("font atlas upload failed");
            ImGui_ImplVulkan_Shutdown();
            failed_ = true;
            return false;
        }
        VOCEM_RLOG("backend ready (%u ring slots, queue family %u)", info.ImageCount,
                   target.queue_family);
        // The session's journal (vocem/journal.h): opened at the first frame
        // the overlay draws in this process, closed into history on a clean
        // exit by the layer's destructor -- and left behind, still `.running`,
        // by a crash, which is the detection.
        session().journal_begin_once();
        vocem::journal_note("Vulkan backend ready");
        backend_ready_ = true;
        device_ = target.device;
        format_ = target.format;
        // Resolved here rather than at teardown, because by then the device may
        // be the one being destroyed and vkGetDeviceProcAddr on it is not ours
        // to call. shutdown_locked() says what it is for.
        device_wait_idle_ = reinterpret_cast<PFN_vkDeviceWaitIdle>(
            resolve_function("vkDeviceWaitIdle", &g_loader_context));

        if (!avatar_adapter_) {
            static Adapter adapter(textures_);
            avatar_adapter_ = &adapter;
        }
        if (!textures_.init(target.device, target.physical_device, target.queue,
                            target.queue_family, resolve_function, &g_loader_context,
                            target.set_loader_data)) {
            // Avatars are optional: without them the panel still shows names and
            // speaking state, so this is not a reason to disable the overlay.
            VOCEM_RLOG("avatar textures unavailable, falling back to plain circles");
        }
    }
    return true;
}

void OverlayRenderer::draw(VkCommandBuffer command_buffer, const Snapshot& snapshot,
                           uint32_t width, uint32_t height, VkPipeline pipeline) {
    std::lock_guard<std::mutex> guard(lock_);
    if (!backend_ready_) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));

    const double now = monotonic_seconds();
    // The measured time since the previous frame, one spelling with the GL side.
    io.DeltaTime = session().delta_time(now);

    // What the atlas should be built at for this output. The rebuild itself cannot
    // happen here -- it destroys the texture the previous frames are still using --
    // so it is only recorded, and process_uploads() acts on it after the present.
    const Config& config = config_.current();
    // Sized by the display, not by the swapchain: a resize recreates the
    // swapchain and used to rebuild the atlas at the new size, rubber-banding
    // every distance in the panel with the window's edge. The daemon publishes
    // the display's mode height; sizing_height() says when the drawable wins
    // instead (zero display, or a supersampled drawable taller than the
    // display and headed for a downscale).
    wanted_font_size_ = font_pixel_size(
        sizing_height(snapshot.display_height, height), config.scale,
        config.font_size);
    wanted_reference_ = config.font_size;
    wanted_font_path_ = config.font_path;
    wanted_font_path_strong_ = config.font_path_strong;
    // Spacing is configurable, and it lives in the style rather than in the draw
    // calls, so the style has to follow an edited settings file even when the font
    // size has not moved.
    configure_style(config);

    if (!config.enabled) {
        return;
    }

    // The message's words, from the note segment the daemon writes them to --
    // read in the post-present phase and handed to the next frame, because
    // this one is inside vkQueuePresentKHR and the layer takes no file work
    // there (rule 8). A toast lasts seconds, so arriving one frame later is
    // invisible; what it buys is that a process that never draws a toast
    // never opens that segment at all (vocem/note.h).
    if (notification_wanted(snapshot, config, now)) {
        wanted_note_serial_ = snapshot.notification.serial;
        std::snprintf(const_cast<Snapshot&>(snapshot).notification.body,
                      sizeof(snapshot.notification.body), "%s", note_body_);
    } else {
        wanted_note_serial_ = 0;
    }

    // Which colour emoji this frame's text needs -- asked AFTER the words are
    // in the snapshot, because the segment's copy of the body is empty by
    // design and an emoji that appears only in a message would otherwise
    // never be noted at all. Only noted here; the rebuild happens in
    // process_uploads(), after the present, like every other atlas change.
    // The snapshot is this frame's own copy (the same const_cast the body
    // above makes), and the noting rewrites an emoji sequence into its key.
    fonts_note_emoji_in(const_cast<Snapshot&>(snapshot));

    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
    AvatarProvider* avatars = textures_.ready() ? avatar_adapter_ : nullptr;
    // A frame can be for the toast alone -- a message arriving outside a voice
    // channel -- and the panel must not be handed a snapshot with nobody in it.
    if (panel_wanted(snapshot, config)) {
        build_panel(snapshot, config, width, height, avatars, now);
    }
    build_notification(snapshot, config, width, height, avatars, now);
    ImGui::Render();

    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), command_buffer, pipeline);
    session().frame_drawn();
}

void OverlayRenderer::process_uploads() {
    std::lock_guard<std::mutex> guard(lock_);
    if (!backend_ready_) {
        return;
    }

    // The Debug section's frame and draw counters. This is the post-present
    // phase, where file work is allowed; the write itself happens at most once
    // every few seconds.
    session().frame_seen();

    // Resolution changed, or the user moved the size slider: rasterise the atlas
    // again at the new size instead of stretching the old one. Safe here and only
    // here -- ImGui_ImplVulkan_CreateFontsTexture waits on the queue before
    // replacing the texture, which is legal after the present has returned and
    // would deadlock inside it.
    if (wanted_font_size_ > 0.0f &&
        ensure_fonts(wanted_font_size_, wanted_reference_, wanted_font_path_.c_str(),
                     wanted_font_path_strong_.c_str())) {
        configure_style(config_.current());
        if (!ImGui_ImplVulkan_CreateFontsTexture()) {
            VOCEM_RLOG("font texture upload failed at %.1f px", wanted_font_size_);
        } else {
            VOCEM_RLOG("font atlas rebuilt at %.1f px", wanted_font_size_);
        }
    }

    // Why there are no colour emoji, and why the text is not in the font the
    // settings name: said once per change, the same way on both paths.
    session().say_font_statuses();

    // The words for the toast the next frame will draw. Here rather than in
    // draw(): this is the phase where file work is allowed. A message that
    // arrived without them is said once, in the session.
    if (wanted_note_serial_ != 0) {
        std::snprintf(note_body_, sizeof(note_body_), "%s",
                      session().note_words(wanted_note_serial_));
    } else if (note_body_[0] != '\0') {
        session().note_forget();
        std::memset(note_body_, 0, sizeof(note_body_));
    }

    textures_.process_pending();
}

void OverlayRenderer::shutdown() {
    std::lock_guard<std::mutex> guard(lock_);
    shutdown_locked();
}

void OverlayRenderer::shutdown_locked() {
    // Nothing below may run while the GPU is still reading what it destroys.
    //
    // This used to be a property of *who could call this*: vocem_DestroyDevice,
    // where the application has already had to finish everything, and nothing
    // else. Then a second caller arrived -- the daemon stopping, which fires on
    // an arbitrary present of a live, presenting device -- and the property went
    // with it. The overlay's submit for the previous image is at most one
    // present old and no fence on this path consults it; textures_.shutdown()
    // frees the images and the command pool it reads, and
    // ImGui_ImplVulkan_Shutdown() frees the vertex ring, the font image, the
    // pipeline and the descriptor pool (imgui_impl_vulkan.cpp has no wait of its
    // own -- checked). The wait belongs here rather than at the new call site so
    // that the next caller inherits it: entry 131 fixed exactly this shape for
    // the second-device case and it came back through a door nobody had yet.
    //
    // vkDeviceWaitIdle wants external synchronisation on the device's queues,
    // which this has: it runs after the present returned, under the renderer's
    // own lock, and destroy_swapchain_resources does the same thing for the same
    // reason. A failure (a lost device) is not a reason to keep the memory --
    // rule 7 -- so the answer is not checked.
    if (device_wait_idle_ && backend_ready_ && device_ != VK_NULL_HANDLE) {
        device_wait_idle_(device_);
    }
    // The next device's first frame must not measure the gap between devices as
    // one animation step.
    session().reset_clock();
    textures_.shutdown();
    if (backend_ready_) {
        ImGui_ImplVulkan_Shutdown();
        backend_ready_ = false;
    }
    if (context_ready_) {
        ImGui::DestroyContext();
        context_ready_ = false;
    }
    // The next prepare() belongs to a different device, so the backend's function
    // table is loaded again rather than kept from the destroyed one -- and a
    // failure against the old device says nothing about the new one.
    functions_loaded_ = false;
    failed_ = false;
    device_ = VK_NULL_HANDLE;
    device_wait_idle_ = nullptr;
    format_ = VK_FORMAT_UNDEFINED;
}

}  // namespace vocem

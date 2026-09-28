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
#ifndef VOCEM_IMCONFIG_INJECTED
#error "vocem/imconfig_injected.h is not in effect: IM_ASSERT would be assert() inside a game"
#endif
#include "vocem/clock.h"
#include "vocem/journal.h"
#include "vocem/fonts.h"
#include "vocem/overlay_log.h"
#include "vocem/panel.h"

// Failures in here are silent in release builds -- the overlay simply does not
// appear -- so they are traced under VOCEM_DEBUG, through the shared logger.
#define VOCEM_RLOG(...) VOCEM_OVERLAY_LOG("vocem/render", __VA_ARGS__)

namespace vocem {
namespace {

// Passed to ImGui's Vulkan backend so every entry point resolves through our
// dispatch chain: the loader's exported symbols would re-enter the chain from
// the top and bypass the layers below us.
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
    // Device-level functions through the device chain; instance-level ones
    // (told by name, all the callers have) go to the instance chain, because
    // vkGetDeviceProcAddr does not promise them and the validation layer says
    // so. vkGetInstanceProcAddr answers device functions too, so a
    // misclassified name costs a trampoline and nothing else.
    const bool instance_level = std::strstr(name, "PhysicalDevice") != nullptr ||
                                std::strstr(name, "SurfaceKHR") != nullptr;
    if (!instance_level && context->gdpa && context->device) {
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
    // Refreshed on every call, ahead of the early return: resolve_function
    // reads it at call time, and a device recreated after a GPU hang must not be
    // resolved against the destroyed handle.
    g_loader_context.instance = target.instance;
    g_loader_context.device = target.device;
    g_loader_context.gipa = target.gipa;
    g_loader_context.gdpa = target.gdpa;

    if (functions_loaded_) {
        return true;
    }

    // ImGui stores these globally: one backend per process, on one device. A
    // hand-over to another device is a shutdown() and a fresh prepare(), which
    // loads them again (vocem_layer.cpp).
    if (!ImGui_ImplVulkan_LoadFunctions(VK_API_VERSION_1_1, resolve_function,
                                       &g_loader_context)) {
        VOCEM_RLOG("ImGui_ImplVulkan_LoadFunctions failed");
        return false;
    }
    functions_loaded_ = true;
    return true;
}

void OverlayRenderer::refuse(const RendererTarget& target) {
    failed_ = true;
    failed_device_ = target.device;
    failed_queue_ = target.queue;
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
        refuse(target);
        return false;
    }

    if (!load_vulkan_functions(target)) {
        refuse(target);
        return false;
    }

    if (!context_ready_) {
        bool first = false;
        if (!context_created_) {
            IMGUI_CHECKVERSION();
            // With the fonts module's atlas, so the ImFont pointers it caches
            // survive this context dying with its device (vocem/fonts.h).
            ImGui::CreateContext(fonts_atlas());
            ImGuiIO& io = ImGui::GetIO();
            io.IniFilename = nullptr;   // never write files from inside a game
            io.LogFilename = nullptr;
            io.BackendPlatformName = "vocem";
            io.DisplaySize = ImVec2(1.0f, 1.0f);
            context_created_ = true;
            first = true;
        }

        // Built before the backend exists, at the target's size, so the upload
        // sends the atlas we want -- and off the game's thread, RGBA widening
        // included (vocem/atlas_owner.h). The context comes first because
        // ensure_fonts() reaches the atlas through ImGui::GetIO(), and GImGui is
        // a plain global the worker sees. The worker copies what it reads.
        const Config& config = config_.current();
        const float pixels = font_pixel_size(target.height, config.scale, config.font_size);
        switch (atlas_worker().step(first, pixels, config.font_size, config.font_path,
                                    config.font_path_strong, nullptr)) {
            case AtlasWorker::Step::Started:
                VOCEM_RLOG("rasterising the font atlas at %.0f px off the game's thread",
                           static_cast<double>(pixels));
                return false;  // asked again on the next frame with something on it
            case AtlasWorker::Step::Building:
                return false;  // still rasterising; the game goes on presenting meanwhile
            case AtlasWorker::Step::NoThread:
                // No thread to be had (a sandbox that refuses clone, resources):
                // the build happens here.
                VOCEM_RLOG("no thread for the font atlas; rasterising it on the game's thread");
                atlas_worker().run_here();
                break;
            case AtlasWorker::Step::Finished:
            case AtlasWorker::Step::Idle:
                break;
        }
        configure_style(config_.current());
        context_ready_ = true;
    }

    // No re-initialise branch: the one caller reaches prepare() only while
    // ready() is false, and a new device arrives through vocem_DestroyDevice ->
    // shutdown(). A recreated swapchain does not come back here; its render
    // pass is compatible with the stock pipeline below only while its format
    // is format_, so the layer builds a pipeline per swapchain and asks
    // format() before drawing with the stock one.
    {
        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = VK_API_VERSION_1_1;
        info.Instance = target.instance;
        info.PhysicalDevice = target.physical_device;
        info.Device = target.device;
        info.QueueFamily = target.queue_family;
        info.Queue = target.queue;
        info.RenderPass = target.render_pass;
        // Sized for more images than any swapchain has (kRingSlots).
        info.MinImageCount = 2;
        info.ImageCount = kRingSlots;
        info.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
        // The backend owns its descriptor pool, sized from the avatar cache's
        // budget: AddTexture on an exhausted pool updates an uninitialised set
        // instead of failing, and kills the game (entry 46).
        info.DescriptorPoolSize = TextureCache::kDescriptorPoolSets;

        // Without this callback the backend discards every VkResult: Init
        // returns true whatever happened and AddTexture hands back an
        // uninitialised set when allocation fails.
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
            refuse(target);
            return false;
        }
        // The font atlas is uploaded here, post-present, by the texture cache,
        // and never left to the backend's first NewFrame: that runs inside
        // draw(), inside the present, where its vkQueueSubmit and
        // vkQueueWaitIdle are what rule 10 keeps out (tests/vk_witness_layer.cpp
        // sees a queue wait inside a present). The cache owns the font texture
        // so a new colour emoji is copied in as a 32x32 square rather than
        // replacing the whole atlas (43 MB at a 2160-line display), so it is
        // initialised before the atlas.
        //
        // Without the cache there is no backend: ImGui's stock font upload
        // allocates a command buffer the loader never registers, which crashes
        // under the validation layer (rule 5). The cache fails only where the
        // device refuses a sampler, a command pool or a memory type, so this
        // declines, says so once, and remembers it until another device.
        if (!avatar_adapter_) {
            static Adapter adapter(textures_);
            avatar_adapter_ = &adapter;
        }
        if (!textures_.init(target.device, target.physical_device, target.queue,
                            target.queue_family, resolve_function, &g_loader_context,
                            target.set_loader_data)) {
            VOCEM_RLOG("not drawing: the texture cache did not come up on this device, and "
                       "without it the font texture would need ImGui's own upload, whose "
                       "command buffer the loader never registers");
            ImGui_ImplVulkan_Shutdown();
            refuse(target);
            return false;
        }
        if (!upload_font_texture(true) || g_backend_failed) {
            VOCEM_RLOG("font atlas upload failed");
            textures_.shutdown();
            ImGui_ImplVulkan_Shutdown();
            refuse(target);
            return false;
        }
        VOCEM_RLOG("backend ready (%u ring slots, queue family %u)", info.ImageCount,
                   target.queue_family);
        // The session's journal (vocem/journal.h): opened at the first frame
        // drawn in this process, closed into history by the layer's destructor
        // on a clean exit, left `.running` by a crash.
        session().journal_begin_once();
        vocem::journal_note("Vulkan backend ready");
        backend_ready_ = true;
        device_ = target.device;
        queue_ = target.queue;
        format_ = target.format;
    }
    return true;
}

bool OverlayRenderer::upload_font_texture(bool whole) {
    ImGuiIO& io = ImGui::GetIO();
    if (!whole) {
        // The squares come with their own pixels: the atlas's RGBA copy is
        // not there once the whole atlas is up, and is not made for a fold.
        AtlasRegion regions[kMaxFoldedRegions];
        const uint32_t count = fonts_take_folded(regions, kMaxFoldedRegions);
        if (io.Fonts->TexWidth > 0 && io.Fonts->TexHeight > 0 &&
            textures_.update_font_atlas(static_cast<uint32_t>(io.Fonts->TexWidth),
                                        static_cast<uint32_t>(io.Fonts->TexHeight), regions,
                                        count)) {
            // Said, so the arrivals scene can count folds (entry 192).
            VOCEM_RLOG("font texture: %u folded square(s) copied in place", count);
            return true;
        }
        // The image does not match the atlas: replace it whole, which is
        // always correct.
    } else {
        fonts_take_folded(nullptr, 0);  // already in the whole atlas
    }
    // The whole atlas as RGBA, widened again with its colour squares if it was
    // handed back after the last whole upload, and handed back once the
    // staging buffer holds it: the image is the atlas from here on.
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    if (!fonts_atlas_rgba(&pixels, &width, &height) || width <= 0 || height <= 0) {
        return false;
    }
    const ImTextureID id = textures_.upload_font_atlas(pixels, static_cast<uint32_t>(width),
                                                       static_cast<uint32_t>(height));
    fonts_atlas_uploaded();
    if (id == 0) {
        return false;
    }
    io.Fonts->SetTexID(id);
    VOCEM_RLOG("font texture uploaded whole (%dx%d)", width, height);
    return true;
}

void OverlayRenderer::draw(VkCommandBuffer command_buffer, const Snapshot& snapshot,
                           uint32_t width, uint32_t height, VkPipeline pipeline, VkDevice device,
                           VkQueue queue) {
    std::lock_guard<std::mutex> guard(lock_);
    if (!backend_ready_ || font_retry_.owed() || device != device_ || queue != queue_) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));

    const double now = monotonic_seconds();
    // The measured time since the previous frame, one spelling with the GL side.
    io.DeltaTime = session().delta_time(now);

    // The atlas size this output wants, only recorded: rebuilding here would
    // destroy the texture earlier frames still use, so process_uploads() does it
    // after the present.
    const Config& config = config_.current();
    // Sized by the display the daemon publishes, not the swapchain, so a window
    // resize does not rubber-band the panel; sizing_height() says when the
    // drawable wins instead.
    wanted_font_size_ = font_pixel_size(
        sizing_height(snapshot.display_height, height), config.scale,
        config.font_size);
    wanted_reference_ = config.font_size;
    wanted_font_path_ = config.font_path;
    wanted_font_path_strong_ = config.font_path_strong;
    // Spacing lives in the style, so the style follows an edited settings file
    // even when the font size has not moved.
    configure_style(config);

    if (!config.enabled) {
        return;
    }

    // The message's words, read post-present into note_body_ and handed to the
    // next frame: this one is inside vkQueuePresentKHR, where the layer does no
    // file work (rule 8). One frame late is invisible for a toast, and a process
    // that never draws one never opens the note segment (vocem/note.h).
    if (notification_wanted(snapshot, config, now)) {
        wanted_note_serial_ = snapshot.notification.serial;
        std::snprintf(const_cast<Snapshot&>(snapshot).notification.body,
                      sizeof(snapshot.notification.body), "%s", note_body_);
    } else {
        wanted_note_serial_ = 0;
    }

    // Which colour emoji this frame's text needs -- after the words are in the
    // snapshot, since the segment's copy of the body is left empty. Only
    // noted here, with no file read; the bank is asked and the fold happens in
    // process_uploads(). The snapshot is this frame's own copy, and noting
    // rewrites a sequence into its key only once the atlas has that key's
    // glyph, or it would draw as '?'.
    fonts_note_emoji_in(const_cast<Snapshot&>(snapshot));

    // The backend's NewFrame is never called: its only job is creating a font
    // texture when it has none, and with the cache owning that texture it
    // never has one -- calling it would upload the whole atlas (43 MB at a
    // 2160-line display) with a queue wait inside the present (rule 10).
    ImGui::NewFrame();
    AvatarProvider* avatars = textures_.ready() ? avatar_adapter_ : nullptr;
    // A frame can be for the toast alone (a message outside a voice channel),
    // and the panel must not be handed a snapshot with nobody in it.
    if (panel_wanted(snapshot, config)) {
        build_panel(snapshot, config, width, height, avatars, now);
    }
    build_notification(snapshot, config, width, height, avatars, now);
    ImGui::Render();

    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), command_buffer, pipeline);
    session().frame_drawn();
}

void OverlayRenderer::process_uploads(VkDevice device, VkQueue queue) {
    std::lock_guard<std::mutex> guard(lock_);
    if (!backend_ready_ || device != device_ || queue != queue_) {
        return;
    }

    // The Debug section's frame counters; the file write is at most once every
    // few seconds.
    session().frame_seen();

    // The size or scale changed: rasterise the atlas again rather than stretch
    // it. Only here, after the present: replacing the font texture waits on the
    // queue, which would deadlock inside it. A new colour emoji answers true
    // too, folded into reserved space and uploaded as its squares with no
    // wait; the log line says which of the two it was.
    if (font_retry_.due(monotonic_seconds())) {
        if (upload_font_texture(true)) {
            font_retry_.succeeded();
            VOCEM_RLOG("font texture went up on a later attempt: drawing again");
        } else {
            font_retry_.failed(monotonic_seconds());
        }
    }
    const uint32_t builds_before = fonts_build_count();
    if (wanted_font_size_ > 0.0f &&
        ensure_fonts(wanted_font_size_, wanted_reference_, wanted_font_path_.c_str(),
                     wanted_font_path_strong_.c_str())) {
        configure_style(config_.current());
        const bool rebuilt = fonts_build_count() != builds_before;
        // While a whole upload is owed, a fold would copy its squares into the
        // old image and call that success: it goes up whole instead.
        if (!upload_font_texture(rebuilt || font_retry_.owed())) {
            // The GPU's image no longer matches the atlas: nothing is drawn
            // with it until an upload works (draw() asks), rather than text from
            // the wrong squares or a descriptor that is gone.
            font_retry_.failed(monotonic_seconds());
            VOCEM_RLOG("font texture upload failed at %.1f px: not drawing until it goes up, "
                       "tried again every second", wanted_font_size_);
        } else if (rebuilt) {
            font_retry_.succeeded();  // the whole atlas went up: the image matches again
            VOCEM_RLOG("font atlas rebuilt at %.1f px", wanted_font_size_);
        } else {
            VOCEM_RLOG("colour emoji folded into the font atlas");
        }
    }

    // Why there are no colour emoji, or why the text is not in the configured
    // font: said once per change, the same way on both paths.
    session().say_font_statuses();

    // The words for the toast the next frame will draw, here where file work is
    // allowed. A message that arrived without them is said once, in the session.
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
    // Nothing below may run while the GPU still reads what it destroys:
    // textures_.shutdown() frees the images (the font atlas's among them) and
    // the command pool, ImGui_ImplVulkan_Shutdown() the vertex ring, the
    // pipeline and the descriptor pool, and neither waits for the overlay's
    // submissions. This is called from arbitrary presents (the daemon
    // stopping, a hand-over from another device's present), so
    // vkDeviceWaitIdle is not an option: it needs every queue externally
    // synchronised. The overlay's per-image fences are waited for by the
    // caller (release_renderer_locked, the only way in), the cache's uploads by
    // textures_.shutdown(); vkWaitForFences needs no queue's synchronisation.
    //
    // The next device's first frame must not measure the gap between devices
    // as one animation step.
    session().reset_clock();
    // A rasterisation still running uses the context and atlas about to go: it
    // finishes first (at most one atlas build, and only at teardown).
    atlas_worker().join();
    textures_.shutdown();
    if (backend_ready_) {
        ImGui_ImplVulkan_Shutdown();
        backend_ready_ = false;
    }
    if (context_created_) {
        ImGui::DestroyContext();
        context_created_ = false;
        context_ready_ = false;
    }
    // The next prepare() belongs to a different device: the function table is
    // loaded again, and a failure against the old device says nothing about
    // the new one.
    functions_loaded_ = false;
    failed_ = false;
    failed_device_ = VK_NULL_HANDLE;
    failed_queue_ = VK_NULL_HANDLE;
    font_retry_ = UploadRetry();
    device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    format_ = VK_FORMAT_UNDEFINED;
}

}  // namespace vocem

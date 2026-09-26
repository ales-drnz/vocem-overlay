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
    // cheaper. Instance-level ones go straight to the instance chain: asking
    // vkGetDeviceProcAddr for a physical-device or surface function is outside
    // what it promises, and the validation layer says so once per name the
    // first time it sits below the overlay (entry 192). The test is by name
    // because the caller (ImGui's loader, the texture cache) only has names;
    // vkGetInstanceProcAddr answers device functions correctly too, so a name
    // this sends the long way round costs a trampoline and nothing else.
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

    // ImGui stores these globally, so there is one backend per process and it
    // lives on one device. A second device that presents is passed through by
    // the layer while the first presents, and the backend moves to it -- a
    // shutdown() and a fresh prepare(), which loads these again -- once the
    // first has been silent for the hand-over interval (vocem_layer.cpp).
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
        if (!context_created_) {
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
            context_created_ = true;

            // Built before the backend exists, so the upload below sends the
            // atlas we want rather than the default bitmap. The size comes from
            // the target, which is why this cannot happen at library load time.
            //
            // OFF the game's thread (entry 192). The context is created first
            // because ensure_fonts() reaches the atlas through ImGui::GetIO(),
            // and GImGui is a plain global the new thread sees from its start.
            // The RGBA widening goes with it: 11 ms more that the upload would
            // otherwise pay on the game's thread. Everything the worker reads is
            // copied into it here.
            const Config& config = config_.current();
            atlas_job_.pixels = font_pixel_size(target.height, config.scale, config.font_size);
            atlas_job_.reference = config.font_size;
            atlas_job_.body = config.font_path;
            atlas_job_.strong = config.font_path_strong;
            atlas_rasterised_.store(false, std::memory_order_relaxed);
            {
                std::lock_guard<std::mutex> worker_guard(worker_lock_);
                atlas_worker_running_ =
                    pthread_create(&atlas_worker_, nullptr, &OverlayRenderer::rasterise_atlas,
                                   this) == 0;
            }
            if (atlas_worker_running_) {
                // Named, so a stack in a game's crash report says whose it is.
                pthread_setname_np(atlas_worker_, "vocem-atlas");
                VOCEM_RLOG("rasterising the font atlas at %.0f px off the game's thread",
                           static_cast<double>(atlas_job_.pixels));
                return false;  // asked again on the next frame with something on it
            }
            // No thread to be had (a sandbox that refuses clone, resources): the
            // build happens here, as it always used to.
            VOCEM_RLOG("no thread for the font atlas; rasterising it on the game's thread");
            rasterise_atlas(this);
        }
        if (!atlas_rasterised_.load(std::memory_order_acquire)) {
            return false;  // still rasterising; the game goes on presenting meanwhile
        }
        join_atlas_worker();  // finished: this returns at once
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
        //
        // By the texture cache (entry 192): it owns the font texture so that a
        // new colour emoji can be copied into it as a 32x32 square instead of
        // replacing 64 MB between two vkQueueWaitIdle. The cache is initialised
        // here, before the atlas, for that reason; it used to come after, as
        // the avatars' alone.
        //
        // And without it there is no backend. The fallback that stood here --
        // plain circles for the faces, and the font texture left to ImGui's
        // stock ImGui_ImplVulkan_CreateFontsTexture -- allocated that upload's
        // command buffer through the chain and never registered it with the
        // loader (rule 5, entry 42): measured with the witness refusing the
        // cache's sampler and the validation layer below the overlay, "The
        // VkDevice dispatch handle was not found and Validation will crash",
        // and the process ended on SIGABRT. The cache fails only where the
        // device refuses a sampler, a command pool or a memory type -- a
        // device the overlay has no business drawing on -- so this declines,
        // says so once, and remembers it until another device arrives.
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
            failed_ = true;
            return false;
        }
        if (!upload_font_texture(true) || g_backend_failed) {
            VOCEM_RLOG("font atlas upload failed");
            textures_.shutdown();
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
        queue_ = target.queue;
        format_ = target.format;
    }
    return true;
}

bool OverlayRenderer::upload_font_texture(bool whole) {
    ImGuiIO& io = ImGui::GetIO();
    unsigned char* pixels = nullptr;
    int width = 0;
    int height = 0;
    io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
    if (!pixels || width <= 0 || height <= 0) {
        return false;
    }
    if (!whole) {
        AtlasRegion regions[kMaxFoldedRegions];
        const uint32_t count = fonts_take_folded(regions, kMaxFoldedRegions);
        if (textures_.update_font_atlas(pixels, static_cast<uint32_t>(width),
                                        static_cast<uint32_t>(height), regions, count)) {
            // Said, so the arrivals scene can count it: a fold that went up
            // whole would pass every other check (entry 192).
            VOCEM_RLOG("font texture: %u folded square(s) copied in place", count);
            return true;
        }
        // The image does not match the atlas: replace it whole, which is
        // always correct and is what every fold cost before.
    } else {
        fonts_take_folded(nullptr, 0);  // already in the whole atlas
    }
    const ImTextureID id = textures_.upload_font_atlas(pixels, static_cast<uint32_t>(width),
                                                       static_cast<uint32_t>(height));
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
    if (!backend_ready_ || font_retry_at_ > 0.0 || device != device_ || queue != queue_) {
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
    // never be noted at all. Only noted here, with no file read -- a codepoint
    // never seen is queued, and the bank is asked in process_uploads(), after
    // the present, where the fold happens too, like every other atlas change.
    // The snapshot is this frame's own copy (the same const_cast the body
    // above makes), and the noting rewrites an emoji sequence into its key --
    // once the atlas has that key's glyph, which is from the frame after the
    // fold on this path: a sequence rewritten earlier drew as '?'.
    fonts_note_emoji_in(const_cast<Snapshot&>(snapshot));

    // The backend's NewFrame is never called: it does one thing, create its
    // own font texture if it has none, and with the cache owning that texture
    // (entry 192) it has none by design -- calling it would build a second,
    // stock one here, inside the present, 64 MB and a queue wait, which is
    // rule 10's whole subject.
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

void OverlayRenderer::process_uploads(VkDevice device, VkQueue queue) {
    std::lock_guard<std::mutex> guard(lock_);
    if (!backend_ready_ || device != device_ || queue != queue_) {
        return;
    }

    // The Debug section's frame and draw counters. This is the post-present
    // phase, where file work is allowed; the write itself happens at most once
    // every few seconds.
    session().frame_seen();

    // Resolution changed, or the user moved the size slider: rasterise the atlas
    // again at the new size instead of stretching the old one. Safe here and only
    // here -- replacing the font texture waits on the queue first (the cache's
    // upload_font_atlas), which is legal after the present has returned and
    // would deadlock inside it.
    // A new colour emoji answers true too, FOLDED into space the build
    // reserved rather than rasterised (entry 191), and only its squares go up,
    // with no wait (entry 192). The line says which of the two it was, because
    // a log that called a fold a rebuild would be counting the cost that is gone.
    if (font_retry_at_ > 0.0 && monotonic_seconds() >= font_retry_at_) {
        if (upload_font_texture(true)) {
            font_retry_at_ = 0.0;
            VOCEM_RLOG("font texture went up on a later attempt: drawing again");
        } else {
            font_retry_at_ = monotonic_seconds() + 1.0;
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
        if (!upload_font_texture(rebuilt || font_retry_at_ > 0.0)) {
            // The image the GPU holds no longer matches the atlas, whether the
            // cache kept the old one alive or had nothing left to keep: nothing
            // is drawn with it until an upload works (draw() asks), rather than
            // text drawn from the wrong squares or a descriptor that is gone.
            font_retry_at_ = monotonic_seconds() + 1.0;
            VOCEM_RLOG("font texture upload failed at %.1f px: not drawing until it goes up, "
                       "tried again every second", wanted_font_size_);
        } else if (rebuilt) {
            font_retry_at_ = 0.0;  // the whole atlas went up: the image matches again
            VOCEM_RLOG("font atlas rebuilt at %.1f px", wanted_font_size_);
        } else {
            VOCEM_RLOG("colour emoji folded into the font atlas");
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

void* OverlayRenderer::rasterise_atlas(void* self) {
    auto* renderer = static_cast<OverlayRenderer*>(self);
    const AtlasJob& job = renderer->atlas_job_;
    ensure_fonts(job.pixels, job.reference, job.body.c_str(), job.strong.c_str());
    unsigned char* rgba = nullptr;
    int width = 0;
    int height = 0;
    fonts_atlas()->GetTexDataAsRGBA32(&rgba, &width, &height);
    renderer->atlas_rasterised_.store(true, std::memory_order_release);
    return nullptr;
}

void OverlayRenderer::join_atlas_worker() {
    std::lock_guard<std::mutex> worker_guard(worker_lock_);
    if (atlas_worker_running_) {
        pthread_join(atlas_worker_, nullptr);
        atlas_worker_running_ = false;
    }
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
    // present old; textures_.shutdown() frees the images -- the font atlas's
    // among them (entry 192) -- and the command pool it reads, and
    // ImGui_ImplVulkan_Shutdown() frees the vertex ring, its own font image
    // where it made one, the pipeline and the descriptor pool
    // (imgui_impl_vulkan.cpp has no wait of its own -- checked). Entry 131
    // fixed exactly this shape for the second-device case and it came back
    // through a door nobody had yet.
    //
    // The wait was a vkDeviceWaitIdle here, under a comment saying its
    // external synchronisation was had. It was not: vkDeviceWaitIdle wants
    // every queue of the device externally synchronised, and a present holds
    // only the queue it was made on -- and the hand-over to a second device
    // (entry 210 on this side) calls this from ANOTHER device's present, which
    // holds none of this one's. What reads these objects on the GPU is two
    // things, each with fences of its own: the overlay's submissions, one per
    // swapchain image, which the layer waits for before calling this
    // (release_renderer_locked, the only way it is called), and the texture
    // cache's uploads, which textures_.shutdown() waits for below.
    // vkWaitForFences needs no queue's synchronisation at all.
    //
    // The next device's first frame must not measure the gap between devices as
    // one animation step.
    session().reset_clock();
    // A rasterisation still running uses the context and the atlas that are
    // about to go: it finishes first. At most the ~113 ms the build takes, and
    // only when the overlay is released, the renderer's device dies, or the
    // last instance goes inside that window.
    join_atlas_worker();
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
    // The next prepare() belongs to a different device, so the backend's function
    // table is loaded again rather than kept from the destroyed one -- and a
    // failure against the old device says nothing about the new one.
    functions_loaded_ = false;
    failed_ = false;
    font_retry_at_ = 0.0;
    device_ = VK_NULL_HANDLE;
    queue_ = VK_NULL_HANDLE;
    format_ = VK_FORMAT_UNDEFINED;
}

}  // namespace vocem

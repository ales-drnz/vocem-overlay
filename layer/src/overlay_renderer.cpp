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
#include "vocem/apps.h"
#include "vocem/clock.h"
#include "vocem/journal.h"
#include "vocem/fonts.h"
#include "vocem/panel.h"

// Failures in here are silent by design in release builds -- the overlay simply
// does not appear -- so they must be traceable when VOCEM_DEBUG is set.
// The env is read once, as every other component's logger reads it: this
// macro used to ask getenv on every log line.
inline bool vocem_render_debug() {
    static const bool enabled = [] {
        const char* env = std::getenv("VOCEM_DEBUG");
        return env && env[0] == '1';
    }();
    return enabled;
}

#define VOCEM_RLOG(...)                                            \
    do {                                                           \
        if (vocem_render_debug()) {                                \
            std::fprintf(stderr, "[vocem/render] " __VA_ARGS__);   \
            std::fputc('\n', stderr);                              \
        }                                                          \
    } while (0)

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
    if (!target.device || !target.render_pass || !target.gdpa || !target.gipa) {
        VOCEM_RLOG("incomplete target: device=%p render_pass=%p gdpa=%p gipa=%p",
                   (void*)target.device, (void*)target.render_pass, (void*)target.gdpa,
                   (void*)target.gipa);
        return false;
    }

    if (!load_vulkan_functions(target)) {
        return false;
    }

    if (!context_ready_) {
        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
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
    // arrives through vocem_DestroyDevice -> shutdown(); a recreated swapchain
    // keeps working because its new render pass is compatible by Vulkan's own
    // rules, not because anything in here notices.
    if (!backend_ready_) {
        ImGui_ImplVulkan_InitInfo info{};
        info.ApiVersion = VK_API_VERSION_1_1;
        info.Instance = target.instance;
        info.PhysicalDevice = target.physical_device;
        info.Device = target.device;
        info.QueueFamily = target.queue_family;
        info.Queue = target.queue;
        info.RenderPass = target.render_pass;
        info.MinImageCount = target.image_count < 2 ? 2 : target.image_count;
        info.ImageCount = info.MinImageCount;
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
            return false;
        }
        VOCEM_RLOG("backend ready (%u images, queue family %u)", info.ImageCount,
                   target.queue_family);
        // The session's journal (vocem/journal.h): opened at the first frame
        // the overlay draws in this process, closed into history on a clean
        // exit by the layer's destructor -- and left behind, still `.running`,
        // by a crash, which is the detection.
        vocem::journal_begin("vulkan", vocem::process_name().c_str());
        {
            char note[300];
            std::snprintf(note, sizeof(note), "drawing: %s",
                          vocem::game_verdict().reason.c_str());
            vocem::journal_note(note);
        }
        vocem::journal_note("Vulkan backend ready");
        backend_ready_ = true;

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
    if (!backend_ready_) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(static_cast<float>(width), static_cast<float>(height));

    const double now = monotonic_seconds();
    const double delta = last_frame_seconds_ > 0.0 ? now - last_frame_seconds_ : 1.0 / 60.0;
    last_frame_seconds_ = now;
    // A stalled or hitching game must not feed ImGui a zero or negative step.
    io.DeltaTime = delta > 0.0001 ? static_cast<float>(delta) : 1.0f / 60.0f;

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
    fonts_note_emoji_in(snapshot);

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
    ++frames_drawn_;
}

void OverlayRenderer::process_uploads() {
    if (!backend_ready_) {
        return;
    }

    // The Debug section's frame and draw counters. This is the post-present
    // phase, where file work is allowed; the write itself happens at most once
    // every few seconds.
    ++frames_seen_;
    if (const double now = monotonic_seconds(); now - last_stat_seconds_ >= 5.0) {
        last_stat_seconds_ = now;
        journal_stat(frames_seen_, frames_drawn_);
    }

    // Resolution changed, or the user moved the size slider: rasterise the atlas
    // again at the new size instead of stretching the old one. Safe here and only
    // here -- ImGui_ImplVulkan_CreateFontsTexture waits on the queue before
    // replacing the texture, which is legal after the present has returned and
    // would deadlock inside it.
    if (wanted_font_size_ > 0.0f && ensure_fonts(wanted_font_size_, wanted_reference_)) {
        configure_style(config_.current());
        if (!ImGui_ImplVulkan_CreateFontsTexture()) {
            VOCEM_RLOG("font texture upload failed at %.1f px", wanted_font_size_);
        } else {
            VOCEM_RLOG("font atlas rebuilt at %.1f px", wanted_font_size_);
        }
    }

    // Why there are no colour emoji, said once. A feature that quietly does not
    // happen reads exactly like one nobody asked for.
    if (const char* status = fonts_emoji_status(); status != emoji_status_said_) {
        emoji_status_said_ = status;
        if (status) {
            VOCEM_RLOG("no colour emoji: %s", status);
        }
    }

    // The words for the toast the next frame will draw. Here rather than in
    // draw(): this is the phase where file work is allowed.
    if (wanted_note_serial_ != 0) {
        std::snprintf(note_body_, sizeof(note_body_), "%s",
                      note_.body_for(wanted_note_serial_));
        if (note_body_[0] == '\0' && said_empty_note_ != wanted_note_serial_) {
            said_empty_note_ = wanted_note_serial_;
            VOCEM_RLOG("message %llu has no words here: the note segment is empty or "
                       "unreachable from this process",
                       (unsigned long long)wanted_note_serial_);
        }
    } else if (note_body_[0] != '\0') {
        note_.forget();
        std::memset(note_body_, 0, sizeof(note_body_));
    }

    textures_.process_pending();
}

void OverlayRenderer::shutdown() {
    // The next device's first frame must not measure the gap between devices as
    // one animation step.
    last_frame_seconds_ = 0.0;
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
    // table is loaded again rather than kept from the destroyed one.
    functions_loaded_ = false;
}

}  // namespace vocem

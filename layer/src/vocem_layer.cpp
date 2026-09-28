// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Vocem Overlay - Vulkan layer.
//
// The plumbing: dispatch tables, per-swapchain resources, queue/family
// tracking, semaphore chaining. What gets painted lives in overlay_renderer /
// common/src/panel.cpp; this file decides where and when, under these rules:
//   * Never block in vkQueuePresentKHR. No per-frame I/O, no allocation on the
//     hot path (the exact claim, with its named exceptions, is at the
//     `drawable` flag in vocem_QueuePresentKHR).
//   * Per-image state is indexed by swapchain image index, never by acquisition
//     order.
//   * If anything we need is missing, degrade to a pure pass-through. A layer
//     must never be the reason a game fails to start.
//   * Dispatchable objects we create (command buffers) must be registered with
//     the loader via pfnSetDeviceLoaderData, or their dispatch will crash.
// The bullets are unnumbered on purpose: a "rule N" cited in this directory
// uses the project notes' numbering (index rule 4, loader-data rule 5).

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "hdr_pipeline.h"
#include "overlay_renderer.h"
#include "vocem/apps.h"
#include "vocem/atlas_owner.h"
#include "vocem/clock.h"
#include "vocem/config.h"
#include "vocem/journal.h"
#include "vocem/draw_decision.h"
#include "vocem/flatpak.h"
#include "vocem/fonts.h"
#include "vocem/overlay_log.h"
#include "vocem/overlay_session.h"
#include "vocem/panel.h"
#include "vocem/shared_state.h"
#include "vocem/shm.h"
#include "vocem/state_poll.h"

// Older Vulkan SDKs defined this in vk_sdk_platform.h, which no longer ships.
// The whole library is built with hidden visibility, so the three loader entry
// points have to opt back in explicitly.
#ifndef VK_LAYER_EXPORT
#define VK_LAYER_EXPORT __attribute__((visibility("default")))
#endif

namespace {

// ---------------------------------------------------------------------------
// Logging. Off unless VOCEM_DEBUG=1, so release runs stay silent; the one
// logger both injected paths share (vocem/overlay_log.h), so VOCEM_LOG_FILE
// reaches a Vulkan game under a launcher that swallows its stderr too.
// ---------------------------------------------------------------------------

#define VOCEM_LOG(...) VOCEM_OVERLAY_LOG("vocem", __VA_ARGS__)

// ---------------------------------------------------------------------------
// Dispatch tables. Only the entry points we actually call are stored; adding a
// function means adding it here and in the loader lookup below.
// ---------------------------------------------------------------------------

struct InstanceDispatch {
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr = nullptr;
    PFN_vkDestroyInstance DestroyInstance = nullptr;
};

struct DeviceDispatch {
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr = nullptr;
    PFN_vkDestroyDevice DestroyDevice = nullptr;
    PFN_vkGetDeviceQueue GetDeviceQueue = nullptr;
    PFN_vkGetDeviceQueue2 GetDeviceQueue2 = nullptr;
    PFN_vkCreateSwapchainKHR CreateSwapchainKHR = nullptr;
    PFN_vkDestroySwapchainKHR DestroySwapchainKHR = nullptr;
    PFN_vkGetSwapchainImagesKHR GetSwapchainImagesKHR = nullptr;
    PFN_vkQueuePresentKHR QueuePresentKHR = nullptr;
    PFN_vkQueueSubmit QueueSubmit = nullptr;
    PFN_vkCreateImageView CreateImageView = nullptr;
    PFN_vkDestroyImageView DestroyImageView = nullptr;
    PFN_vkCreateRenderPass CreateRenderPass = nullptr;
    PFN_vkDestroyRenderPass DestroyRenderPass = nullptr;
    PFN_vkCreateFramebuffer CreateFramebuffer = nullptr;
    PFN_vkDestroyFramebuffer DestroyFramebuffer = nullptr;
    PFN_vkCreateCommandPool CreateCommandPool = nullptr;
    PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
    PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
    PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
    PFN_vkResetCommandBuffer ResetCommandBuffer = nullptr;
    PFN_vkCmdBeginRenderPass CmdBeginRenderPass = nullptr;
    PFN_vkCmdEndRenderPass CmdEndRenderPass = nullptr;
    PFN_vkCreateSemaphore CreateSemaphore = nullptr;
    PFN_vkDestroySemaphore DestroySemaphore = nullptr;
    PFN_vkCreateFence CreateFence = nullptr;
    PFN_vkDestroyFence DestroyFence = nullptr;
    PFN_vkWaitForFences WaitForFences = nullptr;
    PFN_vkResetFences ResetFences = nullptr;
    // For the converting pipeline (hdr_pipeline.h): built once per swapchain
    // whose colour space -- or whose format -- needs the overlay's colours
    // written in something other than what ImGui produces.
    PFN_vkCreateShaderModule CreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout = nullptr;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline DestroyPipeline = nullptr;
};

// Resources owned per swapchain. Rebuilt whenever the application recreates its
// swapchain (resize, mode change, minimise) and torn down with it.
struct SwapchainData {
    VkDevice device = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    // The colour space decides how the overlay's sRGB colours are encoded on
    // the way out: on HDR10 an unconverted 1.0 is a ten-thousand-nit white.
    VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    vocem::HdrPipeline hdr;
    VkExtent2D extent{};
    VkRenderPass render_pass = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    uint32_t queue_family = UINT32_MAX;

    // All of these are indexed by swapchain image index.
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;
    std::vector<VkCommandBuffer> command_buffers;
    std::vector<VkSemaphore> semaphores;
    std::vector<VkFence> fences;
    // Whether our own submission for this image is outstanding. Recording can
    // fail after the fence is reset (begin, end or submit failing), and a wait
    // on a fence nothing will signal would block forever under g_lock.
    std::vector<uint8_t> submitted;

    bool usable = false;   // false => pass through untouched
    bool attempted = false;  // only try to build resources once per swapchain
    // A journal line the build wants written, held until the post-present
    // phase: the build runs inside the present, and a journal write is a file
    // syscall (rule 8).
    bool colour_note_pending = false;
    // "Not drawing here, and why", said once per swapchain rather than per
    // frame, for each of the pass-through reasons draw_overlay() can find.
    bool said_pass_through = false;

    // Whether any of our submits on this swapchain ever signalled one of the
    // semaphores above, which a present then waited on. What decides whether
    // the teardown may destroy them at once (destroy_swapchain_resources).
    bool signalled_any = false;
};

struct DeviceData {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    DeviceDispatch disp;
    PFN_vkSetDeviceLoaderData set_device_loader_data = nullptr;
    std::unordered_map<VkQueue, uint32_t> queue_families;
    // The capabilities of every queue family, indexed by family, asked once at
    // device creation: a family without GRAPHICS cannot take the overlay's
    // render pass. Empty when the query was unavailable; the family is then
    // taken on trust.
    std::vector<VkQueueFlags> family_flags;
    // Semaphores of swapchains already destroyed, which a present may still
    // be waiting on: destroyed with the device, or once more than
    // kMaxRetiredSemaphores are kept (destroy_swapchain_resources says why).
    std::vector<VkSemaphore> retired_semaphores;
};

// The loader hands us dispatchable handles; keying on the raw pointer is the
// conventional way for a layer to find its own per-object state.
std::mutex g_lock;
std::unordered_map<void*, InstanceDispatch> g_instances;
std::unordered_map<void*, DeviceData> g_devices;
std::unordered_map<VkSwapchainKHR, SwapchainData> g_swapchains;

void* dispatch_key(void* handle) { return *reinterpret_cast<void**>(handle); }

// The eight functions the HDR pipeline borrows from the dispatch, in one
// place for the create and the destroy path: a member missing on the destroy
// side makes complete() false and leaks a pipeline per swapchain rebuild.
vocem::HdrDeviceFunctions hdr_functions(const DeviceDispatch& d) {
    vocem::HdrDeviceFunctions fn;
    fn.CreateShaderModule = d.CreateShaderModule;
    fn.DestroyShaderModule = d.DestroyShaderModule;
    fn.CreateDescriptorSetLayout = d.CreateDescriptorSetLayout;
    fn.DestroyDescriptorSetLayout = d.DestroyDescriptorSetLayout;
    fn.CreatePipelineLayout = d.CreatePipelineLayout;
    fn.DestroyPipelineLayout = d.DestroyPipelineLayout;
    fn.CreateGraphicsPipelines = d.CreateGraphicsPipelines;
    fn.DestroyPipeline = d.DestroyPipeline;
    return fn;
}



// The attach/detach/read loop is shared with the GL path (vocem/state_poll.h).
void layer_poll_log(const char* line) { VOCEM_LOG("%s", line); }

vocem::StatePoll g_state{&layer_poll_log};

DeviceData* find_device(void* dispatchable) {
    auto it = g_devices.find(dispatch_key(dispatchable));
    return it == g_devices.end() ? nullptr : &it->second;
}

// ---------------------------------------------------------------------------
// Whose present this is. The renderer lives on one device and uploads on one
// queue (OverlayRenderer::owns); recording another device's frames with its
// vertex ring, pipeline and font image is a validation error, then a crash.
// A present that is not the owner's is passed through, said once; once the
// owner has not presented for HandOver::kSeconds the backend moves to the one
// that does, so a game that loads on one device and plays on another still
// gets the overlay.
//
// The owner is the QUEUE, not only the device: one swapchain presented from
// two queues in turn has the overlay in every other frame (vk_present_draw's
// alternate-queue scene). Drawing for the whole device is not done: the
// texture cache orders its copies into the font image on the renderer's queue
// alone, so a draw on another queue would be unordered against them.
// Everything here is guarded by g_lock.
// ---------------------------------------------------------------------------

// Who holds the renderer, when it last presented and who was told it has no
// overlay (vocem/atlas_owner.h): the transition is one spelling with the GL
// path's. Its clock starts when a renderer comes up, or fails to
// (vocem_QueuePresentKHR, after prepare()); until then nothing is Abandoned.
vocem::HandOver g_hand_over;
// The device and queue a renderer could not be made on, while they hold the
// overlay (HandOver::Holding::Failed): a failure is that device's, not the
// process's, and goes when it falls silent or is destroyed (entry 263's rule,
// tests/vk_present_draw.cpp's failed-presenter scene).
VkDevice g_failed_device = VK_NULL_HANDLE;
VkQueue g_failed_queue = VK_NULL_HANDLE;

using Presenter = vocem::HandOver::Presenter;

bool failed_here(VkDevice device, VkQueue queue) {
    return g_hand_over.holding() == vocem::HandOver::Holding::Failed &&
           device == g_failed_device && queue == g_failed_queue;
}

Presenter whose_present(VkDevice device, VkQueue queue) {
    const bool failed = g_hand_over.holding() == vocem::HandOver::Holding::Failed;
    // Nobody owns a renderer that is neither up nor refused: whoever presents
    // next builds it.
    if (!failed && !vocem::renderer().ready()) {
        return Presenter::Owner;
    }
    const double now = vocem::monotonic_seconds();
    const Presenter who = g_hand_over.present(
        failed ? failed_here(device, queue) : vocem::renderer().owns(device, queue), now);
    if (who == Presenter::Foreign && g_hand_over.first_word_with(device, queue)) {
        if (failed) {
            VOCEM_LOG("not drawing on device %p queue %p: the overlay is held by device %p, "
                      "where it could not be made and which presented %.1f s ago",
                      static_cast<void*>(device), static_cast<void*>(queue),
                      static_cast<void*>(g_failed_device), now - g_hand_over.seen());
        } else {
            VOCEM_LOG("not drawing on device %p queue %p: the overlay's renderer lives on device "
                      "%p and its queue, which presented %.1f s ago", static_cast<void*>(device),
                      static_cast<void*>(queue), static_cast<void*>(vocem::renderer().device()),
                      g_hand_over.seen() > 0.0 ? now - g_hand_over.seen() : 0.0);
        }
    }
    return who;
}

// Waits for every overlay submission still in flight on `device`, by the
// per-image fences the layer submitted them with; those command buffers read
// the renderer's resources (the texture cache's shutdown() waits for its own
// uploads). vkWaitForFences needs no queue's external synchronisation, which
// vkDeviceWaitIdle would.
void wait_for_overlay_work(VkDevice device) {
    DeviceData* dev = find_device(device);
    if (!dev || !dev->disp.WaitForFences) {
        return;
    }
    for (auto& entry : g_swapchains) {
        SwapchainData& sc = entry.second;
        if (sc.device != device) {
            continue;
        }
        for (size_t i = 0; i < sc.submitted.size() && i < sc.fences.size(); ++i) {
            if (sc.submitted[i] && sc.fences[i] != VK_NULL_HANDLE) {
                dev->disp.WaitForFences(device, 1, &sc.fences[i], VK_TRUE, UINT64_MAX);
            }
        }
    }
}

// The one way the renderer is torn down: every overlay submission that reads
// it waited for first (OverlayRenderer::shutdown says why that is the
// caller's), then the backend, the context and the texture cache. The atlas is
// not the renderer's and stays (entry 144); callers that mean to hand it back
// call fonts_release() after this.
void release_renderer_locked() {
    if (VkDevice owner = vocem::renderer().device()) {
        wait_for_overlay_work(owner);
    }
    vocem::renderer().shutdown();
    g_hand_over.let_go();
    g_failed_device = VK_NULL_HANDLE;
    g_failed_queue = VK_NULL_HANDLE;
}

// ---------------------------------------------------------------------------
// Chain helpers
// ---------------------------------------------------------------------------

VkLayerInstanceCreateInfo* find_instance_chain_info(const VkInstanceCreateInfo* info,
                                                   VkLayerFunction func) {
    auto* item = static_cast<const VkLayerInstanceCreateInfo*>(info->pNext);
    while (item) {
        if (item->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
            item->function == func) {
            return const_cast<VkLayerInstanceCreateInfo*>(item);
        }
        item = static_cast<const VkLayerInstanceCreateInfo*>(item->pNext);
    }
    return nullptr;
}

VkLayerDeviceCreateInfo* find_device_chain_info(const VkDeviceCreateInfo* info,
                                                VkLayerFunction func) {
    auto* item = static_cast<const VkLayerDeviceCreateInfo*>(info->pNext);
    while (item) {
        if (item->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
            item->function == func) {
            return const_cast<VkLayerDeviceCreateInfo*>(item);
        }
        item = static_cast<const VkLayerDeviceCreateInfo*>(item->pNext);
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Instance
// ---------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL vocem_CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                    const VkAllocationCallbacks* pAllocator,
                                                    VkInstance* pInstance) {
    // Before anything derives a path: the configuration and the state are both
    // on the far side of the sandbox when the game is a Flatpak, and this is
    // where the three lookups get pointed at the bridge (vocem/flatpak.h).
    vocem::session().enter_flatpak_bridge_once();

    VkLayerInstanceCreateInfo* link = find_instance_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);
    if (!link || !link->u.pLayerInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    auto create_instance =
        reinterpret_cast<PFN_vkCreateInstance>(next_gipa(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!create_instance) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // Advance the chain for the next layer down before calling through.
    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    VkResult result = create_instance(pCreateInfo, pAllocator, pInstance);
    if (result != VK_SUCCESS) {
        return result;
    }

    InstanceDispatch disp;
    disp.instance = *pInstance;
    disp.GetInstanceProcAddr = next_gipa;
    disp.DestroyInstance =
        reinterpret_cast<PFN_vkDestroyInstance>(next_gipa(*pInstance, "vkDestroyInstance"));

    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_instances[dispatch_key(*pInstance)] = disp;
    }

    VOCEM_LOG("instance created");
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vocem_DestroyInstance(VkInstance instance,
                                                 const VkAllocationCallbacks* pAllocator) {
    PFN_vkDestroyInstance destroy = nullptr;
    bool last = false;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_instances.find(dispatch_key(instance));
        if (it != g_instances.end()) {
            destroy = it->second.DestroyInstance;
            g_instances.erase(it);
            last = g_instances.empty();
        }
    }
    if (destroy) {
        destroy(instance, pAllocator);
    }
    // The loader unloads this library after the last instance goes, and the
    // font atlas (kept for the life of the process) is reachable
    // only through this library's statics: hand it back now, or every
    // create/destroy cycle leaves an atlas mapped. shutdown() also joins a
    // build still running and destroys a context whose device died first.
    // Only where the atlas was ever made -- a process that never drew has
    // nothing, and asking would construct the renderer and atlas objects. Made,
    // not built: an instance destroyed during the first build still holds it.
    if (last && vocem::fonts_atlas_made()) {
        {
            std::lock_guard<std::mutex> guard(g_lock);
            release_renderer_locked();
        }
        vocem::fonts_release();
        VOCEM_LOG("the last instance is gone: the font atlas handed back");
    }
}

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------

// Defined with the swapchain code below; vocem_DestroyDevice needs it first.
void destroy_swapchain_resources(DeviceData& dev, SwapchainData& sc, bool device_going);

VKAPI_ATTR VkResult VKAPI_CALL vocem_CreateDevice(VkPhysicalDevice physicalDevice,
                                                  const VkDeviceCreateInfo* pCreateInfo,
                                                  const VkAllocationCallbacks* pAllocator,
                                                  VkDevice* pDevice) {
    VkLayerDeviceCreateInfo* link = find_device_chain_info(pCreateInfo, VK_LAYER_LINK_INFO);
    if (!link || !link->u.pLayerInfo) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetInstanceProcAddr next_gipa = link->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr next_gdpa = link->u.pLayerInfo->pfnNextGetDeviceProcAddr;

    // The loader passes a callback we must use on every dispatchable object we
    // create ourselves. Without it, command buffer dispatch would crash.
    VkLayerDeviceCreateInfo* callback_info =
        find_device_chain_info(pCreateInfo, VK_LOADER_DATA_CALLBACK);
    PFN_vkSetDeviceLoaderData set_loader_data =
        callback_info ? callback_info->u.pfnSetDeviceLoaderData : nullptr;

    auto create_device =
        reinterpret_cast<PFN_vkCreateDevice>(next_gipa(VK_NULL_HANDLE, "vkCreateDevice"));
    if (!create_device) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    link->u.pLayerInfo = link->u.pLayerInfo->pNext;

    VkResult result = create_device(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (result != VK_SUCCESS) {
        return result;
    }

    DeviceData data;
    data.device = *pDevice;
    data.physical_device = physicalDevice;
    data.set_device_loader_data = set_loader_data;
    data.gipa = next_gipa;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_instances.find(dispatch_key(physicalDevice));
        if (it != g_instances.end()) {
            data.instance = it->second.instance;
        }
    }
    // What each queue family can do, so a present on a family that cannot take
    // a render pass is left alone rather than recorded into. Asked through the
    // instance chain, once, here -- never on the present path.
    if (auto family_properties = reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(
            next_gipa(data.instance, "vkGetPhysicalDeviceQueueFamilyProperties"))) {
        uint32_t count = 0;
        family_properties(physicalDevice, &count, nullptr);
        if (count > 0 && count < 64) {
            std::vector<VkQueueFamilyProperties> properties(count);
            family_properties(physicalDevice, &count, properties.data());
            data.family_flags.reserve(count);
            for (const VkQueueFamilyProperties& family : properties) {
                data.family_flags.push_back(family.queueFlags);
            }
        }
    }

    DeviceDispatch& d = data.disp;
    d.GetDeviceProcAddr = next_gdpa;

#define VOCEM_LOAD(name) \
    d.name = reinterpret_cast<PFN_vk##name>(next_gdpa(*pDevice, "vk" #name))

    VOCEM_LOAD(DestroyDevice);
    VOCEM_LOAD(GetDeviceQueue);
    VOCEM_LOAD(GetDeviceQueue2);
    VOCEM_LOAD(CreateSwapchainKHR);
    VOCEM_LOAD(DestroySwapchainKHR);
    VOCEM_LOAD(GetSwapchainImagesKHR);
    VOCEM_LOAD(QueuePresentKHR);
    VOCEM_LOAD(QueueSubmit);
    VOCEM_LOAD(CreateImageView);
    VOCEM_LOAD(DestroyImageView);
    VOCEM_LOAD(CreateRenderPass);
    VOCEM_LOAD(DestroyRenderPass);
    VOCEM_LOAD(CreateFramebuffer);
    VOCEM_LOAD(DestroyFramebuffer);
    VOCEM_LOAD(CreateCommandPool);
    VOCEM_LOAD(DestroyCommandPool);
    VOCEM_LOAD(AllocateCommandBuffers);
    VOCEM_LOAD(BeginCommandBuffer);
    VOCEM_LOAD(EndCommandBuffer);
    VOCEM_LOAD(ResetCommandBuffer);
    VOCEM_LOAD(CmdBeginRenderPass);
    VOCEM_LOAD(CmdEndRenderPass);
    VOCEM_LOAD(CreateSemaphore);
    VOCEM_LOAD(DestroySemaphore);
    VOCEM_LOAD(CreateFence);
    VOCEM_LOAD(DestroyFence);
    VOCEM_LOAD(WaitForFences);
    VOCEM_LOAD(ResetFences);
    VOCEM_LOAD(CreateShaderModule);
    VOCEM_LOAD(DestroyShaderModule);
    VOCEM_LOAD(CreateDescriptorSetLayout);
    VOCEM_LOAD(DestroyDescriptorSetLayout);
    VOCEM_LOAD(CreatePipelineLayout);
    VOCEM_LOAD(DestroyPipelineLayout);
    VOCEM_LOAD(CreateGraphicsPipelines);
    VOCEM_LOAD(DestroyPipeline);

#undef VOCEM_LOAD

    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_devices[dispatch_key(*pDevice)] = std::move(data);
    }

    VOCEM_LOG("device created");
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vocem_DestroyDevice(VkDevice device,
                                               const VkAllocationCallbacks* pAllocator) {
    PFN_vkDestroyDevice destroy = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_devices.find(dispatch_key(device));
        if (it != g_devices.end()) {
            destroy = it->second.disp.DestroyDevice;
            // Whatever of ours still lives on this device goes with it: the
            // driver may reuse the swapchain handle, which must not find a
            // stale entry with resources on a dead device.
            for (auto sc = g_swapchains.begin(); sc != g_swapchains.end();) {
                if (sc->second.device == device) {
                    destroy_swapchain_resources(it->second, sc->second, true);
                    sc = g_swapchains.erase(sc);
                } else {
                    ++sc;
                }
            }
            // The semaphores kept past their swapchains: the application has
            // finished everything on the device before destroying it.
            for (VkSemaphore semaphore : it->second.retired_semaphores) {
                it->second.disp.DestroySemaphore(device, semaphore, nullptr);
            }
            g_devices.erase(it);
        }
        // Only the renderer's OWN device takes the renderer down: a helper
        // device (video decoder, probe, second adapter) must not tear down
        // what the presenting device's command buffers still read. A device
        // the renderer could not be made on gives the overlay up the same way.
        if (vocem::renderer().device() == device ||
            (g_hand_over.holding() == vocem::HandOver::Holding::Failed &&
             g_failed_device == device)) {
            vocem::journal_note("device destroyed; renderer shutting down");
            release_renderer_locked();
        }
    }
    if (destroy) {
        destroy(device, pAllocator);
    }
}

// Queues are opaque handles: to allocate command buffers compatible with the
// queue the application presents on, we record which family each one came from.
VKAPI_ATTR void VKAPI_CALL vocem_GetDeviceQueue(VkDevice device, uint32_t queueFamilyIndex,
                                                uint32_t queueIndex, VkQueue* pQueue) {
    PFN_vkGetDeviceQueue next = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (DeviceData* dev = find_device(device)) {
            next = dev->disp.GetDeviceQueue;
        }
    }
    if (!next) {
        // Same answer as GetDeviceQueue2 below: a handle the application can
        // test, not whatever was on its stack.
        if (pQueue) {
            *pQueue = VK_NULL_HANDLE;
        }
        return;
    }
    next(device, queueFamilyIndex, queueIndex, pQueue);

    std::lock_guard<std::mutex> guard(g_lock);
    if (DeviceData* dev = find_device(device)) {
        dev->queue_families[*pQueue] = queueFamilyIndex;
    }
}

VKAPI_ATTR void VKAPI_CALL vocem_GetDeviceQueue2(VkDevice device,
                                                 const VkDeviceQueueInfo2* pQueueInfo,
                                                 VkQueue* pQueue) {
    PFN_vkGetDeviceQueue2 next = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (DeviceData* dev = find_device(device)) {
            next = dev->disp.GetDeviceQueue2;
        }
    }
    if (!next) {
        // The device lacks this function: vocem_GetInstanceProcAddr can still
        // hand out our hook for it, so this is an ordinary road. The output
        // must be a handle the application can test.
        if (pQueue) {
            *pQueue = VK_NULL_HANDLE;
        }
        return;
    }
    next(device, pQueueInfo, pQueue);

    std::lock_guard<std::mutex> guard(g_lock);
    if (DeviceData* dev = find_device(device)) {
        dev->queue_families[*pQueue] = pQueueInfo->queueFamilyIndex;
    }
}

// ---------------------------------------------------------------------------
// Swapchain
// ---------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL vocem_CreateSwapchainKHR(VkDevice device,
                                                        const VkSwapchainCreateInfoKHR* pCreateInfo,
                                                        const VkAllocationCallbacks* pAllocator,
                                                        VkSwapchainKHR* pSwapchain) {
    PFN_vkCreateSwapchainKHR next = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (DeviceData* dev = find_device(device)) {
            next = dev->disp.CreateSwapchainKHR;
        }
    }
    if (!next) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    // We render into the swapchain images through a render pass, so they must be
    // usable as colour attachments. Nearly every application already asks for
    // this; adding the bit costs nothing when it is already present.
    VkSwapchainCreateInfoKHR info = *pCreateInfo;
    info.imageUsage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    VkResult result = next(device, &info, pAllocator, pSwapchain);
    if (result != VK_SUCCESS) {
        return result;
    }

    SwapchainData data;
    data.device = device;
    data.format = pCreateInfo->imageFormat;
    data.color_space = pCreateInfo->imageColorSpace;
    data.extent = pCreateInfo->imageExtent;

    {
        std::lock_guard<std::mutex> guard(g_lock);
        g_swapchains[*pSwapchain] = std::move(data);
    }

    // Named rather than numbered: this line is what gets read when the
    // panel's colours look wrong.
    const char* conversion = "";
    switch (vocem::hdr_mode_for(pCreateInfo->imageColorSpace, pCreateInfo->imageFormat)) {
        case 1: conversion = " (scRGB: colours re-encoded)"; break;
        case 2: conversion = " (HDR10: colours re-encoded)"; break;
        case 3: conversion = " (sRGB format: colours handed over linear)"; break;
        default: break;
    }
    VOCEM_LOG("swapchain created: %ux%u format %d colour space %d%s",
              pCreateInfo->imageExtent.width, pCreateInfo->imageExtent.height,
              static_cast<int>(pCreateInfo->imageFormat),
              static_cast<int>(pCreateInfo->imageColorSpace), conversion);
    return VK_SUCCESS;
}

// How many semaphores of destroyed swapchains a device keeps before the oldest
// go: three a swapchain, so about twenty recreations.
constexpr size_t kMaxRetiredSemaphores = 64;

// Caller must hold g_lock.
//
// No vkDeviceWaitIdle here: this runs inside the application's
// vkDestroySwapchainKHR, which synchronises that swapchain only, while
// vkDeviceWaitIdle needs every queue of the device externally synchronised.
// Our own submissions, one per image, are waited for by fence instead.
//
// The semaphores they signalled were waited on by a present, and a fence says
// nothing about when the presentation engine consumed them. So they are kept
// on the device and destroyed with it (`device_going`), or oldest first once
// more than kMaxRetiredSemaphores pile up (a reasoned bound, not a measured
// one). A swapchain never submitted to destroys them at once.
void destroy_swapchain_resources(DeviceData& dev, SwapchainData& sc, bool device_going) {
    const DeviceDispatch& d = dev.disp;
    const VkDevice device = dev.device;
    for (size_t i = 0; i < sc.fences.size() && i < sc.submitted.size(); ++i) {
        if (sc.submitted[i] && sc.fences[i] != VK_NULL_HANDLE && d.WaitForFences) {
            d.WaitForFences(device, 1, &sc.fences[i], VK_TRUE, UINT64_MAX);
        }
    }
    for (VkFence fence : sc.fences) {
        if (fence != VK_NULL_HANDLE) d.DestroyFence(device, fence, nullptr);
    }
    for (VkSemaphore semaphore : sc.semaphores) {
        if (semaphore == VK_NULL_HANDLE) {
            continue;
        }
        if (sc.signalled_any && !device_going) {
            dev.retired_semaphores.push_back(semaphore);
        } else {
            d.DestroySemaphore(device, semaphore, nullptr);
        }
    }
    if (dev.retired_semaphores.size() > kMaxRetiredSemaphores) {
        const size_t excess = dev.retired_semaphores.size() - kMaxRetiredSemaphores;
        for (size_t i = 0; i < excess; ++i) {
            d.DestroySemaphore(device, dev.retired_semaphores[i], nullptr);
        }
        dev.retired_semaphores.erase(dev.retired_semaphores.begin(),
                                     dev.retired_semaphores.begin() +
                                         static_cast<std::ptrdiff_t>(excess));
    }
    for (VkFramebuffer fb : sc.framebuffers) {
        if (fb != VK_NULL_HANDLE) d.DestroyFramebuffer(device, fb, nullptr);
    }
    for (VkImageView view : sc.views) {
        if (view != VK_NULL_HANDLE) d.DestroyImageView(device, view, nullptr);
    }
    if (sc.pool != VK_NULL_HANDLE) d.DestroyCommandPool(device, sc.pool, nullptr);
    if (sc.render_pass != VK_NULL_HANDLE) d.DestroyRenderPass(device, sc.render_pass, nullptr);
    vocem::hdr_pipeline_destroy(hdr_functions(d), device, sc.hdr);

    sc.fences.clear();
    sc.semaphores.clear();
    sc.framebuffers.clear();
    sc.views.clear();
    sc.command_buffers.clear();
    sc.images.clear();
    sc.pool = VK_NULL_HANDLE;
    sc.render_pass = VK_NULL_HANDLE;
    sc.usable = false;
    sc.submitted.clear();
    sc.signalled_any = false;
}

VKAPI_ATTR void VKAPI_CALL vocem_DestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
                                                     const VkAllocationCallbacks* pAllocator) {
    PFN_vkDestroySwapchainKHR next = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        DeviceData* dev = find_device(device);
        if (!dev) {
            return;
        }
        next = dev->disp.DestroySwapchainKHR;

        auto it = g_swapchains.find(swapchain);
        if (it != g_swapchains.end()) {
            destroy_swapchain_resources(*dev, it->second, false);
            g_swapchains.erase(it);
        }
    }
    if (next) {
        next(device, swapchain, pAllocator);
    }
}

// Build everything we need to paint into this swapchain. Called once per
// swapchain, on the first present. Any failure leaves usable == false, which
// turns the layer into a pass-through for that swapchain.
// Caller must hold g_lock.
bool build_swapchain_resources(DeviceData& dev, SwapchainData& sc, VkSwapchainKHR swapchain,
                               uint32_t queue_family) {
    const DeviceDispatch& d = dev.disp;
    sc.attempted = true;
    sc.queue_family = queue_family;

    uint32_t image_count = 0;
    if (d.GetSwapchainImagesKHR(dev.device, swapchain, &image_count, nullptr) != VK_SUCCESS ||
        image_count == 0) {
        VOCEM_LOG("could not query swapchain images");
        return false;
    }
    sc.images.resize(image_count);
    if (d.GetSwapchainImagesKHR(dev.device, swapchain, &image_count, sc.images.data()) !=
        VK_SUCCESS) {
        return false;
    }

    // Load/store the existing contents: we paint on top of the finished frame.
    VkAttachmentDescription attachment{};
    attachment.format = sc.format;
    attachment.samples = VK_SAMPLE_COUNT_1_BIT;
    attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    attachment.initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference color_ref{};
    color_ref.attachment = 0;
    color_ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pColorAttachments = &color_ref;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    // READ as well as WRITE: loadOp LOAD reads the game's finished frame, and
    // that read must be ordered after the game's rendering
    // (SYNC-HAZARD-READ-AFTER-WRITE under the validation layer).
    dependency.dstAccessMask =
        VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

    VkRenderPassCreateInfo rp_info{};
    rp_info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rp_info.attachmentCount = 1;
    rp_info.pAttachments = &attachment;
    rp_info.subpassCount = 1;
    rp_info.pSubpasses = &subpass;
    rp_info.dependencyCount = 1;
    rp_info.pDependencies = &dependency;

    if (d.CreateRenderPass(dev.device, &rp_info, nullptr, &sc.render_pass) != VK_SUCCESS) {
        VOCEM_LOG("render pass creation failed");
        return false;
    }

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pool_info.queueFamilyIndex = queue_family;
    if (d.CreateCommandPool(dev.device, &pool_info, nullptr, &sc.pool) != VK_SUCCESS) {
        VOCEM_LOG("command pool creation failed");
        return false;
    }

    sc.views.resize(image_count, VK_NULL_HANDLE);
    sc.framebuffers.resize(image_count, VK_NULL_HANDLE);
    sc.semaphores.resize(image_count, VK_NULL_HANDLE);
    sc.fences.resize(image_count, VK_NULL_HANDLE);
    sc.submitted.assign(image_count, 0);
    sc.command_buffers.resize(image_count, VK_NULL_HANDLE);

    VkCommandBufferAllocateInfo cmd_info{};
    cmd_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cmd_info.commandPool = sc.pool;
    cmd_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cmd_info.commandBufferCount = image_count;
    if (d.AllocateCommandBuffers(dev.device, &cmd_info, sc.command_buffers.data()) != VK_SUCCESS) {
        VOCEM_LOG("command buffer allocation failed");
        return false;
    }

    for (uint32_t i = 0; i < image_count; ++i) {
        // Mandatory for layer-created dispatchable objects.
        if (dev.set_device_loader_data) {
            dev.set_device_loader_data(dev.device, sc.command_buffers[i]);
        }

        // No view and no framebuffer here: those are made at each image's
        // first draw (image_target below), because an image of a swapchain
        // created with VK_SWAPCHAIN_CREATE_DEFERRED_MEMORY_ALLOCATION_BIT_EXT
        // has no memory until it is first acquired, and at this first present
        // only the image being presented ever was.

        VkSemaphoreCreateInfo sem_info{};
        sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (d.CreateSemaphore(dev.device, &sem_info, nullptr, &sc.semaphores[i]) != VK_SUCCESS) {
            return false;
        }

        // Unsignalled: the `submitted` flag guards both the wait and the
        // reset, so a signalled fence would reach the first submit still
        // signalled (VUID-vkQueueSubmit-fence-00063).
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (d.CreateFence(dev.device, &fence_info, nullptr, &sc.fences[i]) != VK_SUCCESS) {
            return false;
        }
    }

    // Every swapchain gets a pipeline of its own, built against its own render
    // pass (hdr_pipeline.h): the converting one where the colour space or format
    // needs it, the identity otherwise. ImGui's stock pipeline is built once,
    // against the first swapchain's render pass, and is incompatible with a
    // later one in another format. If this fails, draw_overlay() uses the stock
    // pipeline where the format matches and otherwise passes through, logged.
    const int mode = vocem::hdr_mode_for(sc.color_space, sc.format);
    if (vocem::hdr_pipeline_create(hdr_functions(d), dev.device, sc.render_pass, mode,
                                   vocem::hdr_sdr_nits(), sc.hdr)) {
        if (mode != 0) {
            VOCEM_LOG("colour pipeline ready (mode %d, SDR white %.0f nits)", mode,
                      vocem::hdr_sdr_nits());
            sc.colour_note_pending = true;
        }
    } else {
        VOCEM_LOG("own pipeline unavailable (mode %d): the stock one draws where the format "
                  "allows, otherwise nothing", mode);
    }

    sc.usable = true;
    VOCEM_LOG("swapchain resources ready: %u images, queue family %u", image_count, queue_family);
    return true;
}

// The view and the framebuffer of one image, made the first time the overlay
// draws into it: with VK_EXT_swapchain_maintenance1's deferred allocation an
// image has no memory until first acquired, and an image being presented has
// been. Done for every swapchain, deferred or not, so one path serves all. A
// failure passes this swapchain through for good, said once.
// Caller must hold g_lock.
VkFramebuffer image_target(DeviceData& dev, SwapchainData& sc, uint32_t image_index) {
    if (sc.framebuffers[image_index] != VK_NULL_HANDLE) {
        return sc.framebuffers[image_index];
    }
    const DeviceDispatch& d = dev.disp;
    if (sc.views[image_index] == VK_NULL_HANDLE) {
        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = sc.images[image_index];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = sc.format;
        view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        if (d.CreateImageView(dev.device, &view_info, nullptr, &sc.views[image_index]) !=
            VK_SUCCESS) {
            sc.views[image_index] = VK_NULL_HANDLE;
            VOCEM_LOG("not drawing into this swapchain: image view creation failed for image %u",
                      image_index);
            sc.usable = false;
            return VK_NULL_HANDLE;
        }
    }
    VkFramebufferCreateInfo fb_info{};
    fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fb_info.renderPass = sc.render_pass;
    fb_info.attachmentCount = 1;
    fb_info.pAttachments = &sc.views[image_index];
    fb_info.width = sc.extent.width;
    fb_info.height = sc.extent.height;
    fb_info.layers = 1;
    if (d.CreateFramebuffer(dev.device, &fb_info, nullptr, &sc.framebuffers[image_index]) !=
        VK_SUCCESS) {
        sc.framebuffers[image_index] = VK_NULL_HANDLE;
        VOCEM_LOG("not drawing into this swapchain: framebuffer creation failed for image %u",
                  image_index);
        sc.usable = false;
        return VK_NULL_HANDLE;
    }
    return sc.framebuffers[image_index];
}

// Record and submit the overlay for one image. Returns the semaphore the
// present must wait on, or VK_NULL_HANDLE to leave the present untouched.
// Caller must hold g_lock: every call down the chain in here goes through a
// pointer resolved for the layer below, never the loader's trampolines, so the
// loader cannot re-enter this layer and deadlock (rule 9).
// A present waiting on more semaphores than this passes through undrawn: the
// submit's stage list is a stack array, so the drawn path never allocates.
constexpr uint32_t kMaxWaitSemaphores = 16;

// `wanted` is set when this frame had something to put on the screen (past the
// poll and both feature guards), whatever happens after. The caller gates the
// renderer's construction on it -- a font atlas, a 64 MB upload and a
// descriptor pool -- and `sizing` is the height that atlas is sized from.
VkSemaphore draw_overlay(DeviceData& dev, SwapchainData& sc, VkQueue queue, uint32_t image_index,
                         const VkSemaphore* wait_semaphores, uint32_t wait_count, bool& wanted,
                         uint32_t& sizing) {
    const DeviceDispatch& d = dev.disp;
    if (image_index >= sc.command_buffers.size() || wait_count > kMaxWaitSemaphores) {
        return VK_NULL_HANDLE;
    }

    // Nothing to show: leave the frame, and the application's synchronisation,
    // completely untouched. This is the common case and must cost nothing.
    // Asked feature by feature (vocem/panel.h), so a toast shows outside a
    // voice channel too.
    const vocem::Snapshot* snapshot = g_state.poll();
    if (!snapshot) {
        return VK_NULL_HANDLE;
    }
    // The master switch is the caller's verdict; what is left is the features.
    const vocem::Config& config = vocem::renderer().current_config();
    if (!vocem::panel_wanted(*snapshot, config) &&
        !vocem::notification_wanted(*snapshot, config, vocem::monotonic_seconds())) {
        static bool logged_idle = false;
        if (!logged_idle) {
            logged_idle = true;
            VOCEM_LOG("nothing to draw: connected=%d in_channel=%d users=%u",
                      snapshot->connected ? 1 : 0, snapshot->in_channel ? 1 : 0,
                      snapshot->user_count);
        }
        return VK_NULL_HANDLE;
    }
    // From here on this frame had something to draw. Everything below is about
    // whether it CAN be drawn, which is a different question and not one the
    // renderer should be built for.
    wanted = true;
    // The height the atlas is sized from, as OverlayRenderer::draw computes it:
    // the display's, not the swapchain's, or a windowed game would rasterise
    // the atlas twice in its first frames.
    sizing = vocem::sizing_height(snapshot->display_height, sc.extent.height);

    // Initialisation happens after the present returns, never here: the font
    // atlas's upload waits on the queue, and blocking on it from inside a queue
    // operation is a stall. The texture cache makes that upload in prepare();
    // the backend's own NewFrame is never called.
    if (!vocem::renderer().ready()) {
        return VK_NULL_HANDLE;
    }

    // Logged once: proof that the ImGui path really executed, which is otherwise
    // invisible without looking at the screen.
    static bool logged_first_draw = false;
    if (!logged_first_draw) {
        logged_first_draw = true;
        VOCEM_LOG("drawing panel: %u user(s) in '%s'", snapshot->user_count,
                  snapshot->channel_name);
    }

    // Which pipeline draws this frame: the swapchain's own, or the backend's
    // stock one where this swapchain's format is the one it was built for.
    // Neither is a frame left alone, said once (rule 7 over a wrong picture).
    VkPipeline pipeline = sc.hdr.pipeline;
    if (pipeline == VK_NULL_HANDLE && sc.format != vocem::renderer().format()) {
        if (!sc.said_pass_through) {
            sc.said_pass_through = true;
            VOCEM_LOG("not drawing into this swapchain: format %d has no pipeline of its own "
                      "and the stock one was built for format %d",
                      static_cast<int>(sc.format), static_cast<int>(vocem::renderer().format()));
        }
        return VK_NULL_HANDLE;
    }
    // The backend cycles a fixed ring of vertex buffers, one slot per draw,
    // and a slot must not come round while a command buffer for an older
    // image still reads it: the ring covers a swapchain of up to kRingSlots
    // images and no more (overlay_renderer.h says why it cannot grow).
    if (sc.command_buffers.size() > vocem::OverlayRenderer::kRingSlots) {
        if (!sc.said_pass_through) {
            sc.said_pass_through = true;
            VOCEM_LOG("not drawing into this swapchain: %zu images exceed the %u-slot ring",
                      sc.command_buffers.size(), vocem::OverlayRenderer::kRingSlots);
        }
        return VK_NULL_HANDLE;
    }

    // Before the fence is touched: a target that cannot be made leaves this
    // image's synchronisation exactly as it found it.
    const VkFramebuffer framebuffer = image_target(dev, sc, image_index);
    if (framebuffer == VK_NULL_HANDLE) {
        return VK_NULL_HANDLE;
    }

    VkCommandBuffer cmd = sc.command_buffers[image_index];
    VkFence fence = sc.fences[image_index];

    // Our own previous submission for this image must have completed before we
    // re-record. This waits on us, never on the application's work.
    if (sc.submitted[image_index]) {
        if (d.WaitForFences(dev.device, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
            return VK_NULL_HANDLE;
        }
        d.ResetFences(dev.device, 1, &fence);
    }
    sc.submitted[image_index] = 0;
    d.ResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin{};
    begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    if (d.BeginCommandBuffer(cmd, &begin) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }

    VkRenderPassBeginInfo rp_begin{};
    rp_begin.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rp_begin.renderPass = sc.render_pass;
    rp_begin.framebuffer = framebuffer;
    rp_begin.renderArea.extent = sc.extent;
    d.CmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    vocem::renderer().draw(cmd, *snapshot, sc.extent.width, sc.extent.height, pipeline,
                           dev.device, queue);

    d.CmdEndRenderPass(cmd);
    if (d.EndCommandBuffer(cmd) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }

    // Chain into the application's synchronisation: wait on whatever the present
    // was going to wait on, and hand the present our own semaphore instead.
    // The stage list is on the stack (no allocation on the hot path); the
    // semaphore count was bounded at the top, because past this point the
    // fence has been reset and not submitting would hang the next frame.
    VkPipelineStageFlags stages[kMaxWaitSemaphores];
    for (uint32_t i = 0; i < wait_count; ++i) {
        stages[i] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    }

    VkSubmitInfo submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submit.waitSemaphoreCount = wait_count;
    submit.pWaitSemaphores = wait_count ? wait_semaphores : nullptr;
    submit.pWaitDstStageMask = wait_count ? stages : nullptr;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &sc.semaphores[image_index];

    if (d.QueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS) {
        VOCEM_LOG("overlay submit failed");
        return VK_NULL_HANDLE;
    }
    sc.submitted[image_index] = 1;
    sc.signalled_any = true;
    return sc.semaphores[image_index];
}

// Whether the overlay belongs in this process: the window's application lists
// and the master switch, decided by the session (vocem/overlay_session.h), which
// also tells the daemon across the Flatpak bridge -- in one call, so the caller
// cannot ask by halves. Asked every frame: the policy re-walks the lists only
// when they were edited, so a change reaches a running game within seconds and
// the steady state is two string comparisons.
bool overlay_wanted_here() {
    return vocem::session().decide(vocem::renderer().current_config());
}

// The last answer, so that a change can be acted on rather than merely obeyed.
// -1 until the first present. Guarded by g_lock, which the only reader and the
// only writer both hold.
int g_drawing = -1;

VKAPI_ATTR VkResult VKAPI_CALL vocem_QueuePresentKHR(VkQueue queue,
                                                     const VkPresentInfoKHR* pPresentInfo) {
    PFN_vkQueuePresentKHR next = nullptr;
    VkSemaphore overlay_semaphore = VK_NULL_HANDLE;

    // Set when the renderer still has to be created. Handles are copied out so
    // the work can happen after the present, without holding the lock or an
    // iterator into the maps.
    bool needs_init = false;
    // Whether this process is one the overlay may draw in. The verdict
    // (overlay_wanted_here()) is taken inside the lock, before the present: its
    // first evaluation reads /proc/self and may scan the desktop entries, once
    // per process; afterwards it is a cached read. Deferring it would only buy a
    // frame of latency, since nothing can be drawn before it exists. The record
    // is written after the present. Rule 8 as it holds: no per-frame blocking
    // I/O, one verdict per process, and the cadenced syscalls it names
    // (LiveConfig's stat every 2 s, StatePoll's once a second) (entry 42).
    bool drawable = false;
    // Set under the lock when the verdict or the master switch turned off on
    // this present; acted on after it returns, where blocking is allowed.
    bool switched_off = false;
    // Set when this present is not the renderer's and its owner has been
    // silent for HandOver::kSeconds: the backend is moved after the present.
    bool hand_over = false;
    VkDevice present_device = VK_NULL_HANDLE;
    vocem::RendererTarget pending_target;

    {
        std::lock_guard<std::mutex> guard(g_lock);
        DeviceData* dev = find_device(queue);
        if (!dev) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        next = dev->disp.QueuePresentKHR;
        present_device = dev->device;

        // Only single-swapchain presents are drawn; others pass through
        // untouched rather than risk incorrect synchronisation.
        drawable = pPresentInfo->swapchainCount == 1;
        if (drawable) {
            auto it = g_swapchains.find(pPresentInfo->pSwapchains[0]);
            // "Is this a swapchain we know" and "does the overlay belong in this
            // process" are asked separately: an unknown swapchain must not read
            // as the overlay being switched off.
            if (it != g_swapchains.end()) {
                const bool want = overlay_wanted_here();
                // A verdict that turns off is a moment, not just a state: the
                // renderer, the atlas and the faces are handed back after the
                // present (`switched_off`), as the GL path does.
                if (g_drawing >= 0 && want != (g_drawing == 1)) {
                    VOCEM_LOG("%s in '%s'", want ? "switched on" : "switched off",
                              vocem::process_name().c_str());
                    switched_off = !want;
                }
                g_drawing = want ? 1 : 0;
                // Not the renderer's device, or not its queue: left alone,
                // said once, and nothing of ours is built for it -- until the
                // owner has been silent long enough to move to this one.
                const Presenter presenter = want ? whose_present(dev->device, queue)
                                                 : Presenter::Owner;
                hand_over = presenter == Presenter::Abandoned;
                if (want && presenter == Presenter::Owner) {
                    SwapchainData& sc = it->second;

                    auto family_it = dev->queue_families.find(queue);
                    uint32_t family =
                        family_it == dev->queue_families.end() ? UINT32_MAX : family_it->second;
                    // A family that cannot take a render pass (a compute or
                    // transfer queue presenting, which the specification allows)
                    // gets the frame back untouched, and so does a family other
                    // than the one the command pool was built on.
                    const bool graphics =
                        family == UINT32_MAX || family >= dev->family_flags.size() ||
                        (dev->family_flags[family] & VK_QUEUE_GRAPHICS_BIT) != 0;
                    if (!graphics || (sc.attempted && sc.queue_family != family)) {
                        if (!sc.said_pass_through) {
                            sc.said_pass_through = true;
                            VOCEM_LOG(
                                "not drawing into this swapchain: presented on queue family "
                                "%u, which %s", family,
                                graphics ? "is not the family its command pool was built on"
                                         : "has no graphics capability");
                        }
                        family = UINT32_MAX;
                    }

                    if (!sc.attempted && family != UINT32_MAX) {
                        if (!build_swapchain_resources(*dev, sc, pPresentInfo->pSwapchains[0],
                                                       family)) {
                            destroy_swapchain_resources(*dev, sc, false);
                        }
                    }

                    if (sc.usable && family != UINT32_MAX) {
                        bool wanted = false;
                        uint32_t sizing = sc.extent.height;
                        overlay_semaphore =
                            draw_overlay(*dev, sc, queue, pPresentInfo->pImageIndices[0],
                                         pPresentInfo->pWaitSemaphores,
                                         pPresentInfo->waitSemaphoreCount, wanted, sizing);
                        // Built only for a frame that had something on it, not
                        // merely for an allowed game: an idle machine with the
                        // tray up must not pay for an atlas, a 64 MB upload and
                        // a descriptor pool. The first drawn frame pays instead,
                        // post-present (rule 10).
                        if (wanted && !vocem::renderer().ready()) {
                            needs_init = true;
                            pending_target.instance = dev->instance;
                            pending_target.physical_device = dev->physical_device;
                            pending_target.device = dev->device;
                            pending_target.render_pass = sc.render_pass;
                            pending_target.format = sc.format;
                            pending_target.queue = queue;
                            pending_target.queue_family = sc.queue_family;
                            pending_target.image_count = static_cast<uint32_t>(sc.images.size());
                            pending_target.height = sizing;
                            pending_target.gipa = dev->gipa;
                            pending_target.gdpa = dev->disp.GetDeviceProcAddr;
                            pending_target.set_loader_data = dev->set_device_loader_data;
                        }
                    }
                }
            }
        }
    }

    if (!next) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult result;
    if (overlay_semaphore == VK_NULL_HANDLE) {
        result = next(queue, pPresentInfo);
    } else {
        VkPresentInfoKHR info = *pPresentInfo;
        info.waitSemaphoreCount = 1;
        info.pWaitSemaphores = &overlay_semaphore;
        result = next(queue, &info);
    }

    // Written down once, whether or not there is anything to draw and whether or
    // not the overlay is allowed to draw here: the window lists what the overlay is
    // loaded into, and an application excluded from it is precisely one somebody
    // may want to find in that list and let back in.
    if (drawable) {
        vocem::record_application("vulkan");
    }

    // The journal line a build inside the present held back (rule 8): a file
    // write, done here where file work is allowed.
    bool daemon_left = false;
    if (drawable) {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_swapchains.find(pPresentInfo->pSwapchains[0]);
        if (it != g_swapchains.end() && it->second.colour_note_pending) {
            it->second.colour_note_pending = false;
            vocem::journal_note("colour pipeline ready");
        }
        // Read under the lock the poll itself runs under, and acted on below
        // in a scope of its own: the release waits for the overlay's fences
        // and takes the renderer's lock after this one, never the reverse.
        daemon_left = g_state.daemon_left();
    }

    // The daemon stopped (the tray's Quit, or `systemctl --user stop`), or the
    // switch turned off: what this process holds on the daemon's behalf -- the
    // renderer's objects and the font atlas -- is handed back here, after the
    // present, where blocking is allowed. The swapchain's own objects stay; the
    // game's next frame needs them. prepare() builds it all back once there is
    // something to draw again.
    if (daemon_left || switched_off) {
        VOCEM_LOG("%s: releasing the backend and the font atlas",
                  daemon_left ? "the daemon stopped" : "switched off");
        vocem::journal_note(daemon_left ? "daemon stopped: released" : "switched off: released");
        {
            std::lock_guard<std::mutex> guard(g_lock);
            release_renderer_locked();
        }
        vocem::fonts_release();
    } else if (hand_over) {
        // The owner has been silent for HandOver::kSeconds: the backend is let go
        // (waited for by fence) and the next present here builds it on this
        // device and queue. The atlas stays. Asked again under the
        // lock: another thread may have moved it already.
        std::lock_guard<std::mutex> guard(g_lock);
        const bool failed_elsewhere =
            g_hand_over.holding() == vocem::HandOver::Holding::Failed &&
            !failed_here(present_device, queue);
        if ((vocem::renderer().ready() && !vocem::renderer().owns(present_device, queue)) ||
            failed_elsewhere) {
            VOCEM_LOG("the renderer's device has not presented for %.0f s: moving the overlay "
                      "to device %p",
                      vocem::HandOver::kSeconds, static_cast<void*>(present_device));
            vocem::journal_note("renderer's device silent: moving the overlay");
            release_renderer_locked();
        }
    }

    // Safe here: the present has returned, so the queue is ours to block on.
    // prepare() is called on each present until the renderer is up (the first
    // atlas is rasterised on a worker meanwhile), and the renderer is built
    // once per device, not per swapchain. `needs_init` is the whole condition:
    // it is set only when draw_overlay found something to draw. ready() is
    // asked again because prepare() runs unlocked and another presenting
    // thread may have built the backend in between.
    if (needs_init && !vocem::renderer().ready()) {
        // Deliberately unlocked: prepare() resolves entry points through the
        // chain, and the loader can route those back into this layer. Its
        // answer is remembered inside: a failure is said once and not retried
        // on every present.
        if (vocem::renderer().prepare(pending_target)) {
            // The owner's clock starts when it becomes the owner, not at its
            // next present: a second device presenting in between must not
            // find it silent since the process began.
            std::lock_guard<std::mutex> guard(g_lock);
            if (vocem::renderer().ready()) {
                g_hand_over.take(vocem::HandOver::Holding::Ready, vocem::monotonic_seconds());
            }
        } else if (vocem::renderer().failed()) {
            // Refused here: this device and queue hold the overlay as a
            // renderer that came up would, so the failure goes with them
            // rather than staying the whole process's.
            // Asked again under the lock: a release on another thread may
            // have cleared the failure in between.
            std::lock_guard<std::mutex> guard(g_lock);
            if (!g_hand_over.held() && vocem::renderer().failed()) {
                g_hand_over.take(vocem::HandOver::Holding::Failed, vocem::monotonic_seconds());
                g_failed_device = pending_target.device;
                g_failed_queue = pending_target.queue;
                VOCEM_LOG("the overlay stays with device %p, where its renderer could not be "
                          "made, until that device is destroyed or falls silent",
                          static_cast<void*>(pending_target.device));
            }
        }
    } else if (vocem::renderer().ready()) {
        // Uploads and rebuilds submit and free what their fences say is done;
        // a rebuild waits the queue idle before it replaces the image, which is
        // why they belong here and not in the present. Only on the owner's
        // queue, which process_uploads() asks itself: this present's external
        // synchronisation covers its own queue and no other.
        vocem::renderer().process_uploads(present_device, queue);
    }

    return result;
}

// ---------------------------------------------------------------------------
// Entry points
// ---------------------------------------------------------------------------

struct NameAndFunction {
    const char* name;
    PFN_vkVoidFunction func;
};

#define VOCEM_ENTRY(name) \
    { "vk" #name, reinterpret_cast<PFN_vkVoidFunction>(vocem_##name) }

const NameAndFunction kInterceptedFunctions[] = {
    VOCEM_ENTRY(CreateInstance),      VOCEM_ENTRY(DestroyInstance),
    VOCEM_ENTRY(CreateDevice),        VOCEM_ENTRY(DestroyDevice),
    VOCEM_ENTRY(GetDeviceQueue),      VOCEM_ENTRY(GetDeviceQueue2),
    VOCEM_ENTRY(CreateSwapchainKHR),  VOCEM_ENTRY(DestroySwapchainKHR),
    VOCEM_ENTRY(QueuePresentKHR),
};

#undef VOCEM_ENTRY

PFN_vkVoidFunction find_intercepted(const char* name) {
    for (const NameAndFunction& entry : kInterceptedFunctions) {
        if (std::strcmp(entry.name, name) == 0) {
            return entry.func;
        }
    }
    return nullptr;
}

}  // namespace

extern "C" {

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vocem_GetDeviceProcAddr(VkDevice device, const char* pName) {
    // The pointer is copied out and the lock released before calling down. For
    // unsupported extension functions the loader re-enters the layer chain from
    // the top and lands back here, so holding the lock across the call would
    // deadlock against ourselves.
    PFN_vkGetDeviceProcAddr next = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        if (DeviceData* dev = find_device(device)) {
            next = dev->disp.GetDeviceProcAddr;
        }
    }
    if (!next) {
        return nullptr;
    }
    // The chain is asked first, and our hook is substituted only for a function
    // the device actually has (vkGetDeviceQueue2 on Vulkan 1.0, vkQueuePresentKHR
    // without VK_KHR_swapchain): otherwise feature detection turns into a call
    // into a layer with nothing below it. The shim's dlsym follows the same rule.
    PFN_vkVoidFunction below = next(device, pName);
    if (!below) {
        return nullptr;
    }
    if (PFN_vkVoidFunction func = find_intercepted(pName)) {
        return func;
    }
    return below;
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vocem_GetInstanceProcAddr(VkInstance instance, const char* pName) {
    // VOCEM_TRACE_PROCADDR=1 logs every name asked of this entry.
    static const bool trace = [] {
        const char* env = std::getenv("VOCEM_TRACE_PROCADDR");
        return env && env[0] == '1';
    }();
    if (trace) {
        VOCEM_LOG("gipa %s %s", instance ? "instance" : "null", pName);
    }
    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(vocem_GetInstanceProcAddr);
    }
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(vocem_GetDeviceProcAddr);
    }
    // The instance-level names are answered from the table before the chain is
    // asked, and must be: the loader asks for vkCreateInstance with a null
    // instance, before there is a chain. The device-level names follow
    // vocem_GetDeviceProcAddr's rule instead: the chain answers
    // first and our hook stands in only for a function the chain has -- which
    // matters to an application asking vkGetInstanceProcAddr(instance,
    // "vkQueuePresentKHR") itself, as the specification allows.
    const bool device_level = std::strcmp(pName, "vkCreateInstance") != 0 &&
                              std::strcmp(pName, "vkDestroyInstance") != 0 &&
                              std::strcmp(pName, "vkCreateDevice") != 0;
    PFN_vkVoidFunction ours = find_intercepted(pName);
    if (ours && !device_level) {
        return ours;
    }
    if (instance == VK_NULL_HANDLE) {
        return nullptr;
    }
    PFN_vkGetInstanceProcAddr next = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_instances.find(dispatch_key(instance));
        if (it != g_instances.end()) {
            next = it->second.GetInstanceProcAddr;
        }
    }
    PFN_vkVoidFunction below = next ? next(instance, pName) : nullptr;
    if (ours && below) {
        return ours;
    }
    return below;
}

// Required by the loader: without this the layer will not load at all. Version 2
// is the first that supports negotiation, so it is also the minimum we accept.
VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL
vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* pVersionStruct) {
    if (!pVersionStruct || pVersionStruct->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (pVersionStruct->loaderLayerInterfaceVersion > 2) {
        pVersionStruct->loaderLayerInterfaceVersion = 2;
    }
    pVersionStruct->pfnGetInstanceProcAddr = vocem_GetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = vocem_GetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;
    return VK_SUCCESS;
}

}  // extern "C"

// libstdc++'s own, defined in the copy this library carries (eh_alloc.cc).
namespace __gnu_cxx {
void __freeres() noexcept;
}

namespace {

// The clean end of the crash journal: a process that unwinds normally runs
// this and takes its marker with it; a crash does not, which is the mechanism
// (vocem/journal.h).
__attribute__((destructor)) void vocem_layer_journal_close() {
    // A font atlas still being rasterised on a worker runs this library's code:
    // it finishes before the library can be unmapped -- by exit, or by the
    // loader's dlclose after vkDestroyInstance.
    vocem::atlas_worker().join();
    vocem::journal_end();
    // Last, the exception emergency pool of the libstdc++ this library carries
    // inside it (-static-libstdc++): its constructor mallocs the pool at every
    // load and nothing frees it but __freeres(), so every unload (every
    // vkDestroyInstance) would leak one (tests/injected_unload.cpp). Only this
    // library's copy is freed, never the game's.
    __gnu_cxx::__freeres();
}

}  // namespace

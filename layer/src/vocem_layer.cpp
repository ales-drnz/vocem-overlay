// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Vocem Overlay - Vulkan layer.
//
// The plumbing: dispatch tables, per-swapchain resources, queue/family
// tracking, semaphore chaining. What gets painted lives in overlay_renderer /
// common/src/panel.cpp; this file decides where and when, and its design rules
// are older than any of the drawing:
//   * Never block in vkQueuePresentKHR. No per-frame I/O, no allocation on the
//     hot path (the exact shape of that claim, with its measured exceptions,
//     is at overlay_wanted_here's call site).
//   * Per-image state is indexed by swapchain image index, never by acquisition
//     order (MangoHud 0.8.3 fixed exactly this class of bug).
//   * If anything we need is missing, degrade to a pure pass-through. A layer
//     must never be the reason a game fails to start.
//   * Dispatchable objects we create (command buffers) must be registered with
//     the loader via pfnSetDeviceLoaderData, or their dispatch will crash.
//
// These bullets are this file's own order and are deliberately unnumbered: the
// numbers anything cites -- in this directory, in tests/, and in DESIGN's own
// entries -- are DESIGN's, where the index rule is 4 and the loader-data rule
// is 5. Five citations named rule 4 for the loader-data one, following this
// list's order while naming that file's numbering (corrected 2026-09-19).

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
    PFN_vkDeviceWaitIdle DeviceWaitIdle = nullptr;
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
    // Whether our own submission for this image is outstanding. The fence is
    // reset before recording and three paths below can return without submitting
    // -- a command buffer that would not begin or end, a queue submit that failed
    // under memory pressure. Waiting on an unsignalled fence that nothing will
    // ever signal blocks forever, under g_lock, with every other present thread
    // and vkDestroySwapchainKHR behind it: a transient allocation failure that
    // should cost one undrawn frame instead froze the game.
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

    // Whether anything of ours was ever submitted on this swapchain. What
    // decides if tearing it down needs to wait for the device: a swapchain
    // whose build failed before its first submit has nothing in flight, and a
    // vkDeviceWaitIdle for it is a stall inside the present for nothing.
    bool ever_submitted() const {
        for (uint8_t flag : submitted) {
            if (flag) {
                return true;
            }
        }
        return false;
    }
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
    // device creation. The overlay records a render pass into a command pool
    // on the family the game presents from; a family without GRAPHICS cannot
    // take one, and nothing used to ask. Empty when the query was unavailable,
    // in which case the family is taken on trust as it always was.
    std::vector<VkQueueFlags> family_flags;
};

// The loader hands us dispatchable handles; keying on the raw pointer is the
// conventional way for a layer to find its own per-object state.
std::mutex g_lock;
std::unordered_map<void*, InstanceDispatch> g_instances;
std::unordered_map<void*, DeviceData> g_devices;
std::unordered_map<VkSwapchainKHR, SwapchainData> g_swapchains;

void* dispatch_key(void* handle) { return *reinterpret_cast<void**>(handle); }

// The eight functions the HDR pipeline borrows from the dispatch, in one
// place: the same list was filled in by hand on the create path and the
// destroy path, and a member missed on the destroy copy makes complete()
// false there -- hdr_pipeline_destroy then returns having destroyed nothing,
// a pipeline leaked per swapchain rebuild, silently, in a resize loop.
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



// The attach/detach/read loop is the shared spelling in vocem/state_poll.h --
// it existed here and in the GL path, identical to the character, free to
// drift (and it had: only this side said why a read failed).
void layer_poll_log(const char* line) { VOCEM_LOG("%s", line); }

vocem::StatePoll g_state{&layer_poll_log};

DeviceData* find_device(void* dispatchable) {
    auto it = g_devices.find(dispatch_key(dispatchable));
    return it == g_devices.end() ? nullptr : &it->second;
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
    {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_instances.find(dispatch_key(instance));
        if (it != g_instances.end()) {
            destroy = it->second.DestroyInstance;
            g_instances.erase(it);
        }
    }
    if (destroy) {
        destroy(instance, pAllocator);
    }
}

// ---------------------------------------------------------------------------
// Device
// ---------------------------------------------------------------------------

// Defined with the swapchain code below; vocem_DestroyDevice needs it first.
void destroy_swapchain_resources(const DeviceDispatch& d, VkDevice device, SwapchainData& sc);

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
    VOCEM_LOAD(DeviceWaitIdle);
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
            // Whatever of ours still lives on this device goes with it: a game
            // that destroys its device with a swapchain still registered here
            // would otherwise leave an entry whose handle the driver is free
            // to hand out again for the next swapchain -- with `attempted`
            // and `usable` already true and resources on a device that no
            // longer exists.
            for (auto sc = g_swapchains.begin(); sc != g_swapchains.end();) {
                if (sc->second.device == device) {
                    destroy_swapchain_resources(it->second.disp, device, sc->second);
                    sc = g_swapchains.erase(sc);
                } else {
                    ++sc;
                }
            }
            g_devices.erase(it);
        }
        // Only the renderer's OWN device takes the renderer down with it. This
        // used to shut it down for any device destroyed in the process: a
        // helper device a game creates beside its main one -- a video decoder,
        // a launcher-side probe, a second adapter -- tore down the pipeline,
        // the descriptor pool and every avatar of the device that was still
        // presenting, with no wait for the command buffers reading them.
        if (vocem::renderer().device() == device) {
            vocem::journal_note("device destroyed; renderer shutting down");
            vocem::renderer().shutdown();
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
        // The same answer as GetDeviceQueue2 below, for the same failure: the
        // output must be a handle the application can test, not whatever was
        // on its stack. The two answered this differently once.
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
        // The application asked for a function this device does not have.
        // vocem_GetDeviceProcAddr no longer hands out our hook for one, but
        // vocem_GetInstanceProcAddr still answers from the intercepted table
        // before asking the chain -- so this is an ordinary road here, not an
        // accident (the previous sentence called it one). The output must be a
        // handle the application can test, not whatever was on its stack.
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

    // Named rather than numbered, because "mode 3" in a log is a number the
    // reader has to go and look up, and this line is what somebody reads when
    // the panel's colours look wrong in their game.
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

// Caller must hold g_lock.
void destroy_swapchain_resources(const DeviceDispatch& d, VkDevice device, SwapchainData& sc) {
    // Only when something of ours may still be executing. The failed-build
    // path reaches here from inside the present, before anything was ever
    // submitted, and a device-wide wait there is a stall in somebody's frame
    // for nothing (rule 8).
    if (d.DeviceWaitIdle && sc.ever_submitted()) {
        d.DeviceWaitIdle(device);
    }
    for (VkFence fence : sc.fences) {
        if (fence != VK_NULL_HANDLE) d.DestroyFence(device, fence, nullptr);
    }
    for (VkSemaphore semaphore : sc.semaphores) {
        if (semaphore != VK_NULL_HANDLE) d.DestroySemaphore(device, semaphore, nullptr);
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
            destroy_swapchain_resources(dev->disp, device, it->second);
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
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

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

        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = sc.images[i];
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = sc.format;
        view_info.components.r = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.g = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.b = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.components.a = VK_COMPONENT_SWIZZLE_IDENTITY;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        if (d.CreateImageView(dev.device, &view_info, nullptr, &sc.views[i]) != VK_SUCCESS) {
            VOCEM_LOG("image view creation failed");
            return false;
        }

        VkFramebufferCreateInfo fb_info{};
        fb_info.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fb_info.renderPass = sc.render_pass;
        fb_info.attachmentCount = 1;
        fb_info.pAttachments = &sc.views[i];
        fb_info.width = sc.extent.width;
        fb_info.height = sc.extent.height;
        fb_info.layers = 1;
        if (d.CreateFramebuffer(dev.device, &fb_info, nullptr, &sc.framebuffers[i]) != VK_SUCCESS) {
            VOCEM_LOG("framebuffer creation failed");
            return false;
        }

        VkSemaphoreCreateInfo sem_info{};
        sem_info.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        if (d.CreateSemaphore(dev.device, &sem_info, nullptr, &sc.semaphores[i]) != VK_SUCCESS) {
            return false;
        }

        // Unsignalled. The `submitted` flag is what keeps the first frame from
        // waiting on a fence nothing will signal; the fence used to be created
        // signalled for that purpose as well, and with the flag guarding both
        // the wait AND the reset, the first submit for every image then took a
        // fence that was still signalled -- VUID-vkQueueSubmit-fence-00063,
        // once per image per swapchain, under a comment that had outlived the
        // flag it was written before.
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (d.CreateFence(dev.device, &fence_info, nullptr, &sc.fences[i]) != VK_SUCCESS) {
            return false;
        }
    }

    // Every swapchain gets a pipeline of its own, built against its own render
    // pass (hdr_pipeline.h): the converting one for a colour space or a format
    // that needs it, the identity for everything else. Not only the converting
    // cases, as it was: ImGui's stock pipeline is built once, against the
    // FIRST swapchain's render pass, and a later swapchain in another format --
    // HDR switched off in the game's settings, an sRGB attachment replaced by
    // a UNORM one -- would have had it drawn into an incompatible render pass.
    // Failure is not failure of the overlay: draw_overlay() still uses the
    // stock pipeline where the format matches the one it was built for, and
    // passes the frame through where it does not, saying so in the log.
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

// Record and submit the overlay for one image. Returns the semaphore the
// present must wait on, or VK_NULL_HANDLE to leave the present untouched.
// Caller must hold g_lock. Every call down the chain in here goes through a
// pointer vkGetDeviceProcAddr resolved for the layer below, never through the
// loader's trampolines -- which is what makes holding the lock safe against
// rule 9's deadlock (the loader re-entering this layer from the top).
// A present waiting on more semaphores than this passes through undrawn: the
// submit's stage list is a stack array, sized for the one or two semaphores a
// real present waits on, so the drawn path never allocates.
constexpr uint32_t kMaxWaitSemaphores = 16;

// `wanted` is set when this frame had something to put on the screen -- past the
// poll, past the master switch and past both feature guards -- whatever happens
// after. It is what the caller gates the renderer's construction on, because
// the construction is 133 ms of atlas and 80 MB of pixels and the one thing
// worth knowing before paying it is whether there is anything to draw.
VkSemaphore draw_overlay(DeviceData& dev, SwapchainData& sc, VkQueue queue, uint32_t image_index,
                         const VkSemaphore* wait_semaphores, uint32_t wait_count, bool& wanted) {
    const DeviceDispatch& d = dev.disp;
    if (image_index >= sc.command_buffers.size() || wait_count > kMaxWaitSemaphores) {
        return VK_NULL_HANDLE;
    }

    // Nothing to show: leave the frame, and the application's synchronisation,
    // completely untouched. This is the common case and must cost nothing.
    //
    // "Nothing" is asked feature by feature: the guard used to ask only about
    // the voice channel, which made a toast outside one unreachable in both
    // injection paths -- exactly the situation a "somebody wrote to you" toast
    // exists for (vocem/panel.h holds the one spelling of these predicates).
    const vocem::Snapshot* snapshot = g_state.poll();
    if (!snapshot) {
        return VK_NULL_HANDLE;
    }
    // Nothing to show: the master switch is handled by the caller's verdict
    // now, so what is left here is the two features.
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

    // Initialisation happens after the present returns, never here: ImGui's
    // Vulkan backend uploads its font atlas with vkQueueWaitIdle, and blocking on
    // the queue from inside a queue operation is a stall at best. That upload
    // is made in prepare() explicitly, because the backend's NewFrame -- which
    // draw() below calls, inside this present -- would otherwise make it here
    // the first time (overlay_renderer.cpp says how that was found).
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
    rp_begin.framebuffer = sc.framebuffers[image_index];
    rp_begin.renderArea.extent = sc.extent;
    d.CmdBeginRenderPass(cmd, &rp_begin, VK_SUBPASS_CONTENTS_INLINE);

    vocem::renderer().draw(cmd, *snapshot, sc.extent.width, sc.extent.height, pipeline);

    d.CmdEndRenderPass(cmd);
    if (d.EndCommandBuffer(cmd) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }

    // Chain into the application's synchronisation: wait on whatever the present
    // was going to wait on, and hand the present our own semaphore instead.
    //
    // The stage list lives on the stack: this used to be a std::vector, one
    // malloc and free per drawn frame under g_lock, on the path whose own
    // header says "no allocation on the hot path". Presents wait on one or two
    // semaphores in practice; more than the array holds passes through
    // undrawn, decided at the top of this function -- past this point the
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
    return sc.semaphores[image_index];
}

// Whether the overlay must stay out of this process, from the list the window
// keeps. The OpenGL side has always had this; the Vulkan side had nothing, and
// drew in every Vulkan process on the machine -- the desktop's own compositor
// included.
//
// Asked every frame now, like the OpenGL side and like the Applications page
// promises ("takes effect in a running game within a couple of seconds"): the
// policy in vocem/draw_decision.h re-walks the lists only when they were
// edited, so the steady state is two string comparisons. This used to be a
// static decided at the first present, which made the switch live in one path
// and next-restart in the other. Letting an application back in builds the
// swapchain resources at the next present -- the same cost the first frame
// always paid; hiding one skips the drawing without releasing anything, which
// is what the Vulkan enabled switch has always done (entry 37's footnote).
//
// The decision, its evidence in the log and the word to the daemon across the
// bridge are the session's (vocem/overlay_session.h), one spelling with the GL
// side -- which is where this side's omission showed: it told the daemon
// `allowed` where the GL side told it `enabled && allowed`, so a Flatpak game
// with the master switch off kept receiving the channel and every face from a
// daemon that believed it was drawing (entry 138).
// Whether this process should be carrying the overlay at all: the lists, the
// verdict and the master switch, in one sentence the caller cannot ask by
// halves (vocem/overlay_session.h).
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
    // Whether this process is one the overlay is in a position to draw in, which
    // is what the window's list is a list of. The RECORD is acted on after the
    // present: a handful of syscalls, once in the life of a process.
    //
    // The verdict behind it is not, and this comment used to say it was. The
    // `overlay_wanted_here()` below is asked inside the lock and before the
    // present, and its first evaluation reads /proc/self/comm, /proc/self/exe,
    // /proc/self/cgroup and /proc/self/cmdline -- and, for a process none of the
    // launcher signals answer for, opens every installed desktop entry.
    // Measured by tests/apps_cost on this machine: 2.2-2.4 ms once when it falls
    // through to that last pass, 76-92 us once when a signal answers it, and
    // 0.9 ns per frame ever after.
    //
    // And "except the first frame" is still not the whole of it, which the
    // previous version of this comment claimed (the entry-114 shape, one layer
    // further in). Two steady-state costs also live inside the present, on
    // their own cadences: current_config() runs LiveConfig::current() -- one
    // stat() at most every two seconds, a full fopen-and-reparse when the file
    // moved -- both here and in draw(); and the state poll asks the segment's
    // name about itself (one shm_open, two fstats and a close) once a second,
    // StatePoll::kCadenceSeconds. That used to be a count -- 300 presents -- and
    // the count is what made the same Quit take two seconds at 144 frames and
    // ten at thirty; pacing it by the clock costs a drawing process four
    // syscalls a second at any frame rate, which is 10x the old rate at 30 fps
    // and half it at 600. Also per present, and new with that change: one
    // clock_gettime(CLOCK_MONOTONIC), which is the vDSO's on any machine whose
    // clocksource supports it (tsc here) and a real syscall on one whose does
    // not.
    //
    // So rule 8 as it holds is: no PER-FRAME blocking I/O, a once-per-process
    // verdict on the first frame, and a handful of deliberate, cadenced
    // syscalls the design accepts by name. A claim wider than that is where the
    // next violation hides (entry 42) -- and DESIGN's rule 8 is currently the
    // wider claim, which is why this paragraph exists.
    //
    // Kept that way on purpose, and not because 2.4 ms is small. Nothing can be
    // drawn before the verdict exists, so moving it past the present buys a
    // frame of latency rather than removing the work; the frame it would buy is
    // one the overlay is not on anyway, since the renderer is built post-present
    // by rule 10. Against that, it runs under a lock inside somebody's game, and
    // the cost is once, while the process is still starting, paid hardest by the
    // processes with no launcher signal -- the browsers and the compositor, not
    // the games. The sentence that used to stand here, "the Vulkan present path
    // has no frame-level test to change it under", was true when it was written
    // on 2026-08-08 and stopped being true on 2026-08-17, when
    // tests/vk_present_draw.cpp arrived (entry 129): the deferral is testable
    // work now, and what is left is the argument above rather than the absence
    // of an instrument.
    bool drawable = false;
    // Set under the lock when the verdict or the master switch turned off on
    // this present; acted on after it returns, where blocking is allowed.
    bool switched_off = false;
    vocem::RendererTarget pending_target;

    {
        std::lock_guard<std::mutex> guard(g_lock);
        DeviceData* dev = find_device(queue);
        if (!dev) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        next = dev->disp.QueuePresentKHR;

        // A process the overlay could draw in, which is what the window's list is
        // a list of -- recorded below whether or not it is allowed to, since an
        // excluded application is precisely the one somebody goes looking for in
        // that list.
        // Only the single-swapchain case is handled; multi-swapchain presents
        // pass through untouched rather than risk incorrect synchronisation.
        drawable = pPresentInfo->swapchainCount == 1;
        if (drawable) {
            auto it = g_swapchains.find(pPresentInfo->pSwapchains[0]);
            const bool want = it != g_swapchains.end() && overlay_wanted_here();
            // A verdict that turns off is a moment, not just a state: this
            // process is holding a renderer, an atlas and a descriptor set per
            // face, and "stop drawing" without "give it back" is what entry 37's
            // footnote recorded the Vulkan switch as always having done. The
            // OpenGL side has released on this transition since it had a
            // transition to release on; this is that, on the other path.
            if (g_drawing >= 0 && want != (g_drawing == 1)) {
                VOCEM_LOG("%s in '%s'", want ? "switched on" : "switched off",
                          vocem::process_name().c_str());
                switched_off = !want;
            }
            if (it != g_swapchains.end()) {
                g_drawing = want ? 1 : 0;
            }
            if (want) {
                SwapchainData& sc = it->second;

                auto family_it = dev->queue_families.find(queue);
                uint32_t family =
                    family_it == dev->queue_families.end() ? UINT32_MAX : family_it->second;
                // A family that cannot take a render pass -- a compute or a
                // transfer queue presenting, which the specification allows --
                // gets the frame back untouched, and so does a swapchain
                // presented from a family other than the one its command pool
                // was built on: a pool's buffers may only be submitted to its
                // own family. Nothing used to ask either question.
                const bool graphics =
                    family == UINT32_MAX || family >= dev->family_flags.size() ||
                    (dev->family_flags[family] & VK_QUEUE_GRAPHICS_BIT) != 0;
                if (!graphics || (sc.attempted && sc.queue_family != family)) {
                    if (!sc.said_pass_through) {
                        sc.said_pass_through = true;
                        VOCEM_LOG("not drawing into this swapchain: presented on queue family %u, "
                                  "which %s", family,
                                  graphics ? "is not the family its command pool was built on"
                                           : "has no graphics capability");
                    }
                    family = UINT32_MAX;
                }

                if (!sc.attempted && family != UINT32_MAX) {
                    if (!build_swapchain_resources(*dev, sc, pPresentInfo->pSwapchains[0], family)) {
                        destroy_swapchain_resources(dev->disp, dev->device, sc);
                    }
                }

                if (sc.usable && family != UINT32_MAX) {
                    bool wanted = false;
                    overlay_semaphore =
                        draw_overlay(*dev, sc, queue, pPresentInfo->pImageIndices[0],
                                     pPresentInfo->pWaitSemaphores,
                                     pPresentInfo->waitSemaphoreCount, wanted);
                    // Built only for a frame that had something on it. This
                    // used to be decided from the swapchain and the
                    // application's verdict alone -- everything except whether
                    // there was anything to draw -- so a game the overlay is
                    // allowed in built the whole renderer, 133 ms of atlas and
                    // 80 MB of pixels and a descriptor pool, while the owner
                    // was simply not in a voice channel: the ordinary state of
                    // a machine with the tray icon up. The OpenGL path has
                    // always had this door and one more: its draw() returns at
                    // the poll and again at these two predicates, both above
                    // ensure_backend(). The cost of asking late is the first
                    // frame with something on it, which rule 10 spends anyway.
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
                        pending_target.height = sc.extent.height;
                        pending_target.gipa = dev->gipa;
                        pending_target.gdpa = dev->disp.GetDeviceProcAddr;
                        pending_target.set_loader_data = dev->set_device_loader_data;
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
    bool daemon_attached = false;
    if (drawable) {
        std::lock_guard<std::mutex> guard(g_lock);
        auto it = g_swapchains.find(pPresentInfo->pSwapchains[0]);
        if (it != g_swapchains.end() && it->second.colour_note_pending) {
            it->second.colour_note_pending = false;
            vocem::journal_note("colour pipeline ready");
        }
        // Read under the lock the poll itself runs under, acted on below with
        // the lock let go: shutdown() takes the renderer's own.
        daemon_left = g_state.daemon_left();
        daemon_attached = g_state.attached();
    }

    // The daemon stopped -- the tray's Quit, or `systemctl --user stop`. This
    // process is holding a backend, a font atlas and a descriptor set per face
    // on its behalf, and not drawing does not hand any of it back. Here, after
    // the present has returned, which is where this side is allowed to block;
    // the next present finds the renderer not ready, sets needs_init again, and
    // prepare() builds it back if a daemon returns.
    //
    // The switch being turned off is the same situation from this side, and the
    // same answer: what is held is held on the daemon's behalf either way. What
    // "everything" means is the renderer's own objects and the font atlas -- the
    // swapchain's render pass, framebuffers, views, command pool, semaphores and
    // fences stay, because they belong to the swapchain and the next frame in
    // this game still needs them.
    if (daemon_left || switched_off) {
        VOCEM_LOG("%s: releasing the backend and the font atlas",
                  daemon_left ? "the daemon stopped" : "switched off");
        vocem::journal_note(daemon_left ? "daemon stopped: released" : "switched off: released");
        vocem::renderer().shutdown();
        vocem::fonts_release();
    }

    // Safe here: the present has returned, so the queue is ours to block on. Costs
    // one stall on the first frame that has something to draw, once per swapchain.
    //
    // And only while a daemon is publishing. `needs_init` above is decided from
    // the swapchain and the application's verdict alone, which is everything
    // except whether there is anything to draw: a game the overlay is allowed in
    // built its whole renderer -- 133 ms rasterising an atlas, 80 MB of pixels,
    // a descriptor pool -- with the daemon stopped and nothing to put in it, and
    // built it again on the present after the release below. The OpenGL path
    // never had this door, because its draw() returns at the poll, above
    // ensure_backend(). tests/vk_present_draw.cpp's daemon-gone scenario counts
    // "backend ready": twice before this line, once after.
    if (needs_init && !vocem::renderer().ready() && daemon_attached) {
        // Deliberately unlocked: prepare() resolves entry points through the
        // chain, and the loader can route those back into this layer. Its
        // answer is remembered inside: a failure is said once and not retried
        // on every present.
        vocem::renderer().prepare(pending_target);
    } else if (vocem::renderer().ready()) {
        // Avatar uploads submit and wait on a fence, so they belong here too.
        vocem::renderer().process_uploads();
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
    // the device actually has. Answering for one it does not -- vkGetDeviceQueue2
    // on a Vulkan 1.0 device, vkQueuePresentKHR without VK_KHR_swapchain -- turns
    // an application's feature detection into a call into a layer with nothing
    // below it. The shim has followed exactly this rule for dlsym since entry 35;
    // this half of the project did not.
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
    // What the loader and the application ask this entry for, by name, when
    // VOCEM_TRACE_PROCADDR=1: the measurement behind the note below on which
    // names are answered from the table before the chain is asked.
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
    // The instance-level names are answered from the table before the chain
    // is asked, and have to be: the loader asks for vkCreateInstance with a
    // null instance, before there is a chain to ask. The device-level names in
    // the same table follow vocem_GetDeviceProcAddr's rule instead (entry 70):
    // asked with an instance, the chain answers first and our hook stands in
    // only for a function the chain has. Measured with VOCEM_TRACE_PROCADDR on
    // one probe run: the loader asks this entry 107 names, every one with a
    // real instance, and none of them is a device-level name of ours -- so the
    // order was never wrong in practice, and is right now for an application
    // that asks vkGetInstanceProcAddr(instance, "vkQueuePresentKHR") itself,
    // which the specification allows.
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

namespace {

// The clean end of the crash journal: a process that unwinds normally runs
// this and takes its marker with it; a crash does not, which is the mechanism
// (vocem/journal.h).
__attribute__((destructor)) void vocem_layer_journal_close() {
    vocem::journal_end();
}

}  // namespace

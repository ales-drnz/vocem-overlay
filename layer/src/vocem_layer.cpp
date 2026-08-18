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
//     is at overlay_hidden_here's call site).
//   * Per-image state is indexed by swapchain image index, never by acquisition
//     order (MangoHud 0.8.3 fixed exactly this class of bug).
//   * If anything we need is missing, degrade to a pure pass-through. A layer
//     must never be the reason a game fails to start.
//   * Dispatchable objects we create (command buffers) must be registered with
//     the loader via pfnSetDeviceLoaderData, or their dispatch will crash.

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
// Logging. Off unless VOCEM_DEBUG=1, so release runs stay silent.
// ---------------------------------------------------------------------------

bool debug_enabled() {
    static const bool enabled = [] {
        const char* env = std::getenv("VOCEM_DEBUG");
        return env && env[0] == '1';
    }();
    return enabled;
}

#define VOCEM_LOG(...)                                    \
    do {                                                  \
        if (debug_enabled()) {                            \
            std::fprintf(stderr, "[vocem] " __VA_ARGS__); \
            std::fputc('\n', stderr);                     \
        }                                                 \
    } while (0)

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
};

struct DeviceData {
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device = VK_NULL_HANDLE;
    VkInstance instance = VK_NULL_HANDLE;
    PFN_vkGetInstanceProcAddr gipa = nullptr;
    DeviceDispatch disp;
    PFN_vkSetDeviceLoaderData set_device_loader_data = nullptr;
    std::unordered_map<VkQueue, uint32_t> queue_families;
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

// Once per process: whether this one is inside a Flatpak sandbox, and if it is,
// what was done about it.
//
// It says so either way it can fail, because the failure is invisible
// otherwise: a game whose overlay never found the bridge behaves exactly like a
// game the overlay was never asked to draw in.
void enter_flatpak_bridge_once() {
    static bool asked = false;
    if (asked) {
        return;
    }
    asked = true;
    const char* id = vocem::flatpak_app_id();
    if (!id) {
        return;  // on the host, where everything is where it has always been
    }
    if (vocem::enter_flatpak_bridge()) {
        char root[512];
        vocem::bridge_root(root, sizeof(root));
        VOCEM_LOG("inside the Flatpak sandbox of %s: state, settings and avatars come from %s", id,
                  root);
        return;
    }
    VOCEM_LOG("inside the Flatpak sandbox of %s but could not ask vocemd for a bridge: no "
              "XDG_RUNTIME_DIR, or its app directory is not writable. There will be no overlay "
              "in this process.", id);
}

VKAPI_ATTR VkResult VKAPI_CALL vocem_CreateInstance(const VkInstanceCreateInfo* pCreateInfo,
                                                    const VkAllocationCallbacks* pAllocator,
                                                    VkInstance* pInstance) {
    // Before anything derives a path: the configuration and the state are both
    // on the far side of the sandbox when the game is a Flatpak, and this is
    // where the three lookups get pointed at the bridge (vocem/flatpak.h).
    enter_flatpak_bridge_once();

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
    vocem::journal_note("device destroyed; renderer shutting down");
    PFN_vkDestroyDevice destroy = nullptr;
    {
        std::lock_guard<std::mutex> guard(g_lock);
        vocem::renderer().shutdown();
        auto it = g_devices.find(dispatch_key(device));
        if (it != g_devices.end()) {
            destroy = it->second.disp.DestroyDevice;
            g_devices.erase(it);
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
    if (d.DeviceWaitIdle) {
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

        // Created signalled so the first frame does not wait on a fence that
        // was never submitted.
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        fence_info.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        if (d.CreateFence(dev.device, &fence_info, nullptr, &sc.fences[i]) != VK_SUCCESS) {
            return false;
        }
    }

    // A swapchain the stock pipeline would paint wrongly on gets the converting
    // one (hdr_pipeline.h) -- for its colour space, or for a format that
    // carries the sRGB encoding itself, which is the ordinary case and not an
    // HDR one. Failure is not failure of the overlay: the stock pipeline still
    // draws, with the colours it drew with before this existed, and the log
    // says which happened.
    if (const int mode = vocem::hdr_mode_for(sc.color_space, sc.format)) {
        if (vocem::hdr_pipeline_create(hdr_functions(d), dev.device, sc.render_pass, mode,
                                       vocem::hdr_sdr_nits(), sc.hdr)) {
            VOCEM_LOG("colour pipeline ready (mode %d, SDR white %.0f nits)", mode,
                      vocem::hdr_sdr_nits());
            vocem::journal_note("colour pipeline ready");
        } else {
            VOCEM_LOG("colour pipeline unavailable: drawing with unconverted colours");
        }
    }

    sc.usable = true;
    VOCEM_LOG("swapchain resources ready: %u images, queue family %u", image_count, queue_family);
    return true;
}

// Record and submit the overlay for one image. Returns the semaphore the
// present must wait on, or VK_NULL_HANDLE to leave the present untouched.
// Caller must hold g_lock.
// A present waiting on more semaphores than this passes through undrawn: the
// submit's stage list is a stack array, sized for the one or two semaphores a
// real present waits on, so the drawn path never allocates.
constexpr uint32_t kMaxWaitSemaphores = 16;

VkSemaphore draw_overlay(DeviceData& dev, SwapchainData& sc, VkQueue queue, uint32_t image_index,
                         const VkSemaphore* wait_semaphores, uint32_t wait_count) {
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
    // The master switch first: with the overlay off, "off" used to mean a full
    // fence wait, an empty render pass and a submit chained into the game's
    // semaphores, with only the ImGui half skipped. Off now leaves the present
    // exactly as the application made it.
    const vocem::Config& config = vocem::renderer().current_config();
    if (!config.enabled ||
        (!vocem::panel_wanted(*snapshot, config) &&
         !vocem::notification_wanted(*snapshot, config, vocem::monotonic_seconds()))) {
        static bool logged_idle = false;
        if (!logged_idle) {
            logged_idle = true;
            VOCEM_LOG("nothing to draw: connected=%d in_channel=%d users=%u",
                      snapshot->connected ? 1 : 0, snapshot->in_channel ? 1 : 0,
                      snapshot->user_count);
        }
        return VK_NULL_HANDLE;
    }

    // Initialisation happens after the present returns, never here: ImGui's
    // Vulkan backend uploads its font atlas with vkQueueWaitIdle, and blocking on
    // the queue from inside a queue operation deadlocks the driver.
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

    vocem::renderer().draw(cmd, *snapshot, sc.extent.width, sc.extent.height, sc.hdr.pipeline);

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
bool overlay_hidden_here() {
    static vocem::DrawDecision decision;
    if (decision.refresh(vocem::renderer().current_config())) {
        // The evidence, not just the verdict: a game that is missed has to be a
        // case somebody can read off one line of the log.
        if (decision.allowed()) {
            VOCEM_LOG("drawing in '%s': %s", vocem::process_name().c_str(),
                      vocem::game_verdict().reason.c_str());
        } else {
            VOCEM_LOG("not drawing in '%s': %s (%s)", vocem::process_name().c_str(),
                      vocem::looks_like_game() ? "on the hidden list"
                                               : "does not look like a game",
                      vocem::game_verdict().reason.c_str());
        }
    }
    // Inside a Flatpak, tell the daemon. It adopted this sandbox before the
    // settings could be read, because the settings arrive across the bridge;
    // this is where it learns whether the overlay actually belongs here, and
    // whether to keep sending the channel and the faces at all.
    vocem::flatpak_bridge_drawing(decision.allowed());
    return !decision.allowed();
}

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
    // `overlay_hidden_here()` below is asked inside the lock and before the
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
    // moved -- both here and in draw(); and the state poll asks
    // still_current() (one shm_open, two fstats) every 300 presents. So rule 8
    // as it holds is: no PER-FRAME blocking I/O, a once-per-process verdict on
    // the first frame, and a handful of deliberate, cadenced syscalls the
    // design accepts by name. A claim wider than that is where the next
    // violation hides (entry 42).
    //
    // Kept that way on purpose, and not because 2.4 ms is small. Nothing can be
    // drawn before the verdict exists, so moving it past the present buys a
    // frame of latency rather than removing the work; the frame it would buy is
    // one the overlay is not on anyway, since the renderer is built post-present
    // by rule 10. Against that: the Vulkan present path has no frame-level test
    // to change it under (entry 70 is that finding), and this runs under a lock
    // inside somebody's game. The cost is once, while the process is still
    // starting, and it is the processes with no launcher signal -- the browsers
    // and the compositor, not the games -- that pay the 2.4 ms.
    bool drawable = false;
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
            if (it != g_swapchains.end() && !overlay_hidden_here()) {
                SwapchainData& sc = it->second;

                auto family_it = dev->queue_families.find(queue);
                uint32_t family =
                    family_it == dev->queue_families.end() ? UINT32_MAX : family_it->second;

                if (!sc.attempted && family != UINT32_MAX) {
                    if (!build_swapchain_resources(*dev, sc, pPresentInfo->pSwapchains[0], family)) {
                        destroy_swapchain_resources(dev->disp, dev->device, sc);
                    }
                }

                if (sc.usable) {
                    if (!vocem::renderer().ready()) {
                        needs_init = true;
                        pending_target.instance = dev->instance;
                        pending_target.physical_device = dev->physical_device;
                        pending_target.device = dev->device;
                        pending_target.render_pass = sc.render_pass;
                        pending_target.queue = queue;
                        pending_target.queue_family = sc.queue_family;
                        pending_target.image_count = static_cast<uint32_t>(sc.images.size());
                        pending_target.height = sc.extent.height;
                        pending_target.gipa = dev->gipa;
                        pending_target.gdpa = dev->disp.GetDeviceProcAddr;
                        pending_target.set_loader_data = dev->set_device_loader_data;
                    }
                    overlay_semaphore =
                        draw_overlay(*dev, sc, queue, pPresentInfo->pImageIndices[0],
                                     pPresentInfo->pWaitSemaphores,
                                     pPresentInfo->waitSemaphoreCount);
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

    // Safe here: the present has returned, so the queue is ours to block on. Costs
    // one stall on the first frame that has something to draw, once per swapchain.
    if (needs_init && !vocem::renderer().ready()) {
        // Deliberately unlocked: prepare() resolves entry points through the
        // chain, and the loader can route those back into this layer.
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
    if (std::strcmp(pName, "vkGetInstanceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(vocem_GetInstanceProcAddr);
    }
    if (std::strcmp(pName, "vkGetDeviceProcAddr") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(vocem_GetDeviceProcAddr);
    }
    if (PFN_vkVoidFunction func = find_intercepted(pName)) {
        return func;
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
    return next ? next(instance, pName) : nullptr;
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

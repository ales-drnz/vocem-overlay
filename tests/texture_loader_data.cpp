// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The avatar upload's command buffer must be registered with the loader.
//
// A command buffer is a dispatchable object, and layer rule 4 (vocem_layer.cpp's
// own header) is that every dispatchable object the layer creates goes through
// pfnSetDeviceLoaderData, or dispatch on it will crash. The layer honours that
// for the per-swapchain command buffers it records the overlay into -- and the
// TextureCache allocated one more for the avatar staging copy, under a comment
// claiming the renderer had registered it, which nothing did. A layer below ours
// in the chain keys its own bookkeeping on the dispatch pointer the loader
// writes into the handle; hand it an unregistered one and its lookup misses.
//
// The whole upload path is exercised against a stub Vulkan: TextureCache already
// takes its entry points through a resolver, so the test supplies its own and
// watches the order of events. BeginCommandBuffer fails on purpose -- the
// defect's window is between allocation and first use, and stopping there keeps
// ImGui's real backend out of a test about the loader contract.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "texture_cache.h"
#include "vocem/avatar_rgba.h"

// Referenced by TextureCache::upload()/destroy(); never reached here because
// BeginCommandBuffer fails first. Stubbed so the test does not need the real
// backend, whose init wants a living device.
VkDescriptorSet ImGui_ImplVulkan_AddTexture(VkSampler, VkImageView, VkImageLayout) {
    return VK_NULL_HANDLE;
}
void ImGui_ImplVulkan_RemoveTexture(VkDescriptorSet) {}

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

// The order of events is the claim under test: allocated, registered, then used.
int counter = 0;
int alloc_order = 0;
int register_order = 0;
int begin_order = 0;

int fake_device_storage = 0;
int fake_cmd_storage = 0;
VkDevice fake_device() { return reinterpret_cast<VkDevice>(&fake_device_storage); }
VkCommandBuffer fake_cmd() { return reinterpret_cast<VkCommandBuffer>(&fake_cmd_storage); }

VkDevice registered_device = VK_NULL_HANDLE;
void* registered_object = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL stub_set_loader_data(VkDevice device, void* object) {
    register_order = ++counter;
    registered_device = device;
    registered_object = object;
    return VK_SUCCESS;
}

// --- stub Vulkan: every entry point the cache resolves -----------------------

template <typename T>
T fake_handle(uintptr_t value) {
    return reinterpret_cast<T>(value);
}

VKAPI_ATTR void VKAPI_CALL stub_GetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* out) {
    memset(out, 0, sizeof(*out));
    out->memoryTypeCount = 1;
    out->memoryTypes[0].propertyFlags =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}
VKAPI_ATTR VkResult VKAPI_CALL stub_CreateImage(VkDevice, const VkImageCreateInfo*,
                                                const VkAllocationCallbacks*, VkImage* out) {
    *out = fake_handle<VkImage>(0x1001);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL stub_DestroyImage(VkDevice, VkImage, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL stub_CreateImageView(VkDevice, const VkImageViewCreateInfo*,
                                                    const VkAllocationCallbacks*,
                                                    VkImageView* out) {
    *out = fake_handle<VkImageView>(0x1002);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL stub_DestroyImageView(VkDevice, VkImageView,
                                                 const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL stub_CreateSampler(VkDevice, const VkSamplerCreateInfo*,
                                                  const VkAllocationCallbacks*, VkSampler* out) {
    *out = fake_handle<VkSampler>(0x1003);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL stub_DestroySampler(VkDevice, VkSampler,
                                               const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL stub_AllocateMemory(VkDevice, const VkMemoryAllocateInfo*,
                                                   const VkAllocationCallbacks*,
                                                   VkDeviceMemory* out) {
    *out = fake_handle<VkDeviceMemory>(0x1004);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL stub_FreeMemory(VkDevice, VkDeviceMemory,
                                           const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL stub_BindImageMemory(VkDevice, VkImage, VkDeviceMemory,
                                                    VkDeviceSize) {
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL stub_GetImageMemoryRequirements(VkDevice, VkImage,
                                                           VkMemoryRequirements* out) {
    out->size = vocem::kAvatarRgbaBytes;
    out->alignment = 4;
    out->memoryTypeBits = 1;
}
VKAPI_ATTR VkResult VKAPI_CALL stub_CreateBuffer(VkDevice, const VkBufferCreateInfo*,
                                                 const VkAllocationCallbacks*, VkBuffer* out) {
    *out = fake_handle<VkBuffer>(0x1005);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL stub_DestroyBuffer(VkDevice, VkBuffer, const VkAllocationCallbacks*) {}
VKAPI_ATTR void VKAPI_CALL stub_GetBufferMemoryRequirements(VkDevice, VkBuffer,
                                                            VkMemoryRequirements* out) {
    out->size = vocem::kAvatarRgbaBytes;
    out->alignment = 4;
    out->memoryTypeBits = 1;
}
VKAPI_ATTR VkResult VKAPI_CALL stub_BindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory,
                                                     VkDeviceSize) {
    return VK_SUCCESS;
}
unsigned char mapped_storage[vocem::kAvatarRgbaBytes];
VKAPI_ATTR VkResult VKAPI_CALL stub_MapMemory(VkDevice, VkDeviceMemory, VkDeviceSize,
                                              VkDeviceSize, VkMemoryMapFlags, void** out) {
    *out = mapped_storage;
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL stub_UnmapMemory(VkDevice, VkDeviceMemory) {}
VKAPI_ATTR VkResult VKAPI_CALL stub_CreateCommandPool(VkDevice, const VkCommandPoolCreateInfo*,
                                                      const VkAllocationCallbacks*,
                                                      VkCommandPool* out) {
    *out = fake_handle<VkCommandPool>(0x1006);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL stub_DestroyCommandPool(VkDevice, VkCommandPool,
                                                   const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL stub_AllocateCommandBuffers(VkDevice,
                                                           const VkCommandBufferAllocateInfo*,
                                                           VkCommandBuffer* out) {
    alloc_order = ++counter;
    *out = fake_cmd();
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL stub_FreeCommandBuffers(VkDevice, VkCommandPool, uint32_t,
                                                   const VkCommandBuffer*) {}
VKAPI_ATTR VkResult VKAPI_CALL stub_BeginCommandBuffer(VkCommandBuffer,
                                                       const VkCommandBufferBeginInfo*) {
    begin_order = ++counter;
    // Fail here on purpose: the claim under test is entirely between the
    // allocation and this first dispatch, and stopping now keeps the real ImGui
    // backend (AddTexture needs a living one) out of the test.
    return VK_ERROR_DEVICE_LOST;
}
VKAPI_ATTR VkResult VKAPI_CALL stub_EndCommandBuffer(VkCommandBuffer) { return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL stub_CmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags,
                                                   VkPipelineStageFlags, VkDependencyFlags,
                                                   uint32_t, const VkMemoryBarrier*, uint32_t,
                                                   const VkBufferMemoryBarrier*, uint32_t,
                                                   const VkImageMemoryBarrier*) {}
VKAPI_ATTR void VKAPI_CALL stub_CmdCopyBufferToImage(VkCommandBuffer, VkBuffer, VkImage,
                                                     VkImageLayout, uint32_t,
                                                     const VkBufferImageCopy*) {}
VKAPI_ATTR VkResult VKAPI_CALL stub_QueueSubmit(VkQueue, uint32_t, const VkSubmitInfo*,
                                                VkFence) {
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL stub_CreateFence(VkDevice, const VkFenceCreateInfo*,
                                                const VkAllocationCallbacks*, VkFence* out) {
    *out = fake_handle<VkFence>(0x1007);
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL stub_DestroyFence(VkDevice, VkFence, const VkAllocationCallbacks*) {}
VKAPI_ATTR VkResult VKAPI_CALL stub_WaitForFences(VkDevice, uint32_t, const VkFence*, VkBool32,
                                                  uint64_t) {
    return VK_SUCCESS;
}

struct NameAndFunction {
    const char* name;
    PFN_vkVoidFunction func;
};

PFN_vkVoidFunction resolve(const char* name, void*) {
    static const NameAndFunction table[] = {
        {"vkGetPhysicalDeviceMemoryProperties",
         reinterpret_cast<PFN_vkVoidFunction>(stub_GetPhysicalDeviceMemoryProperties)},
        {"vkCreateImage", reinterpret_cast<PFN_vkVoidFunction>(stub_CreateImage)},
        {"vkDestroyImage", reinterpret_cast<PFN_vkVoidFunction>(stub_DestroyImage)},
        {"vkCreateImageView", reinterpret_cast<PFN_vkVoidFunction>(stub_CreateImageView)},
        {"vkDestroyImageView", reinterpret_cast<PFN_vkVoidFunction>(stub_DestroyImageView)},
        {"vkCreateSampler", reinterpret_cast<PFN_vkVoidFunction>(stub_CreateSampler)},
        {"vkDestroySampler", reinterpret_cast<PFN_vkVoidFunction>(stub_DestroySampler)},
        {"vkAllocateMemory", reinterpret_cast<PFN_vkVoidFunction>(stub_AllocateMemory)},
        {"vkFreeMemory", reinterpret_cast<PFN_vkVoidFunction>(stub_FreeMemory)},
        {"vkBindImageMemory", reinterpret_cast<PFN_vkVoidFunction>(stub_BindImageMemory)},
        {"vkGetImageMemoryRequirements",
         reinterpret_cast<PFN_vkVoidFunction>(stub_GetImageMemoryRequirements)},
        {"vkCreateBuffer", reinterpret_cast<PFN_vkVoidFunction>(stub_CreateBuffer)},
        {"vkDestroyBuffer", reinterpret_cast<PFN_vkVoidFunction>(stub_DestroyBuffer)},
        {"vkGetBufferMemoryRequirements",
         reinterpret_cast<PFN_vkVoidFunction>(stub_GetBufferMemoryRequirements)},
        {"vkBindBufferMemory", reinterpret_cast<PFN_vkVoidFunction>(stub_BindBufferMemory)},
        {"vkMapMemory", reinterpret_cast<PFN_vkVoidFunction>(stub_MapMemory)},
        {"vkUnmapMemory", reinterpret_cast<PFN_vkVoidFunction>(stub_UnmapMemory)},
        {"vkCreateCommandPool", reinterpret_cast<PFN_vkVoidFunction>(stub_CreateCommandPool)},
        {"vkDestroyCommandPool", reinterpret_cast<PFN_vkVoidFunction>(stub_DestroyCommandPool)},
        {"vkAllocateCommandBuffers",
         reinterpret_cast<PFN_vkVoidFunction>(stub_AllocateCommandBuffers)},
        {"vkFreeCommandBuffers", reinterpret_cast<PFN_vkVoidFunction>(stub_FreeCommandBuffers)},
        {"vkBeginCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(stub_BeginCommandBuffer)},
        {"vkEndCommandBuffer", reinterpret_cast<PFN_vkVoidFunction>(stub_EndCommandBuffer)},
        {"vkCmdPipelineBarrier", reinterpret_cast<PFN_vkVoidFunction>(stub_CmdPipelineBarrier)},
        {"vkCmdCopyBufferToImage",
         reinterpret_cast<PFN_vkVoidFunction>(stub_CmdCopyBufferToImage)},
        {"vkQueueSubmit", reinterpret_cast<PFN_vkVoidFunction>(stub_QueueSubmit)},
        {"vkCreateFence", reinterpret_cast<PFN_vkVoidFunction>(stub_CreateFence)},
        {"vkDestroyFence", reinterpret_cast<PFN_vkVoidFunction>(stub_DestroyFence)},
        {"vkWaitForFences", reinterpret_cast<PFN_vkVoidFunction>(stub_WaitForFences)},
    };
    for (const NameAndFunction& entry : table) {
        if (strcmp(entry.name, name) == 0) {
            return entry.func;
        }
    }
    return nullptr;
}

}  // namespace

int main() {
    // A private cache directory, so the avatar file the upload reads is this
    // test's own and nothing of the user's is touched.
    char root[] = "/tmp/vocem-loader-data-XXXXXX";
    if (!mkdtemp(root)) {
        printf("FAIL could not create a scratch directory\n");
        return 1;
    }
    setenv("XDG_CACHE_HOME", root, 1);

    char dir[512];
    vocem::avatar_cache_dir(dir, sizeof(dir));
    char parent[512];
    snprintf(parent, sizeof(parent), "%s/vocem", root);
    mkdir(parent, 0700);
    mkdir(dir, 0700);

    const uint64_t user_id = 1018972252676554842ull;
    const char* hash = "0123456789abcdef0123456789abcdef";
    char path[768];
    vocem::avatar_rgba_path(path, sizeof(path), user_id, hash);

    static unsigned char pixels[vocem::kAvatarRgbaBytes];
    memset(pixels, 0x7f, sizeof(pixels));
    check(vocem::avatar_rgba_write(path, pixels, vocem::kAvatarPixels, vocem::kAvatarPixels),
          "the daemon's own writer puts an avatar in the scratch cache");

    vocem::TextureCache cache;
    check(cache.init(fake_device(), fake_handle<VkPhysicalDevice>(0x2001),
                     fake_handle<VkQueue>(0x2002), 0, resolve, nullptr, stub_set_loader_data),
          "the cache initialises against the stub Vulkan");

    check(cache.get(user_id, hash) == 0, "the first ask queues the file and returns no texture");

    // Post-present: the upload runs, allocates its command buffer, and fails at
    // BeginCommandBuffer by the stub's design.
    cache.process_pending();

    check(alloc_order > 0, "the upload allocated a command buffer");
    check(register_order > 0,
          "the command buffer was handed to pfnSetDeviceLoaderData (layer rule 4)");
    check(register_order > alloc_order, "after it was allocated");
    check(begin_order > 0 && register_order < begin_order,
          "and before the first dispatch on it");
    check(registered_object == fake_cmd(), "the registered handle is the allocated one");
    check(registered_device == fake_device(), "on the device that allocated it");

    cache.shutdown();
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

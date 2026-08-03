// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The avatar cache must never outgrow the descriptor pool it draws from.
//
// The backend's descriptor pool is created with a fixed number of sets, and
// ImGui_ImplVulkan_AddTexture on an exhausted pool does not fail -- it ignores
// the allocation error and hands vkUpdateDescriptorSets an uninitialised
// descriptor set, which the NVIDIA driver dereferences and dies on. That is
// not a theory: it is the Minecraft crash of 2026-07-29, five times over,
// symbolised frame by frame out of the core dump -- vocem_QueuePresentKHR ->
// process_uploads -> process_pending -> upload -> AddTexture -> SIGSEGV in
// libnvidia-glcore, SEGV_MAPERR at 0x108. The pool held 8 sets: the font
// atlas plus seven faces, and the eighth person to join a call -- the owner's
// friend, whose picture was mid-download -- was the crash.
//
// The cache therefore budgets itself: at most kMaxAvatarDescriptors live
// descriptor sets, and a face beyond the budget stays the grey placeholder --
// logged, never crashed into. This test walks more distinct faces than the
// budget through the stub Vulkan and counts what reaches AddTexture.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "texture_cache.h"
#include "vocem/avatar_rgba.h"

namespace {
int add_texture_calls = 0;
int remove_texture_calls = 0;
}  // namespace

// The real backend is not linked: AddTexture is counted instead, and hands
// back a distinct non-null set each time, as a healthy pool would.
VkDescriptorSet ImGui_ImplVulkan_AddTexture(VkSampler, VkImageView, VkImageLayout) {
    ++add_texture_calls;
    return reinterpret_cast<VkDescriptorSet>(static_cast<uintptr_t>(0x9000 + add_texture_calls));
}
void ImGui_ImplVulkan_RemoveTexture(VkDescriptorSet) { ++remove_texture_calls; }

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    printf("%s %s\n", condition ? "ok  " : "FAIL", what);
    if (!condition) {
        ++failures;
    }
}

template <typename T>
T fake_handle(uintptr_t value) {
    return reinterpret_cast<T>(value);
}

int fake_device_storage = 0;
int fake_cmd_storage = 0;

VKAPI_ATTR VkResult VKAPI_CALL stub_set_loader_data(VkDevice, void*) { return VK_SUCCESS; }

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
    *out = fake_handle<VkCommandBuffer>(reinterpret_cast<uintptr_t>(&fake_cmd_storage));
    return VK_SUCCESS;
}
VKAPI_ATTR void VKAPI_CALL stub_FreeCommandBuffers(VkDevice, VkCommandPool, uint32_t,
                                                   const VkCommandBuffer*) {}
VKAPI_ATTR VkResult VKAPI_CALL stub_BeginCommandBuffer(VkCommandBuffer,
                                                       const VkCommandBufferBeginInfo*) {
    return VK_SUCCESS;
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
    char root[] = "/tmp/vocem-descriptor-budget-XXXXXX";
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

    static unsigned char pixels[vocem::kAvatarRgbaBytes];
    memset(pixels, 0x55, sizeof(pixels));

    vocem::TextureCache cache;
    check(cache.init(fake_handle<VkDevice>(reinterpret_cast<uintptr_t>(&fake_device_storage)),
                     fake_handle<VkPhysicalDevice>(0x2001), fake_handle<VkQueue>(0x2002), 0,
                     resolve, nullptr, stub_set_loader_data),
          "the cache initialises against the stub Vulkan");

    // More distinct faces than the budget, every file already on disk -- the
    // situation five Minecraft crashes were made of, in miniature.
    const uint32_t kFaces = vocem::TextureCache::kMaxAvatarDescriptors + 5;
    const char* hash = "0123456789abcdef0123456789abcdef";
    for (uint32_t i = 0; i < kFaces; ++i) {
        char path[768];
        vocem::avatar_rgba_path(path, sizeof(path), 1000 + i, hash);
        if (!vocem::avatar_rgba_write(path, pixels, vocem::kAvatarPixels, vocem::kAvatarPixels)) {
            printf("FAIL could not write avatar file %u\n", i);
            return 1;
        }
        cache.get(1000 + i, hash);
    }

    // One upload per post-present pass, as in a game.
    for (uint32_t i = 0; i < kFaces + 8; ++i) {
        cache.process_pending();
    }

    printf("     AddTexture calls: %d (budget %u)\n", add_texture_calls,
           vocem::TextureCache::kMaxAvatarDescriptors);
    check(add_texture_calls <= static_cast<int>(vocem::TextureCache::kMaxAvatarDescriptors),
          "the cache never asks for more descriptor sets than its budget");

    // The faces beyond the budget must resolve -- to the placeholder, at once,
    // not to a retry loop that will exhaust the pool a second later.
    int beyond_with_texture = 0;
    for (uint32_t i = vocem::TextureCache::kMaxAvatarDescriptors; i < kFaces; ++i) {
        if (cache.get(1000 + i, hash) != 0) {
            ++beyond_with_texture;
        }
    }
    check(beyond_with_texture == 0, "a face beyond the budget draws the placeholder");
    check(add_texture_calls <= static_cast<int>(vocem::TextureCache::kMaxAvatarDescriptors),
          "and asking again does not grow the count");

    cache.shutdown();
    check(remove_texture_calls == add_texture_calls,
          "every set taken from the pool goes back to it");

    char cleanup[600];
    snprintf(cleanup, sizeof(cleanup), "rm -rf %s", root);
    if (system(cleanup) != 0) {
        // best-effort scratch cleanup
    }
    printf("%s\n", failures == 0 ? "all checks passed" : "FAILURES");
    return failures == 0 ? 0 : 1;
}

// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// A stub Vulkan device for tests that drive the layer's TextureCache through
// its resolver seam: every call succeeds, handles are distinct, and what the
// tests ask about is counted. Two knobs: `fence_status` is what GetFenceStatus
// answers (VK_NOT_READY holds the GPU behind), `fail_create_image` fails the
// next vkCreateImage once. The real ImGui backend is not linked; AddTexture
// hands back a distinct non-null set each time, as a healthy pool would.
//
// One spelling for the stubs texture_descriptor_budget.cpp and
// texture_font_copies.cpp share (texture_loader_data.cpp's are its own: its
// subject is the ORDER of three calls, and its BeginCommandBuffer fails).

#pragma once

#include <stdint.h>
#include <string.h>

#include "texture_cache.h"
#include "vocem/avatar_rgba.h"

namespace vk_stub {

inline int add_texture_calls = 0;
inline int remove_texture_calls = 0;
inline int images_created = 0;
inline int images_destroyed = 0;
inline int buffers_created = 0;
inline int buffers_destroyed = 0;
inline int fence_waits = 0;
inline int queue_idles = 0;
inline VkResult fence_status = VK_SUCCESS;
inline bool fail_create_image = false;
// What MapMemory hands back: big enough for a face and for the small font
// atlases the font tests use.
inline unsigned char mapped_storage[1 << 20];

template <typename T>
T fake_handle(uintptr_t value) {
    return reinterpret_cast<T>(value);
}

inline int fake_device_storage = 0;
inline int fake_cmd_storage = 0;

inline VKAPI_ATTR VkResult VKAPI_CALL stub_set_loader_data(VkDevice, void*) { return VK_SUCCESS; }

inline VKAPI_ATTR void VKAPI_CALL stub_GetPhysicalDeviceMemoryProperties(
    VkPhysicalDevice, VkPhysicalDeviceMemoryProperties* out) {
    memset(out, 0, sizeof(*out));
    out->memoryTypeCount = 1;
    out->memoryTypes[0].propertyFlags =
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
}
inline VKAPI_ATTR VkResult VKAPI_CALL stub_CreateImage(VkDevice, const VkImageCreateInfo*,
                                                const VkAllocationCallbacks*, VkImage* out) {
    if (fail_create_image) {
        fail_create_image = false;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    ++images_created;
    *out = fake_handle<VkImage>(0x1000000 + images_created);
    return VK_SUCCESS;
}
inline VKAPI_ATTR void VKAPI_CALL stub_DestroyImage(VkDevice, VkImage image, const VkAllocationCallbacks*) {
    if (image != VK_NULL_HANDLE) {
        ++images_destroyed;
    }
}
inline VKAPI_ATTR VkResult VKAPI_CALL stub_CreateImageView(VkDevice, const VkImageViewCreateInfo*,
                                                    const VkAllocationCallbacks*,
                                                    VkImageView* out) {
    *out = fake_handle<VkImageView>(0x1002);
    return VK_SUCCESS;
}
inline VKAPI_ATTR void VKAPI_CALL stub_DestroyImageView(VkDevice, VkImageView,
                                                 const VkAllocationCallbacks*) {}
inline VKAPI_ATTR VkResult VKAPI_CALL stub_CreateSampler(VkDevice, const VkSamplerCreateInfo*,
                                                  const VkAllocationCallbacks*, VkSampler* out) {
    *out = fake_handle<VkSampler>(0x1003);
    return VK_SUCCESS;
}
inline VKAPI_ATTR void VKAPI_CALL stub_DestroySampler(VkDevice, VkSampler,
                                               const VkAllocationCallbacks*) {}
inline VKAPI_ATTR VkResult VKAPI_CALL stub_AllocateMemory(VkDevice, const VkMemoryAllocateInfo*,
                                                   const VkAllocationCallbacks*,
                                                   VkDeviceMemory* out) {
    *out = fake_handle<VkDeviceMemory>(0x1004);
    return VK_SUCCESS;
}
inline VKAPI_ATTR void VKAPI_CALL stub_FreeMemory(VkDevice, VkDeviceMemory,
                                           const VkAllocationCallbacks*) {}
inline VKAPI_ATTR VkResult VKAPI_CALL stub_BindImageMemory(VkDevice, VkImage, VkDeviceMemory,
                                                    VkDeviceSize) {
    return VK_SUCCESS;
}
inline VKAPI_ATTR void VKAPI_CALL stub_GetImageMemoryRequirements(VkDevice, VkImage,
                                                           VkMemoryRequirements* out) {
    out->size = vocem::kAvatarRgbaBytes;
    out->alignment = 4;
    out->memoryTypeBits = 1;
}
inline VKAPI_ATTR VkResult VKAPI_CALL stub_CreateBuffer(VkDevice, const VkBufferCreateInfo*,
                                                 const VkAllocationCallbacks*, VkBuffer* out) {
    ++buffers_created;
    *out = fake_handle<VkBuffer>(0x2000000 + buffers_created);
    return VK_SUCCESS;
}
inline VKAPI_ATTR void VKAPI_CALL stub_DestroyBuffer(VkDevice, VkBuffer buffer,
                                              const VkAllocationCallbacks*) {
    if (buffer != VK_NULL_HANDLE) {
        ++buffers_destroyed;
    }
}
inline VKAPI_ATTR void VKAPI_CALL stub_GetBufferMemoryRequirements(VkDevice, VkBuffer,
                                                            VkMemoryRequirements* out) {
    out->size = sizeof(mapped_storage);
    out->alignment = 4;
    out->memoryTypeBits = 1;
}
inline VKAPI_ATTR VkResult VKAPI_CALL stub_BindBufferMemory(VkDevice, VkBuffer, VkDeviceMemory,
                                                     VkDeviceSize) {
    return VK_SUCCESS;
}
inline VKAPI_ATTR VkResult VKAPI_CALL stub_MapMemory(VkDevice, VkDeviceMemory, VkDeviceSize,
                                              VkDeviceSize, VkMemoryMapFlags, void** out) {
    *out = mapped_storage;
    return VK_SUCCESS;
}
inline VKAPI_ATTR void VKAPI_CALL stub_UnmapMemory(VkDevice, VkDeviceMemory) {}
inline VKAPI_ATTR VkResult VKAPI_CALL stub_CreateCommandPool(VkDevice, const VkCommandPoolCreateInfo*,
                                                      const VkAllocationCallbacks*,
                                                      VkCommandPool* out) {
    *out = fake_handle<VkCommandPool>(0x1006);
    return VK_SUCCESS;
}
inline VKAPI_ATTR void VKAPI_CALL stub_DestroyCommandPool(VkDevice, VkCommandPool,
                                                   const VkAllocationCallbacks*) {}
inline VKAPI_ATTR VkResult VKAPI_CALL stub_AllocateCommandBuffers(VkDevice,
                                                           const VkCommandBufferAllocateInfo*,
                                                           VkCommandBuffer* out) {
    *out = fake_handle<VkCommandBuffer>(reinterpret_cast<uintptr_t>(&fake_cmd_storage));
    return VK_SUCCESS;
}
inline VKAPI_ATTR void VKAPI_CALL stub_FreeCommandBuffers(VkDevice, VkCommandPool, uint32_t,
                                                   const VkCommandBuffer*) {}
inline VKAPI_ATTR VkResult VKAPI_CALL stub_BeginCommandBuffer(VkCommandBuffer,
                                                       const VkCommandBufferBeginInfo*) {
    return VK_SUCCESS;
}
inline VKAPI_ATTR VkResult VKAPI_CALL stub_EndCommandBuffer(VkCommandBuffer) { return VK_SUCCESS; }
inline VKAPI_ATTR void VKAPI_CALL stub_CmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags,
                                                   VkPipelineStageFlags, VkDependencyFlags,
                                                   uint32_t, const VkMemoryBarrier*, uint32_t,
                                                   const VkBufferMemoryBarrier*, uint32_t,
                                                   const VkImageMemoryBarrier*) {}
inline VKAPI_ATTR void VKAPI_CALL stub_CmdCopyBufferToImage(VkCommandBuffer, VkBuffer, VkImage,
                                                     VkImageLayout, uint32_t,
                                                     const VkBufferImageCopy*) {}
inline VKAPI_ATTR VkResult VKAPI_CALL stub_QueueSubmit(VkQueue, uint32_t, const VkSubmitInfo*,
                                                VkFence) {
    return VK_SUCCESS;
}
inline VKAPI_ATTR VkResult VKAPI_CALL stub_CreateFence(VkDevice, const VkFenceCreateInfo*,
                                                const VkAllocationCallbacks*, VkFence* out) {
    *out = fake_handle<VkFence>(0x1007);
    return VK_SUCCESS;
}
inline VKAPI_ATTR void VKAPI_CALL stub_DestroyFence(VkDevice, VkFence, const VkAllocationCallbacks*) {}
inline VKAPI_ATTR VkResult VKAPI_CALL stub_WaitForFences(VkDevice, uint32_t, const VkFence*, VkBool32,
                                                  uint64_t) {
    ++fence_waits;
    return VK_SUCCESS;
}
// Resolved at init since entry 192: the font texture's upload and the face
// uploads, which are no longer waited for, ask the fence with GetFenceStatus
// on a later call. Signalled at once by default, so a face finishes on the
// next process_pending(); `fence_status` holds a GPU behind.
inline VKAPI_ATTR VkResult VKAPI_CALL stub_GetFenceStatus(VkDevice, VkFence) { return fence_status; }
inline VKAPI_ATTR VkResult VKAPI_CALL stub_QueueWaitIdle(VkQueue) {
    ++queue_idles;
    return VK_SUCCESS;
}

struct NameAndFunction {
    const char* name;
    PFN_vkVoidFunction func;
};

inline PFN_vkVoidFunction resolve(const char* name, void*) {
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
        {"vkGetFenceStatus", reinterpret_cast<PFN_vkVoidFunction>(stub_GetFenceStatus)},
        {"vkQueueWaitIdle", reinterpret_cast<PFN_vkVoidFunction>(stub_QueueWaitIdle)},
    };
    for (const NameAndFunction& entry : table) {
        if (strcmp(entry.name, name) == 0) {
            return entry.func;
        }
    }
    return nullptr;
}


inline VkDevice device() {
    return fake_handle<VkDevice>(reinterpret_cast<uintptr_t>(&fake_device_storage));
}

}  // namespace vk_stub

// Not inline: texture_cache.cpp is the only caller, and an inline definition
// that nothing in this translation unit calls is never emitted. Include this
// header from one source file per test.
VkDescriptorSet ImGui_ImplVulkan_AddTexture(VkSampler, VkImageView, VkImageLayout) {
    ++vk_stub::add_texture_calls;
    return reinterpret_cast<VkDescriptorSet>(
        static_cast<uintptr_t>(0x9000 + vk_stub::add_texture_calls));
}
void ImGui_ImplVulkan_RemoveTexture(VkDescriptorSet) { ++vk_stub::remove_texture_calls; }

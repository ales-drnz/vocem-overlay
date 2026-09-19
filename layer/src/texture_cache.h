// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Avatar textures for the in-game panel.
//
// The daemon has already put raw RGBA files in the cache directory
// (vocem/avatar_rgba.h is the whole format); this only turns them into Vulkan
// images. Two rules shape the design:
//
//   * Uploading needs a command buffer submit and a fence wait, so it happens in
//     the post-present phase, never inside vkQueuePresentKHR (layer rule 10).
//   * One upload per frame at most. A busy channel filling up must not cost a
//     visible hitch; a few frames without an avatar is invisible.

#ifndef VOCEM_TEXTURE_CACHE_H
#define VOCEM_TEXTURE_CACHE_H

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "imgui.h"
#include "vocem/avatar_file.h"
#include "vocem/avatar_key.h"
#include "vocem/shared_state.h"

namespace vocem {

// Resolves Vulkan entry points through the layer's dispatch chain.
using FunctionResolver = PFN_vkVoidFunction (*)(const char* name, void* user_data);

class TextureCache {
public:
    // The cache's descriptor budget. Every live avatar texture holds one
    // descriptor set from the backend's pool, and ImGui_ImplVulkan_AddTexture
    // on an exhausted pool does not fail -- it ignores the allocation error
    // and updates an uninitialised set, which the NVIDIA driver dies on
    // (entry 46: the Minecraft crash, the eighth face in a call). So the pool
    // is sized from here (see OverlayRenderer's DescriptorPoolSize) and the
    // cache stops at the budget: a face beyond it is the grey placeholder,
    // logged, never a crash. 48 covers the 24 panel slots plus toast authors
    // and avatar changes within one session.
    static constexpr uint32_t kMaxAvatarDescriptors = 48;
    // "48 covers the 24 panel slots" is the sentence above as a fact rather
    // than prose: raising kMaxUsers without room here would put mid-session
    // placeholders on faces the panel is entitled to.
    static_assert(kMaxAvatarDescriptors >= 2 * vocem::kMaxUsers,
                  "the descriptor budget must cover a full panel with headroom");
    // What the backend's pool must hold: the budget, the font atlas set, and
    // headroom so a rebuild or a future fixed texture cannot land exactly on
    // the edge.
    static constexpr uint32_t kDescriptorPoolSets = kMaxAvatarDescriptors + 8;

    // set_loader_data is the loader's pfnSetDeviceLoaderData for this device:
    // the upload allocates a command buffer, a dispatchable object, and layer
    // rule 5 says every one of those is registered or dispatch on it crashes.
    bool init(VkDevice device, VkPhysicalDevice physical_device, VkQueue queue,
              uint32_t queue_family, FunctionResolver resolver, void* resolver_data,
              PFN_vkSetDeviceLoaderData set_loader_data);

    // Returns a texture ready for ImGui, or 0 if it is not loaded yet. A miss
    // queues the file for the next post-present pass.
    ImTextureID get(uint64_t user_id, const char* avatar_hash);

    // Called after the present returns. Uploads at most one pending avatar.
    void process_pending();

    void shutdown();

    bool ready() const { return ready_; }

private:
    struct Texture {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDescriptorSet descriptor = VK_NULL_HANDLE;
    };

    bool upload(const AvatarKey& key, const std::string& path);
    uint32_t find_memory_type(uint32_t type_bits, VkMemoryPropertyFlags properties) const;
    void destroy(Texture& texture);

    struct {
        PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties = nullptr;
        PFN_vkCreateImage CreateImage = nullptr;
        PFN_vkDestroyImage DestroyImage = nullptr;
        PFN_vkCreateImageView CreateImageView = nullptr;
        PFN_vkDestroyImageView DestroyImageView = nullptr;
        PFN_vkCreateSampler CreateSampler = nullptr;
        PFN_vkDestroySampler DestroySampler = nullptr;
        PFN_vkAllocateMemory AllocateMemory = nullptr;
        PFN_vkFreeMemory FreeMemory = nullptr;
        PFN_vkBindImageMemory BindImageMemory = nullptr;
        PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
        PFN_vkCreateBuffer CreateBuffer = nullptr;
        PFN_vkDestroyBuffer DestroyBuffer = nullptr;
        PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
        PFN_vkBindBufferMemory BindBufferMemory = nullptr;
        PFN_vkMapMemory MapMemory = nullptr;
        PFN_vkUnmapMemory UnmapMemory = nullptr;
        PFN_vkCreateCommandPool CreateCommandPool = nullptr;
        PFN_vkDestroyCommandPool DestroyCommandPool = nullptr;
        PFN_vkAllocateCommandBuffers AllocateCommandBuffers = nullptr;
        PFN_vkFreeCommandBuffers FreeCommandBuffers = nullptr;
        PFN_vkBeginCommandBuffer BeginCommandBuffer = nullptr;
        PFN_vkEndCommandBuffer EndCommandBuffer = nullptr;
        PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
        PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage = nullptr;
        PFN_vkQueueSubmit QueueSubmit = nullptr;
        PFN_vkCreateFence CreateFence = nullptr;
        PFN_vkDestroyFence DestroyFence = nullptr;
        PFN_vkWaitForFences WaitForFences = nullptr;
    } fn_;

    VkDevice device_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    VkCommandPool pool_ = VK_NULL_HANDLE;
    VkSampler sampler_ = VK_NULL_HANDLE;
    PFN_vkSetDeviceLoaderData set_loader_data_ = nullptr;
    // Live descriptor sets this cache holds, against kMaxAvatarDescriptors.
    uint32_t descriptor_count_ = 0;
    bool ready_ = false;

    // Keyed by the fixed-size POD key, so the per-frame lookup in get() never
    // allocates -- the formatted string this used to be was a malloc and free
    // per visible face per frame (vocem/avatar_key.h says why).
    std::unordered_map<AvatarKey, Texture, AvatarKeyHash> textures_;
    // A file that has been asked for and is not on disk yet. The daemon is
    // probably still downloading it, so it is looked at again rather than written
    // off -- see vocem/avatar_file.h. The path std::string is built once when
    // the request is queued, never on the steady per-frame path.
    struct Pending {
        AvatarKey key;
        std::string path;
        AvatarWait wait;
    };

    std::vector<Pending> pending_;
};

}  // namespace vocem

#endif  // VOCEM_TEXTURE_CACHE_H

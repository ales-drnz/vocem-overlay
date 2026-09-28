// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// Avatar textures for the in-game panel, and the font atlas's texture. The
// daemon has already written raw RGBA files (vocem/avatar_rgba.h); this turns
// them into Vulkan images, post-present only (rule 10), at most one upload per
// call, and never waits for a fence: a later call frees what is done.

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
#include "vocem/fonts.h"
#include "vocem/shared_state.h"

namespace vocem {

// Resolves Vulkan entry points through the layer's dispatch chain.
using FunctionResolver = PFN_vkVoidFunction (*)(const char* name, void* user_data);

class TextureCache {
public:
    // Every live avatar texture holds one descriptor set from the backend's
    // pool, and AddTexture on an exhausted pool updates an uninitialised set
    // instead of failing, which the driver dies on (entry 46). So the pool is
    // sized from here and the cache stops at the budget: a face beyond it stays
    // the placeholder, logged.
    static constexpr uint32_t kMaxAvatarDescriptors = 48;
    // A full panel with headroom for toast authors and avatar changes: raising
    // kMaxUsers without room here would put placeholders on the panel's faces.
    static_assert(kMaxAvatarDescriptors >= 2 * vocem::kMaxUsers,
                  "the descriptor budget must cover a full panel with headroom");
    // The backend's pool: the budget, the font atlas's set, and headroom so
    // nothing lands exactly on the edge.
    static constexpr uint32_t kDescriptorPoolSets = kMaxAvatarDescriptors + 8;

    // set_loader_data is the loader's pfnSetDeviceLoaderData for this device:
    // every command buffer allocated here is registered through it (rule 5).
    bool init(VkDevice device, VkPhysicalDevice physical_device, VkQueue queue,
              uint32_t queue_family, FunctionResolver resolver, void* resolver_data,
              PFN_vkSetDeviceLoaderData set_loader_data);

    // Returns a texture ready for ImGui, or 0 if it is not loaded yet. A miss
    // queues the file for the next post-present pass.
    ImTextureID get(uint64_t user_id, const char* avatar_hash);

    // Called after the present returns. Uploads at most one pending avatar.
    void process_pending();

    // The font atlas's texture is owned here rather than by imgui_impl_vulkan:
    // the stock upload replaces the whole 64 MB image between two
    // vkQueueWaitIdle, while a new colour emoji changes a 32x32 square. The
    // backend cannot update part of its image, so the renderer never calls its
    // NewFrame and hands ImGui this texture through SetTexID.
    //
    // upload_font_atlas: the whole atlas into a new image, after a real build.
    // The new image and staging buffer are made FIRST, and only then is the old
    // one retired after a queue idle (frames in flight may sample it), so a
    // failed replacement leaves ImGui's TexID alive. The copy itself is not
    // waited for. Returns the descriptor for SetTexID, or 0. Post-present only.
    ImTextureID upload_font_atlas(const unsigned char* rgba, uint32_t width, uint32_t height);
    // update_font_atlas: only the squares a fold wrote, into the live image,
    // with no CPU wait: a barrier on the same queue orders the copy after the
    // frames that sample it, queue order before the next. Up to
    // kFontCopiesInFlight copies in flight; the oldest is waited for only when
    // all are still copying. Each square's pixels are the region's own
    // (vocem/fonts.h); the atlas's size only has to match the image's. False
    // when the image does not match the atlas -- the caller then uploads it
    // whole.
    bool update_font_atlas(uint32_t atlas_width, uint32_t atlas_height,
                           const AtlasRegion* regions, uint32_t count);

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
    bool create_image(uint32_t width, uint32_t height, Texture* texture);
    bool create_staging(VkDeviceSize size, VkBuffer* buffer, VkDeviceMemory* memory,
                        void** mapped);
    // A copy into the font image still on the GPU: its fence, its command
    // buffer and its staging buffer, freed together once the fence has
    // signalled.
    struct FontCopy {
        VkFence fence = VK_NULL_HANDLE;
        VkCommandBuffer command = VK_NULL_HANDLE;
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory staging_memory = VK_NULL_HANDLE;
        uint64_t order = 0;
    };
    void destroy_font();
    // Frees every font copy whose fence has signalled, or waits for all of
    // them when `wait` is set (the image is about to go).
    void retire_font_copies(bool wait);
    void free_font_copy(FontCopy& copy);
    // An empty slot for the next copy: the ones already done are freed first,
    // and only with every slot still copying is the oldest waited for.
    FontCopy& take_font_copy();
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
        PFN_vkGetFenceStatus GetFenceStatus = nullptr;
        PFN_vkQueueWaitIdle QueueWaitIdle = nullptr;
    } fn_;

    // The font atlas's texture, outside textures_ and the avatar budget
    // (kDescriptorPoolSets counts its set).
    Texture font_;
    uint32_t font_width_ = 0;
    uint32_t font_height_ = 0;
    // The copies into it still in flight. More than one, because a single slot
    // made a fold right after another upload wait on the game's thread for
    // everything it had submitted (tests/texture_font_copies.cpp).
    static constexpr uint32_t kFontCopiesInFlight = 4;
    FontCopy font_copies_[kFontCopiesInFlight];
    uint64_t font_copy_order_ = 0;

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
    // allocates (vocem/avatar_key.h).
    std::unordered_map<AvatarKey, Texture, AvatarKeyHash> textures_;
    // A file asked for and not on disk yet: the daemon is probably still
    // downloading it, so it is looked at again (vocem/avatar_file.h). The path
    // is built once when queued, never on the per-frame path.
    struct Pending {
        AvatarKey key;
        std::string path;
        AvatarWait wait;
    };

    std::vector<Pending> pending_;

    // The one face whose copy is on the GPU and not yet known finished:
    // submitted and left, and handed to ImGui by process_pending()
    // once its fence has signalled, so a face appears a frame or two later and
    // the game never waits. One at a time, the one-face-per-call budget.
    struct InFlight {
        bool active = false;
        AvatarKey key;
        std::string path;
        Texture texture;
        VkFence fence = VK_NULL_HANDLE;
        VkBuffer staging = VK_NULL_HANDLE;
        VkDeviceMemory staging_memory = VK_NULL_HANDLE;
        VkCommandBuffer command = VK_NULL_HANDLE;
    };
    InFlight in_flight_;
    // Frees the in-flight face's temporaries once its fence has signalled (or
    // at once when `wait` is set), and hands the face to ImGui. False while
    // the copy is still running.
    bool finish_in_flight(bool wait);
};

}  // namespace vocem

#endif  // VOCEM_TEXTURE_CACHE_H

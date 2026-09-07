// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.

#include "texture_cache.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "imgui_impl_vulkan.h"
#include "vocem/avatar_file.h"
#include "vocem/avatar_rgba.h"
#include "vocem/journal.h"
#include "vocem/overlay_log.h"
#include "vocem/shared_state.h"

// No image parser in here, on purpose. The cache is raw RGBA at one fixed size
// (vocem/avatar_rgba.h); the daemon is the only process that ever decodes a PNG.
// This file carried stb_image for four packages, parsing internet-supplied bytes
// inside every Vulkan game the layer drew in.

// The one logger both paths share (vocem/overlay_log.h), under this file's tag.
#define VOCEM_TLOG(...) VOCEM_OVERLAY_LOG("vocem/texture", __VA_ARGS__)

namespace vocem {

bool TextureCache::init(VkDevice device, VkPhysicalDevice physical_device, VkQueue queue,
                        uint32_t queue_family, FunctionResolver resolver, void* resolver_data,
                        PFN_vkSetDeviceLoaderData set_loader_data) {
    if (ready_) {
        return true;
    }
    if (!device || !queue || !resolver) {
        return false;
    }

    device_ = device;
    physical_device_ = physical_device;
    queue_ = queue;
    set_loader_data_ = set_loader_data;

#define VOCEM_RESOLVE(name)                                                      \
    fn_.name = reinterpret_cast<PFN_vk##name>(resolver("vk" #name, resolver_data)); \
    if (!fn_.name) return false

    VOCEM_RESOLVE(GetPhysicalDeviceMemoryProperties);
    VOCEM_RESOLVE(CreateImage);
    VOCEM_RESOLVE(DestroyImage);
    VOCEM_RESOLVE(CreateImageView);
    VOCEM_RESOLVE(DestroyImageView);
    VOCEM_RESOLVE(CreateSampler);
    VOCEM_RESOLVE(DestroySampler);
    VOCEM_RESOLVE(AllocateMemory);
    VOCEM_RESOLVE(FreeMemory);
    VOCEM_RESOLVE(BindImageMemory);
    VOCEM_RESOLVE(GetImageMemoryRequirements);
    VOCEM_RESOLVE(CreateBuffer);
    VOCEM_RESOLVE(DestroyBuffer);
    VOCEM_RESOLVE(GetBufferMemoryRequirements);
    VOCEM_RESOLVE(BindBufferMemory);
    VOCEM_RESOLVE(MapMemory);
    VOCEM_RESOLVE(UnmapMemory);
    VOCEM_RESOLVE(CreateCommandPool);
    VOCEM_RESOLVE(DestroyCommandPool);
    VOCEM_RESOLVE(AllocateCommandBuffers);
    VOCEM_RESOLVE(FreeCommandBuffers);
    VOCEM_RESOLVE(BeginCommandBuffer);
    VOCEM_RESOLVE(EndCommandBuffer);
    VOCEM_RESOLVE(CmdPipelineBarrier);
    VOCEM_RESOLVE(CmdCopyBufferToImage);
    VOCEM_RESOLVE(QueueSubmit);
    VOCEM_RESOLVE(CreateFence);
    VOCEM_RESOLVE(DestroyFence);
    VOCEM_RESOLVE(WaitForFences);

#undef VOCEM_RESOLVE

    VkCommandPoolCreateInfo pool_info{};
    pool_info.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pool_info.flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT;
    pool_info.queueFamilyIndex = queue_family;
    if (fn_.CreateCommandPool(device_, &pool_info, nullptr, &pool_) != VK_SUCCESS) {
        return false;
    }

    VkSamplerCreateInfo sampler_info{};
    sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sampler_info.magFilter = VK_FILTER_LINEAR;
    sampler_info.minFilter = VK_FILTER_LINEAR;
    sampler_info.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sampler_info.minLod = -1000.0f;
    sampler_info.maxLod = 1000.0f;
    sampler_info.maxAnisotropy = 1.0f;
    if (fn_.CreateSampler(device_, &sampler_info, nullptr, &sampler_) != VK_SUCCESS) {
        fn_.DestroyCommandPool(device_, pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
        return false;
    }

    ready_ = true;
    return true;
}

uint32_t TextureCache::find_memory_type(uint32_t type_bits,
                                        VkMemoryPropertyFlags properties) const {
    VkPhysicalDeviceMemoryProperties memory{};
    fn_.GetPhysicalDeviceMemoryProperties(physical_device_, &memory);
    for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
        if ((type_bits & (1u << i)) &&
            (memory.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    return UINT32_MAX;
}

ImTextureID TextureCache::get(uint64_t user_id, const char* avatar_hash) {
    if (!ready_) {
        return 0;
    }
    // A POD key on the stack: the formatted std::string this used to be was a
    // malloc and free per visible face, per frame (vocem/avatar_key.h).
    const AvatarKey key = AvatarKey::make(user_id, avatar_hash);

    auto it = textures_.find(key);
    if (it != textures_.end()) {
        return reinterpret_cast<ImTextureID>(it->second.descriptor);
    }

    for (const auto& entry : pending_) {
        if (entry.key == key) {
            return 0;  // already queued
        }
    }

    char path[768];
    avatar_rgba_path(path, sizeof(path), user_id, avatar_hash);
    Pending request;
    request.key = key;
    request.path = path;
    request.wait = AvatarWait::start(avatar_now_seconds());
    pending_.push_back(request);
    return 0;
}

void TextureCache::process_pending() {
    if (!ready_ || pending_.empty()) {
        return;
    }

    // The first request that is due. One per call: the upload blocks on a fence,
    // and a frame's worth of stalls for a whole channel would be visible.
    const double now = avatar_now_seconds();
    size_t index = pending_.size();
    for (size_t i = 0; i < pending_.size(); ++i) {
        if (pending_[i].wait.due(now)) {
            index = i;
            break;
        }
    }
    if (index == pending_.size()) {
        return;  // everything queued is waiting for its next look
    }

    const Pending request = pending_[index];
    pending_.erase(pending_.begin() + static_cast<long>(index));

    // The budget, before anything touches the pool. AddTexture on an exhausted
    // pool does not fail: it updates an uninitialised descriptor set and the
    // driver dereferences it -- the Minecraft crash of entry 46, five times in
    // four minutes, the eighth face of a call. A face beyond the budget is the
    // grey placeholder, said loudly, which is a defect somebody can report
    // rather than a game that dies.
    if (descriptor_count_ >= kMaxAvatarDescriptors) {
        VOCEM_TLOG("descriptor budget exhausted (%u live): %s stays the placeholder",
                   descriptor_count_, request.path.c_str());
        journal_note("avatar descriptor budget exhausted; drawing placeholders");
        textures_.emplace(request.key, Texture{});
        return;
    }

    // Not there yet is not the same as broken. Somebody who joins the channel is
    // drawn on the next frame, while the daemon is still downloading their
    // picture; giving up then is what left them a grey disc for the rest of the
    // session. The policy is vocem/avatar_file.h's, one spelling with the GL
    // provider.
    if (!avatar_file_exists(request.path.c_str())) {
        Pending again = request;
        if (again.wait.missed(now)) {
            pending_.push_back(again);
        } else {
            VOCEM_TLOG("gave up waiting for %s", request.path.c_str());
            textures_.emplace(request.key, Texture{});
        }
        return;
    }

    // Noted before the work and after the exists check: entry 46's crash was
    // an avatar upload and nothing said so -- but the note used to fire before
    // the check above too, and a journal that says "uploading" about a file
    // that was not there yet is a journal telling a small lie.
    {
        char note[840];
        std::snprintf(note, sizeof(note), "uploading avatar %s", request.path.c_str());
        journal_note(note);
    }

    if (upload(request.key, request.path)) {
        VOCEM_TLOG("uploaded %s", request.path.c_str());
    } else {
        VOCEM_TLOG("failed to load %s", request.path.c_str());
        // The file is there and will not decode, which asking again cannot fix:
        // the daemon renames into place, so what exists is complete.
        textures_.emplace(request.key, Texture{});
    }
}

bool TextureCache::upload(const AvatarKey& key, const std::string& path) {
    // Not a decode: a size check and a copy. Anything that is not exactly the
    // format's one size is refused.
    static unsigned char pixels[kAvatarRgbaBytes];
    if (!avatar_rgba_load(path.c_str(), pixels)) {
        return false;
    }
    constexpr uint32_t width = kAvatarPixels;
    constexpr uint32_t height = kAvatarPixels;

    const VkDeviceSize size = static_cast<VkDeviceSize>(kAvatarRgbaBytes);
    Texture texture;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool success = false;

    // Single exit path: everything temporary is released whatever happens.
    do {
        VkImageCreateInfo image_info{};
        image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image_info.imageType = VK_IMAGE_TYPE_2D;
        image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        image_info.extent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
        image_info.mipLevels = 1;
        image_info.arrayLayers = 1;
        image_info.samples = VK_SAMPLE_COUNT_1_BIT;
        image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
        image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        if (fn_.CreateImage(device_, &image_info, nullptr, &texture.image) != VK_SUCCESS) {
            break;
        }

        VkMemoryRequirements image_requirements{};
        fn_.GetImageMemoryRequirements(device_, texture.image, &image_requirements);
        VkMemoryAllocateInfo image_allocation{};
        image_allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        image_allocation.allocationSize = image_requirements.size;
        image_allocation.memoryTypeIndex = find_memory_type(
            image_requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (image_allocation.memoryTypeIndex == UINT32_MAX ||
            fn_.AllocateMemory(device_, &image_allocation, nullptr, &texture.memory) != VK_SUCCESS ||
            fn_.BindImageMemory(device_, texture.image, texture.memory, 0) != VK_SUCCESS) {
            break;
        }

        VkImageViewCreateInfo view_info{};
        view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        view_info.image = texture.image;
        view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
        view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        view_info.subresourceRange.levelCount = 1;
        view_info.subresourceRange.layerCount = 1;
        if (fn_.CreateImageView(device_, &view_info, nullptr, &texture.view) != VK_SUCCESS) {
            break;
        }

        VkBufferCreateInfo buffer_info{};
        buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        buffer_info.size = size;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        if (fn_.CreateBuffer(device_, &buffer_info, nullptr, &staging) != VK_SUCCESS) {
            break;
        }

        VkMemoryRequirements buffer_requirements{};
        fn_.GetBufferMemoryRequirements(device_, staging, &buffer_requirements);
        VkMemoryAllocateInfo buffer_allocation{};
        buffer_allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        buffer_allocation.allocationSize = buffer_requirements.size;
        buffer_allocation.memoryTypeIndex =
            find_memory_type(buffer_requirements.memoryTypeBits,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        if (buffer_allocation.memoryTypeIndex == UINT32_MAX ||
            fn_.AllocateMemory(device_, &buffer_allocation, nullptr, &staging_memory) != VK_SUCCESS ||
            fn_.BindBufferMemory(device_, staging, staging_memory, 0) != VK_SUCCESS) {
            break;
        }

        void* mapped = nullptr;
        if (fn_.MapMemory(device_, staging_memory, 0, size, 0, &mapped) != VK_SUCCESS) {
            break;
        }
        std::memcpy(mapped, pixels, static_cast<size_t>(size));
        fn_.UnmapMemory(device_, staging_memory);

        VkCommandBufferAllocateInfo command_info{};
        command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_info.commandPool = pool_;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = 1;
        if (fn_.AllocateCommandBuffers(device_, &command_info, &command_buffer) != VK_SUCCESS) {
            break;
        }
        // A dispatchable object this layer created: registered with the loader
        // before anything dispatches on it, or a layer below ours keying its
        // bookkeeping on the handle's dispatch pointer misses (rule 4). This
        // comment used to say the renderer had done it, and nothing had.
        if (set_loader_data_) {
            set_loader_data_(device_, command_buffer);
        }

        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (fn_.BeginCommandBuffer(command_buffer, &begin) != VK_SUCCESS) {
            break;
        }

        VkImageMemoryBarrier to_transfer{};
        to_transfer.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        to_transfer.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        to_transfer.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_transfer.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        to_transfer.image = texture.image;
        to_transfer.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        to_transfer.subresourceRange.levelCount = 1;
        to_transfer.subresourceRange.layerCount = 1;
        to_transfer.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        fn_.CmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                               &to_transfer);

        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
        fn_.CmdCopyBufferToImage(command_buffer, staging, texture.image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

        VkImageMemoryBarrier to_shader = to_transfer;
        to_shader.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_shader.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        to_shader.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_shader.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        fn_.CmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                               &to_shader);

        if (fn_.EndCommandBuffer(command_buffer) != VK_SUCCESS) {
            break;
        }

        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (fn_.CreateFence(device_, &fence_info, nullptr, &fence) != VK_SUCCESS) {
            break;
        }

        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &command_buffer;
        if (fn_.QueueSubmit(queue_, 1, &submit, fence) != VK_SUCCESS) {
            break;
        }
        // Safe to block: process_pending() only runs after the present returned.
        if (fn_.WaitForFences(device_, 1, &fence, VK_TRUE, UINT64_MAX) != VK_SUCCESS) {
            break;
        }

        texture.descriptor = ImGui_ImplVulkan_AddTexture(sampler_, texture.view,
                                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (texture.descriptor == VK_NULL_HANDLE) {
            break;
        }
        ++descriptor_count_;

        textures_.emplace(key, texture);
        success = true;
    } while (false);

    if (fence != VK_NULL_HANDLE) {
        fn_.DestroyFence(device_, fence, nullptr);
    }
    if (command_buffer != VK_NULL_HANDLE) {
        fn_.FreeCommandBuffers(device_, pool_, 1, &command_buffer);
    }
    if (staging != VK_NULL_HANDLE) {
        fn_.DestroyBuffer(device_, staging, nullptr);
    }
    if (staging_memory != VK_NULL_HANDLE) {
        fn_.FreeMemory(device_, staging_memory, nullptr);
    }

    if (!success) {
        destroy(texture);
    }
    return success;
}

void TextureCache::destroy(Texture& texture) {
    if (texture.descriptor != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(texture.descriptor);
        texture.descriptor = VK_NULL_HANDLE;
        if (descriptor_count_ > 0) {
            --descriptor_count_;
        }
    }
    if (texture.view != VK_NULL_HANDLE) {
        fn_.DestroyImageView(device_, texture.view, nullptr);
        texture.view = VK_NULL_HANDLE;
    }
    if (texture.image != VK_NULL_HANDLE) {
        fn_.DestroyImage(device_, texture.image, nullptr);
        texture.image = VK_NULL_HANDLE;
    }
    if (texture.memory != VK_NULL_HANDLE) {
        fn_.FreeMemory(device_, texture.memory, nullptr);
        texture.memory = VK_NULL_HANDLE;
    }
}

void TextureCache::shutdown() {
    if (!ready_) {
        return;
    }
    for (auto& entry : textures_) {
        destroy(entry.second);
    }
    textures_.clear();
    pending_.clear();

    if (sampler_ != VK_NULL_HANDLE) {
        fn_.DestroySampler(device_, sampler_, nullptr);
        sampler_ = VK_NULL_HANDLE;
    }
    if (pool_ != VK_NULL_HANDLE) {
        fn_.DestroyCommandPool(device_, pool_, nullptr);
        pool_ = VK_NULL_HANDLE;
    }
    device_ = VK_NULL_HANDLE;
    ready_ = false;
}

}  // namespace vocem

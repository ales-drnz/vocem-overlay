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

// No image parser in here, on purpose: the cache is raw RGBA at one fixed size
// (vocem/avatar_rgba.h), and the daemon is the only process that decodes a PNG.

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
    VOCEM_RESOLVE(GetFenceStatus);
    VOCEM_RESOLVE(QueueWaitIdle);

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
    // A POD key on the stack: no allocation per visible face per frame.
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
    if (in_flight_.active && in_flight_.key == key) {
        return 0;  // on its way to the GPU
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
    if (!ready_) {
        return;
    }
    // The font texture's copies, whole or in part, freed once they are done;
    // never waited for here.
    retire_font_copies(false);
    // The face already on its way first: until its copy is done nothing else
    // starts, which keeps the one-face-per-call budget.
    if (in_flight_.active && !finish_in_flight(false)) {
        return;
    }
    if (pending_.empty()) {
        return;
    }

    // The first request that is due. One per call: a channel's worth of
    // uploads in one frame would be visible even without a wait.
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

    // The budget, before anything touches the pool (kMaxAvatarDescriptors, entry
    // 46): a face beyond it is the placeholder, said loudly, not a dead game.
    if (descriptor_count_ >= kMaxAvatarDescriptors) {
        VOCEM_TLOG("descriptor budget exhausted (%u live): %s stays the placeholder",
                   descriptor_count_, request.path.c_str());
        journal_note("avatar descriptor budget exhausted; drawing placeholders");
        textures_.emplace(request.key, Texture{});
        return;
    }

    // Not there yet is not broken: somebody who just joined is drawn while the
    // daemon still downloads their picture. The retry policy is
    // vocem/avatar_file.h's, shared with the GL provider.
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

    // Noted before the work and after the exists check, so the journal names
    // an avatar upload that crashes and never claims a file that was not there.
    {
        char note[840];
        std::snprintf(note, sizeof(note), "uploading avatar %s", request.path.c_str());
        journal_note(note);
    }

    if (upload(request.key, request.path)) {
        // Submitted; "uploaded" is said by finish_in_flight when it is done.
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

    // Single failure path: a submitted upload returns from inside the loop, and
    // everything temporary is released below whatever else happened.
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
        // before anything dispatches on it, or a layer below keying its
        // bookkeeping on the dispatch pointer misses it (rule 5).
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
        // Not waited for (see InFlight): the temporaries and the texture go to
        // in_flight_, and finish_in_flight hands the face over once the fence
        // has signalled.
        in_flight_.active = true;
        in_flight_.key = key;
        in_flight_.path = path;
        in_flight_.texture = texture;
        in_flight_.fence = fence;
        in_flight_.staging = staging;
        in_flight_.staging_memory = staging_memory;
        in_flight_.command = command_buffer;
        return true;
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
    destroy(texture);
    return false;
}

bool TextureCache::create_image(uint32_t width, uint32_t height, Texture* texture) {
    VkImageCreateInfo image_info{};
    image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {width, height, 1};
    image_info.mipLevels = 1;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    image_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (fn_.CreateImage(device_, &image_info, nullptr, &texture->image) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements requirements{};
    fn_.GetImageMemoryRequirements(device_, texture->image, &requirements);
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex =
        find_memory_type(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (allocation.memoryTypeIndex == UINT32_MAX ||
        fn_.AllocateMemory(device_, &allocation, nullptr, &texture->memory) != VK_SUCCESS ||
        fn_.BindImageMemory(device_, texture->image, texture->memory, 0) != VK_SUCCESS) {
        return false;
    }
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = texture->image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;
    return fn_.CreateImageView(device_, &view_info, nullptr, &texture->view) == VK_SUCCESS;
}

bool TextureCache::create_staging(VkDeviceSize size, VkBuffer* buffer, VkDeviceMemory* memory,
                                  void** mapped) {
    VkBufferCreateInfo buffer_info{};
    buffer_info.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (fn_.CreateBuffer(device_, &buffer_info, nullptr, buffer) != VK_SUCCESS) {
        return false;
    }
    VkMemoryRequirements requirements{};
    fn_.GetBufferMemoryRequirements(device_, *buffer, &requirements);
    VkMemoryAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocation.allocationSize = requirements.size;
    allocation.memoryTypeIndex =
        find_memory_type(requirements.memoryTypeBits,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (allocation.memoryTypeIndex == UINT32_MAX ||
        fn_.AllocateMemory(device_, &allocation, nullptr, memory) != VK_SUCCESS ||
        fn_.BindBufferMemory(device_, *buffer, *memory, 0) != VK_SUCCESS) {
        return false;
    }
    return fn_.MapMemory(device_, *memory, 0, size, 0, mapped) == VK_SUCCESS;
}

namespace {

VkImageMemoryBarrier font_barrier(VkImage image, VkImageLayout from, VkImageLayout to,
                                  VkAccessFlags src, VkAccessFlags dst) {
    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = from;
    barrier.newLayout = to;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = src;
    barrier.dstAccessMask = dst;
    return barrier;
}

}  // namespace

ImTextureID TextureCache::upload_font_atlas(const unsigned char* rgba, uint32_t width,
                                            uint32_t height) {
    if (!rgba) {
        return 0;
    }
    return upload_font_atlas(
        width, height,
        [](unsigned char* destination, uint32_t w, uint32_t h, const void* source) {
            std::memcpy(destination, source, static_cast<size_t>(w) * h * 4);
            return true;
        },
        rgba);
}

ImTextureID TextureCache::upload_font_atlas(uint32_t width, uint32_t height, FillPixels fill,
                                            const void* context) {
    if (!ready_ || !fill || width == 0 || height == 0) {
        return 0;
    }
    const VkDeviceSize size = static_cast<VkDeviceSize>(width) * height * 4;
    Texture texture;
    VkBuffer staging = VK_NULL_HANDLE;
    VkDeviceMemory staging_memory = VK_NULL_HANDLE;
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkFence fence = VK_NULL_HANDLE;
    bool success = false;
    do {
        void* mapped = nullptr;
        if (!create_image(width, height, &texture) ||
            !create_staging(size, &staging, &staging_memory, &mapped)) {
            break;
        }
        const bool filled = fill(static_cast<unsigned char*>(mapped), width, height, context);
        fn_.UnmapMemory(device_, staging_memory);
        if (!filled) {
            break;
        }

        // Only now that its replacement exists is the old image retired. The
        // frames in flight may still be sampling it: the same wait the stock
        // upload makes, on the same queue, and only on this path -- a real
        // build, which is rare. Its copies go with it.
        if (font_.image != VK_NULL_HANDLE) {
            fn_.QueueWaitIdle(queue_);
        }
        retire_font_copies(true);
        destroy_font();

        VkCommandBufferAllocateInfo command_info{};
        command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_info.commandPool = pool_;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = 1;
        if (fn_.AllocateCommandBuffers(device_, &command_info, &command_buffer) != VK_SUCCESS) {
            break;
        }
        // Rule 5, as for an avatar's command buffer.
        if (set_loader_data_) {
            set_loader_data_(device_, command_buffer);
        }
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (fn_.BeginCommandBuffer(command_buffer, &begin) != VK_SUCCESS) {
            break;
        }
        const VkImageMemoryBarrier to_transfer =
            font_barrier(texture.image, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT);
        fn_.CmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                               &to_transfer);
        VkBufferImageCopy region{};
        region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {width, height, 1};
        fn_.CmdCopyBufferToImage(command_buffer, staging, texture.image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        const VkImageMemoryBarrier to_shader = font_barrier(
            texture.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT);
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
        // Not waited for (entry 192): the first draw that samples this image is
        // submitted later to the same queue and ordered after the copy by the
        // barrier above; the descriptor does not depend on the pixels. The
        // staging buffer is freed once the fence says the copy is done.
        FontCopy& copy = take_font_copy();
        copy.fence = fence;
        copy.command = command_buffer;
        copy.staging = staging;
        copy.staging_memory = staging_memory;
        copy.order = ++font_copy_order_;
        fence = VK_NULL_HANDLE;
        command_buffer = VK_NULL_HANDLE;
        staging = VK_NULL_HANDLE;
        staging_memory = VK_NULL_HANDLE;
        texture.descriptor = ImGui_ImplVulkan_AddTexture(sampler_, texture.view,
                                                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        success = texture.descriptor != VK_NULL_HANDLE;
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
        if (texture.image == VK_NULL_HANDLE || font_.image != VK_NULL_HANDLE) {
            // Failed before the old image was retired: it is still the live
            // one, and so is the descriptor ImGui holds. Only the new
            // resources go.
            destroy(texture);
            return 0;
        }
        // Failed after: a copy may be in flight into the image about to go.
        retire_font_copies(true);
        font_ = texture;
        destroy_font();
        return 0;
    }
    font_ = texture;
    font_width_ = width;
    font_height_ = height;
    return reinterpret_cast<ImTextureID>(font_.descriptor);
}

bool TextureCache::update_font_atlas(uint32_t atlas_width, uint32_t atlas_height,
                                     const AtlasRegion* regions, uint32_t count) {
    if (!ready_ || font_.image == VK_NULL_HANDLE || atlas_width != font_width_ ||
        atlas_height != font_height_ || count > kMaxFoldedRegions) {
        return false;
    }
    if (count == 0) {
        return true;
    }
    VkDeviceSize size = 0;
    for (uint32_t i = 0; i < count; ++i) {
        const AtlasRegion& r = regions[i];
        if (!r.pixels || r.width <= 0 || r.height <= 0 || r.x < 0 || r.y < 0 ||
            static_cast<uint32_t>(r.x + r.width) > atlas_width ||
            static_cast<uint32_t>(r.y + r.height) > atlas_height) {
            return false;
        }
        size += static_cast<VkDeviceSize>(r.width) * r.height * 4;
    }
    // A slot of its own: the copies before it may still be on the GPU, and
    // nothing here waits for them.
    FontCopy& slot = take_font_copy();

    VkBufferImageCopy copies[kMaxFoldedRegions];
    bool success = false;
    do {
        void* mapped = nullptr;
        if (!create_staging(size, &slot.staging, &slot.staging_memory, &mapped)) {
            break;
        }
        VkDeviceSize offset = 0;
        for (uint32_t i = 0; i < count; ++i) {
            const AtlasRegion& r = regions[i];
            unsigned char* out = static_cast<unsigned char*>(mapped) + offset;
            std::memcpy(out, r.pixels, static_cast<size_t>(r.width) * r.height * 4);
            VkBufferImageCopy& copy = copies[i];
            copy = VkBufferImageCopy{};
            copy.bufferOffset = offset;
            copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copy.imageSubresource.layerCount = 1;
            copy.imageOffset = {r.x, r.y, 0};
            copy.imageExtent = {static_cast<uint32_t>(r.width), static_cast<uint32_t>(r.height), 1};
            offset += static_cast<VkDeviceSize>(r.width) * r.height * 4;
        }
        fn_.UnmapMemory(device_, slot.staging_memory);

        VkCommandBufferAllocateInfo command_info{};
        command_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        command_info.commandPool = pool_;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = 1;
        if (fn_.AllocateCommandBuffers(device_, &command_info, &slot.command) != VK_SUCCESS) {
            break;
        }
        if (set_loader_data_) {
            set_loader_data_(device_, slot.command);
        }
        VkCommandBufferBeginInfo begin{};
        begin.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (fn_.BeginCommandBuffer(slot.command, &begin) != VK_SUCCESS) {
            break;
        }
        // The image is live: frames already submitted to this queue sample it.
        // A barrier's first scope is every command submitted before it on the
        // same queue, so the copy waits for those reads on the GPU and the
        // CPU waits for nothing. The whole image changes layout, and the rest
        // of its contents survive a SHADER_READ_ONLY -> TRANSFER_DST transition.
        const VkImageMemoryBarrier to_transfer = font_barrier(
            font_.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
            VK_ACCESS_TRANSFER_WRITE_BIT);
        fn_.CmdPipelineBarrier(slot.command, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                               &to_transfer);
        fn_.CmdCopyBufferToImage(slot.command, slot.staging, font_.image,
                                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, count, copies);
        const VkImageMemoryBarrier to_shader = font_barrier(
            font_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_SHADER_READ_BIT);
        fn_.CmdPipelineBarrier(slot.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                               &to_shader);
        if (fn_.EndCommandBuffer(slot.command) != VK_SUCCESS) {
            break;
        }
        VkFenceCreateInfo fence_info{};
        fence_info.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        if (fn_.CreateFence(device_, &fence_info, nullptr, &slot.fence) != VK_SUCCESS) {
            break;
        }
        VkSubmitInfo submit{};
        submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        submit.commandBufferCount = 1;
        submit.pCommandBuffers = &slot.command;
        if (fn_.QueueSubmit(queue_, 1, &submit, slot.fence) != VK_SUCCESS) {
            break;
        }
        success = true;
    } while (false);

    if (!success) {
        // Nothing was submitted, or the submit failed: nothing the GPU holds.
        if (slot.fence != VK_NULL_HANDLE) {
            fn_.DestroyFence(device_, slot.fence, nullptr);
            slot.fence = VK_NULL_HANDLE;
        }
        free_font_copy(slot);
        return false;
    }
    slot.order = ++font_copy_order_;
    return true;
}

void TextureCache::free_font_copy(FontCopy& copy) {
    if (copy.fence != VK_NULL_HANDLE) {
        fn_.DestroyFence(device_, copy.fence, nullptr);
    }
    if (copy.command != VK_NULL_HANDLE) {
        fn_.FreeCommandBuffers(device_, pool_, 1, &copy.command);
    }
    if (copy.staging != VK_NULL_HANDLE) {
        fn_.DestroyBuffer(device_, copy.staging, nullptr);
    }
    if (copy.staging_memory != VK_NULL_HANDLE) {
        fn_.FreeMemory(device_, copy.staging_memory, nullptr);
    }
    copy = FontCopy{};
}

void TextureCache::retire_font_copies(bool wait) {
    for (FontCopy& copy : font_copies_) {
        if (copy.fence != VK_NULL_HANDLE) {
            if (wait) {
                fn_.WaitForFences(device_, 1, &copy.fence, VK_TRUE, UINT64_MAX);
            } else if (fn_.GetFenceStatus(device_, copy.fence) != VK_SUCCESS) {
                continue;  // still copying; asked again next time
            }
        }
        free_font_copy(copy);
    }
}

TextureCache::FontCopy& TextureCache::take_font_copy() {
    retire_font_copies(false);
    FontCopy* oldest = &font_copies_[0];
    for (FontCopy& copy : font_copies_) {
        if (copy.fence == VK_NULL_HANDLE && copy.staging == VK_NULL_HANDLE &&
            copy.command == VK_NULL_HANDLE) {
            return copy;
        }
        if (copy.order < oldest->order) {
            oldest = &copy;
        }
    }
    // Every slot still copying: four folds inside the frames the GPU is
    // behind. The oldest is the one nearest done.
    fn_.WaitForFences(device_, 1, &oldest->fence, VK_TRUE, UINT64_MAX);
    free_font_copy(*oldest);
    return *oldest;
}

void TextureCache::destroy_font() {
    if (font_.descriptor != VK_NULL_HANDLE) {
        ImGui_ImplVulkan_RemoveTexture(font_.descriptor);
        font_.descriptor = VK_NULL_HANDLE;
    }
    if (font_.view != VK_NULL_HANDLE) {
        fn_.DestroyImageView(device_, font_.view, nullptr);
        font_.view = VK_NULL_HANDLE;
    }
    if (font_.image != VK_NULL_HANDLE) {
        fn_.DestroyImage(device_, font_.image, nullptr);
        font_.image = VK_NULL_HANDLE;
    }
    if (font_.memory != VK_NULL_HANDLE) {
        fn_.FreeMemory(device_, font_.memory, nullptr);
        font_.memory = VK_NULL_HANDLE;
    }
    font_width_ = 0;
    font_height_ = 0;
}

bool TextureCache::finish_in_flight(bool wait) {
    if (!in_flight_.active) {
        return true;
    }
    if (wait) {
        fn_.WaitForFences(device_, 1, &in_flight_.fence, VK_TRUE, UINT64_MAX);
    } else if (fn_.GetFenceStatus(device_, in_flight_.fence) != VK_SUCCESS) {
        return false;
    }
    fn_.DestroyFence(device_, in_flight_.fence, nullptr);
    fn_.FreeCommandBuffers(device_, pool_, 1, &in_flight_.command);
    fn_.DestroyBuffer(device_, in_flight_.staging, nullptr);
    fn_.FreeMemory(device_, in_flight_.staging_memory, nullptr);
    Texture texture = in_flight_.texture;
    const AvatarKey key = in_flight_.key;
    const std::string path = in_flight_.path;
    in_flight_ = InFlight{};
    if (wait) {
        destroy(texture);  // shutting down: nobody will draw it
        return true;
    }
    texture.descriptor = ImGui_ImplVulkan_AddTexture(sampler_, texture.view,
                                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    if (texture.descriptor == VK_NULL_HANDLE) {
        destroy(texture);
        return true;
    }
    ++descriptor_count_;
    textures_.emplace(key, texture);
    VOCEM_TLOG("uploaded %s", path.c_str());
    return true;
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
    // A face still copying goes first: its image and staging buffer are in
    // the GPU's hands until the fence says otherwise.
    finish_in_flight(true);
    for (auto& entry : textures_) {
        destroy(entry.second);
    }
    textures_.clear();
    pending_.clear();
    // The font texture too, and the copies into it.
    retire_font_copies(true);
    destroy_font();

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

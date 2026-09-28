// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The overlay's colours on swapchains that are not plain sRGB-numeric.
//
// ImGui's stock pipeline writes sRGB-encoded values, which an HDR10 swapchain
// reads as PQ (1.0 = 10000 nits) and an scRGB one as linear: an oversaturated,
// blinding panel. One fragment shader (hdr.frag) picks the encode by
// specialization constant, in a pipeline whose layout is defined identically
// to ImGui's own, so RenderDrawData binds it with the backend's descriptor
// sets and push constants: layout compatibility is by definition, not handle.

#ifndef VOCEM_HDR_PIPELINE_H
#define VOCEM_HDR_PIPELINE_H

#include <vulkan/vulkan.h>

namespace vocem {

// Whether a swapchain's format carries the sRGB encoding itself, so the
// hardware would encode ImGui's already-sRGB colours twice. Vulkan's _SRGB
// formats a swapchain can plausibly have; block-compressed ones are left out.
inline bool format_is_srgb(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8_SRGB:
        case VK_FORMAT_R8G8_SRGB:
        case VK_FORMAT_R8G8B8_SRGB:
        case VK_FORMAT_B8G8R8_SRGB:
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SRGB:
        case VK_FORMAT_A8B8G8R8_SRGB_PACK32:
            return true;
        default:
            return false;
    }
}

// Which conversion a swapchain needs. 0 = none, the stock look: every SDR
// space in a linear-numeric format and every space not implemented here (HLG
// among them, on purpose) -- unconverted colours degrade the look, a wrong
// formula lies about it. The colour space is asked first: a space implemented
// here never comes in an *_SRGB format, and it describes what the display does.
//
// Mode 3, an *_SRGB format, is the most common answer. It is fixed in the
// shader rather than by linearising style colours on the CPU, because avatars
// and colour emoji come from textures and never touch a style colour (entry 93).
inline int hdr_mode_for(VkColorSpaceKHR space, VkFormat format = VK_FORMAT_UNDEFINED) {
    switch (space) {
        case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
            return 1;  // scRGB: linear, 1.0 = 80 nits
        case VK_COLOR_SPACE_HDR10_ST2084_EXT:
            return 2;  // HDR10: BT.2020 primaries, PQ transfer
        default:
            return format_is_srgb(format) ? 3 : 0;
    }
}

// Where the overlay's white lands in an HDR range: reference SDR diffuse
// white (ITU-R BT.2408), overridable with VOCEM_HDR_NITS for displays whose
// compositor maps SDR content elsewhere.
float hdr_sdr_nits();

// The device entry points the pipeline needs, resolved by the layer through
// its own dispatch -- this module never looks anything up.
struct HdrDeviceFunctions {
    PFN_vkCreateShaderModule CreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout = nullptr;
    PFN_vkCreateGraphicsPipelines CreateGraphicsPipelines = nullptr;
    PFN_vkDestroyPipeline DestroyPipeline = nullptr;

    bool complete() const {
        return CreateShaderModule && DestroyShaderModule && CreateDescriptorSetLayout &&
               DestroyDescriptorSetLayout && CreatePipelineLayout && DestroyPipelineLayout &&
               CreateGraphicsPipelines && DestroyPipeline;
    }
};

struct HdrPipeline {
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptors = VK_NULL_HANDLE;
};

// Builds the pipeline for `mode` (0 to 3) against the layer's own render pass.
// Mode 0 too (the identity): a swapchain's own pipeline is the only one
// guaranteed compatible with its render pass, and ImGui's stock pipeline is
// built for the first swapchain's format only
// (VUID-vkCmdDrawIndexed-renderPass-02684 after a format change). Returns
// false, with everything released, on any failure; the caller then draws with
// the stock pipeline only where its format matches and passes the frame
// through otherwise (rule 7).
bool hdr_pipeline_create(const HdrDeviceFunctions& fn, VkDevice device, VkRenderPass render_pass,
                         int mode, float sdr_nits, HdrPipeline& out);

void hdr_pipeline_destroy(const HdrDeviceFunctions& fn, VkDevice device, HdrPipeline& pipeline);

}  // namespace vocem

#endif  // VOCEM_HDR_PIPELINE_H

// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.
//
// The overlay's colours on swapchains that are not sRGB.
//
// ImGui's stock pipeline writes sRGB-encoded values, which an HDR10 swapchain
// reads as PQ -- where 1.0 means ten thousand nits -- and an scRGB swapchain
// reads as linear. Both produce the oversaturated, blinding panel the open
// risk in DESIGN promised. The cure is one fragment shader with the encode
// chosen at pipeline creation (hdr.frag, specialization constants), in a
// pipeline whose layout is defined identically to ImGui's own, so
// ImGui_ImplVulkan_RenderDrawData can bind it with the backend's descriptor
// sets and push constants: Vulkan's pipeline layout compatibility is by
// definition, not by handle.

#ifndef VOCEM_HDR_PIPELINE_H
#define VOCEM_HDR_PIPELINE_H

#include <vulkan/vulkan.h>

namespace vocem {

// Which conversion a swapchain's colour space needs. 0 = none: draw with the
// stock pipeline, which is every SDR space and every space this file does not
// implement -- unconverted colours degrade the look, a wrong formula lies
// about it. HLG in particular is recognised and left at 0 on purpose.
inline int hdr_mode_for(VkColorSpaceKHR space) {
    switch (space) {
        case VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT:
            return 1;  // scRGB: linear, 1.0 = 80 nits
        case VK_COLOR_SPACE_HDR10_ST2084_EXT:
            return 2;  // HDR10: BT.2020 primaries, PQ transfer
        default:
            return 0;
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

// Builds the converting pipeline for `mode` (1 or 2) against the layer's own
// render pass. Returns false -- with everything released -- on any failure,
// in which case the caller draws with the stock pipeline: a game must never
// lose its overlay, let alone its stability, to a pipeline that would only
// have corrected its colours (rule 7).
bool hdr_pipeline_create(const HdrDeviceFunctions& fn, VkDevice device, VkRenderPass render_pass,
                         int mode, float sdr_nits, HdrPipeline& out);

void hdr_pipeline_destroy(const HdrDeviceFunctions& fn, VkDevice device, HdrPipeline& pipeline);

}  // namespace vocem

#endif  // VOCEM_HDR_PIPELINE_H

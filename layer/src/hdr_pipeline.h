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

// Whether a swapchain's *format* carries the sRGB encoding itself, in which
// case the hardware applies linear->sRGB to whatever the shader writes and
// ImGui's already-sRGB colours are encoded twice.
//
// The list is Vulkan's: every VK_FORMAT_* whose name ends in _SRGB that a
// swapchain can plausibly be created with. The block-compressed sRGB formats
// exist too and are not here -- an image no swapchain has ever been made of is
// noise in a list somebody has to keep right.
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

// Which conversion a swapchain needs. 0 = none: draw with the stock pipeline,
// which is every SDR space in a linear-numeric format and every space this file
// does not implement -- unconverted colours degrade the look, a wrong formula
// lies about it. HLG in particular is recognised and left at 0 on purpose.
//
// The colour space is asked first and the format second, because a colour space
// this file implements always comes in a format that carries no encoding of its
// own (an HDR10 swapchain is 10-bit or float, never *_SRGB): where both could
// speak, the space is the one that describes what the display will do.
//
// Mode 3 is not an HDR mode and it is by far the most common answer: `*_SRGB`
// is what a great many games ask their swapchain for, and until this was here
// they all got a washed-out panel. It was found by reading MangoHud, which
// answers the same question on the CPU by linearising its own style colours --
// a cure that works for a HUD of text and rectangles and would leave our
// avatars and colour emoji wrong, because those come out of a texture and never
// touch a style colour.
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

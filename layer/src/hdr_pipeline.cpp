// Copyright © 2026 & onwards, Alessandro Di Ronza <ales.drnz@gmail.com>.
// All rights reserved.
// Use of this source code is governed by BSD 3-Clause license that can be found in the LICENSE file.

#include "hdr_pipeline.h"

#include <cstdio>
#include <cstdlib>

#include "hdr_shaders.h"
#include "imgui.h"

namespace vocem {

float hdr_sdr_nits() {
    static const float nits = [] {
        if (const char* env = std::getenv("VOCEM_HDR_NITS"); env && env[0]) {
            const float value = static_cast<float>(std::atof(env));
            if (value >= 40.0f && value <= 1000.0f) {
                return value;
            }
        }
        return 203.0f;
    }();
    return nits;
}

bool hdr_pipeline_create(const HdrDeviceFunctions& fn, VkDevice device, VkRenderPass render_pass,
                         int mode, float sdr_nits, HdrPipeline& out) {
    if (!fn.complete() || mode == 0) {
        return false;
    }

    VkShaderModule vert = VK_NULL_HANDLE;
    VkShaderModule frag = VK_NULL_HANDLE;
    bool success = false;

    do {
        VkShaderModuleCreateInfo vert_info{};
        vert_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        vert_info.codeSize = sizeof(vocem_overlay_vert_spv);
        vert_info.pCode = vocem_overlay_vert_spv;
        if (fn.CreateShaderModule(device, &vert_info, nullptr, &vert) != VK_SUCCESS) {
            break;
        }
        VkShaderModuleCreateInfo frag_info{};
        frag_info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        frag_info.codeSize = sizeof(vocem_hdr_frag_spv);
        frag_info.pCode = vocem_hdr_frag_spv;
        if (fn.CreateShaderModule(device, &frag_info, nullptr, &frag) != VK_SUCCESS) {
            break;
        }

        // The descriptor set layout and push constant range, defined exactly
        // as ImGui's backend defines its own: compatibility is by definition,
        // and this is the definition (imgui_impl_vulkan.cpp,
        // CreateDeviceObjects).
        VkDescriptorSetLayoutBinding binding{};
        binding.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        binding.descriptorCount = 1;
        binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo set_info{};
        set_info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        set_info.bindingCount = 1;
        set_info.pBindings = &binding;
        if (fn.CreateDescriptorSetLayout(device, &set_info, nullptr, &out.descriptors) !=
            VK_SUCCESS) {
            break;
        }

        VkPushConstantRange push{};
        push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        push.offset = 0;
        push.size = sizeof(float) * 4;
        VkPipelineLayoutCreateInfo layout_info{};
        layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        layout_info.setLayoutCount = 1;
        layout_info.pSetLayouts = &out.descriptors;
        layout_info.pushConstantRangeCount = 1;
        layout_info.pPushConstantRanges = &push;
        if (fn.CreatePipelineLayout(device, &layout_info, nullptr, &out.layout) != VK_SUCCESS) {
            break;
        }

        // The encode, decided here and folded into the shader by the driver.
        struct Specialization {
            int32_t mode;
            float sdr_nits;
        } specialization{mode, sdr_nits};
        VkSpecializationMapEntry entries[2] = {};
        entries[0].constantID = 0;
        entries[0].offset = offsetof(Specialization, mode);
        entries[0].size = sizeof(int32_t);
        entries[1].constantID = 1;
        entries[1].offset = offsetof(Specialization, sdr_nits);
        entries[1].size = sizeof(float);
        VkSpecializationInfo spec_info{};
        spec_info.mapEntryCount = 2;
        spec_info.pMapEntries = entries;
        spec_info.dataSize = sizeof(specialization);
        spec_info.pData = &specialization;

        VkPipelineShaderStageCreateInfo stages[2] = {};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vert;
        stages[0].pName = "main";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = frag;
        stages[1].pName = "main";
        stages[1].pSpecializationInfo = &spec_info;

        // Every state below is ImGui's own pipeline, restated: the draw data
        // this pipeline consumes is recorded by RenderDrawData, and a state
        // that differs from the backend's is a way to draw it wrongly.
        VkVertexInputBindingDescription vertex_binding{};
        vertex_binding.stride = sizeof(ImDrawVert);
        vertex_binding.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        VkVertexInputAttributeDescription attributes[3] = {};
        attributes[0].location = 0;
        attributes[0].format = VK_FORMAT_R32G32_SFLOAT;
        attributes[0].offset = offsetof(ImDrawVert, pos);
        attributes[1].location = 1;
        attributes[1].format = VK_FORMAT_R32G32_SFLOAT;
        attributes[1].offset = offsetof(ImDrawVert, uv);
        attributes[2].location = 2;
        attributes[2].format = VK_FORMAT_R8G8B8A8_UNORM;
        attributes[2].offset = offsetof(ImDrawVert, col);
        VkPipelineVertexInputStateCreateInfo vertex_info{};
        vertex_info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
        vertex_info.vertexBindingDescriptionCount = 1;
        vertex_info.pVertexBindingDescriptions = &vertex_binding;
        vertex_info.vertexAttributeDescriptionCount = 3;
        vertex_info.pVertexAttributeDescriptions = attributes;

        VkPipelineInputAssemblyStateCreateInfo assembly{};
        assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

        VkPipelineViewportStateCreateInfo viewport{};
        viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewport.viewportCount = 1;
        viewport.scissorCount = 1;

        VkPipelineRasterizationStateCreateInfo raster{};
        raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        raster.polygonMode = VK_POLYGON_MODE_FILL;
        raster.cullMode = VK_CULL_MODE_NONE;
        raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        raster.lineWidth = 1.0f;

        VkPipelineMultisampleStateCreateInfo multisample{};
        multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState blend_attachment{};
        blend_attachment.blendEnable = VK_TRUE;
        blend_attachment.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        blend_attachment.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attachment.colorBlendOp = VK_BLEND_OP_ADD;
        blend_attachment.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        blend_attachment.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        blend_attachment.alphaBlendOp = VK_BLEND_OP_ADD;
        blend_attachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        VkPipelineColorBlendStateCreateInfo blend{};
        blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        blend.attachmentCount = 1;
        blend.pAttachments = &blend_attachment;

        VkPipelineDepthStencilStateCreateInfo depth{};
        depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;

        VkDynamicState dynamic_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{};
        dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dynamic_states;

        VkGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipeline_info.stageCount = 2;
        pipeline_info.pStages = stages;
        pipeline_info.pVertexInputState = &vertex_info;
        pipeline_info.pInputAssemblyState = &assembly;
        pipeline_info.pViewportState = &viewport;
        pipeline_info.pRasterizationState = &raster;
        pipeline_info.pMultisampleState = &multisample;
        pipeline_info.pDepthStencilState = &depth;
        pipeline_info.pColorBlendState = &blend;
        pipeline_info.pDynamicState = &dynamic;
        pipeline_info.layout = out.layout;
        pipeline_info.renderPass = render_pass;
        if (fn.CreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr,
                                       &out.pipeline) != VK_SUCCESS) {
            break;
        }
        success = true;
    } while (false);

    // Shader modules may go as soon as the pipeline exists; on failure,
    // everything goes.
    if (vert != VK_NULL_HANDLE) {
        fn.DestroyShaderModule(device, vert, nullptr);
    }
    if (frag != VK_NULL_HANDLE) {
        fn.DestroyShaderModule(device, frag, nullptr);
    }
    if (!success) {
        hdr_pipeline_destroy(fn, device, out);
    }
    return success;
}

void hdr_pipeline_destroy(const HdrDeviceFunctions& fn, VkDevice device, HdrPipeline& pipeline) {
    if (!fn.complete()) {
        return;
    }
    if (pipeline.pipeline != VK_NULL_HANDLE) {
        fn.DestroyPipeline(device, pipeline.pipeline, nullptr);
        pipeline.pipeline = VK_NULL_HANDLE;
    }
    if (pipeline.layout != VK_NULL_HANDLE) {
        fn.DestroyPipelineLayout(device, pipeline.layout, nullptr);
        pipeline.layout = VK_NULL_HANDLE;
    }
    if (pipeline.descriptors != VK_NULL_HANDLE) {
        fn.DestroyDescriptorSetLayout(device, pipeline.descriptors, nullptr);
        pipeline.descriptors = VK_NULL_HANDLE;
    }
}

}  // namespace vocem

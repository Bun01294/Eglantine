// SPDX-FileCopyrightText: Copyright 2026 Vortex727 (FUKKO)
// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdint>

#include "video_core/host_shaders/bc_encoder_comp_spv.h"
#include "video_core/renderer_vulkan/nvn/nvn_bc_encode.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"

namespace Vulkan::Nvn {
namespace {

/// The push constant block of bc_encoder.comp.
struct PushConstants {
    u32 blocks_x;
    u32 blocks_y;
    u32 width;
    u32 height;
    u32 bc3;
    u32 out_offset; ///< in 8-byte units
};
static_assert(sizeof(PushConstants) == 24);

constexpr u32 BINDING_SOURCE = 0;
constexpr u32 BINDING_OUTPUT = 1;

} // Anonymous namespace

bool BcGpuEncoder::Supported(const Device& device) {
    if (!device.IsKhrPushDescriptorSupported() || device.MaxPushDescriptors() < 2 ||
        !device.IsOptimalBcnSupported()) {
        return false;
    }
    constexpr VkFormatFeatureFlags BC_FEATURES =
        VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    return device.IsFormatSupported(SOURCE_FORMAT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
                                    FormatType::Optimal) &&
           device.IsFormatSupported(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, BC_FEATURES,
                                    FormatType::Optimal) &&
           device.IsFormatSupported(VK_FORMAT_BC3_UNORM_BLOCK, BC_FEATURES, FormatType::Optimal);
}

BcGpuEncoder::BcGpuEncoder(const Device& device_) : device{device_} {
    const vk::Device& logical = device.GetLogical();
    const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
        {
            .binding = BINDING_SOURCE,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
        {
            .binding = BINDING_OUTPUT,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
    }};
    set_layout = logical.CreateDescriptorSetLayout({
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    });
    const VkPushConstantRange push_range{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(PushConstants),
    };
    pipeline_layout = logical.CreatePipelineLayout({
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = set_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    });
    const std::span<const u32> code = BC_ENCODER_COMP_SPV;
    module = logical.CreateShaderModule({
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = static_cast<u32>(code.size_bytes()),
        .pCode = code.data(),
    });
    pipeline = logical.CreateComputePipeline(
        VkComputePipelineCreateInfo{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = *module,
                .pName = "main",
                .pSpecializationInfo = nullptr,
            },
            .layout = *pipeline_layout,
            .basePipelineHandle = {},
            .basePipelineIndex = 0,
        },
        // Compiled on a worker like the ASTC decoder: no shared, externally synchronized cache.
        VK_NULL_HANDLE);
}

BcGpuEncoder::~BcGpuEncoder() = default;

void BcGpuEncoder::Record(vk::CommandBuffer cmd, VkImageView source, u32 width, u32 height,
                          u32 layers, bool bc3, VkBuffer output, u64 output_size) const {
    const VkDescriptorImageInfo image_info{
        .sampler = VK_NULL_HANDLE,
        .imageView = source,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    const VkDescriptorBufferInfo buffer_info{
        .buffer = output,
        .offset = 0,
        .range = output_size,
    };
    const std::array<VkWriteDescriptorSet, 2> writes{{
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = BINDING_SOURCE,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &image_info,
            .pBufferInfo = nullptr,
            .pTexelBufferView = nullptr,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = BINDING_OUTPUT,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pImageInfo = nullptr,
            .pBufferInfo = &buffer_info,
            .pTexelBufferView = nullptr,
        },
    }};
    const PushConstants constants{
        .blocks_x = (width + 3) / 4,
        .blocks_y = (height + 3) / 4,
        .width = width,
        .height = height,
        .bc3 = bc3 ? 1u : 0u,
        .out_offset = 0,
    };
    cmd.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
    cmd.PushDescriptorSetKHR(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline_layout, 0, writes);
    cmd.PushConstants(*pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, constants);
    cmd.Dispatch((constants.blocks_x + 7) / 8, (constants.blocks_y + 7) / 8, layers);
}

std::shared_ptr<BcScratch> MakeBcScratch(const Device& device, const MemoryAllocator& allocator,
                                         u32 width, u32 height, u32 layers) {
    auto scratch = std::make_shared<BcScratch>();
    scratch->width = width;
    scratch->height = height;
    scratch->layers = layers;
    scratch->image = allocator.CreateImage(VkImageCreateInfo{
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = BcGpuEncoder::SOURCE_FORMAT,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = layers,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
    scratch->view = device.GetLogical().CreateImageView({
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = *scratch->image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
        .format = BcGpuEncoder::SOURCE_FORMAT,
        .components{
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = layers,
        },
    });
    scratch->blocks = allocator.CreateBuffer(
        VkBufferCreateInfo{
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = BcBlocksSize(width, height, layers, 16),
            .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        },
        MemoryUsage::DeviceLocal);
    return scratch;
}

std::vector<BcChunk> SplitBcLayers(u32 level, u32 first_layer, u32 layers, u32 width, u32 height,
                                   u32 x, u32 y) {
    const u64 layer_bytes = (std::max)(u64{width} * height * 4, u64{1});
    const u32 per_chunk = static_cast<u32>(
        std::clamp<u64>(BC_SCRATCH_BUDGET / layer_bytes, 1, (std::max)(layers, 1u)));
    std::vector<BcChunk> chunks;
    for (u32 layer = 0; layer < layers; layer += per_chunk) {
        chunks.push_back({
            .level = level,
            .first_layer = first_layer + layer,
            .layers = (std::min)(per_chunk, layers - layer),
            .width = width,
            .height = height,
            .x = x,
            .y = y,
        });
    }
    return chunks;
}

} // namespace Vulkan::Nvn

// SPDX-FileCopyrightText: Copyright 2026 Vortex727 (FUKKO)
// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// ASTC stand-ins held as BC1/BC3/BC7 (nvn_bc_encode.h, nvn_format.h ToBcStandin): the stand-in
// choice, the chunking and block rules, a GPU round trip of block-linear ASTC blocks through the
// replay's decoder, the BC scratch and the encoder into a BC image, and BC7 against BC3 on layered
// mip chains and alpha cut-outs, with the encode cost (SKIP without a GPU).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <string>
#include <memory>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <fmt/format.h>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/nvn/nvn_astc_gpu.h"
#include "video_core/renderer_vulkan/nvn/nvn_bc_encode.h"
#include "video_core/renderer_vulkan/nvn/nvn_format.h"
#include "video_core/renderer_vulkan/nvn/nvn_texture_layout.h"
#include "video_core/texture_cache/decode_bc.h"
#include "video_core/texture_cache/types.h"
#include "video_core/textures/decoders.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/vulkan_common/vma.h"

namespace {

using namespace Vulkan::Nvn;
using VideoCore::Surface::PixelFormat;

HostFormat AstcRgba8StandIn(PixelFormat pixel_format, bool srgb) {
    HostFormat host{};
    host.format = srgb ? VK_FORMAT_A8B8G8R8_SRGB_PACK32 : VK_FORMAT_A8B8G8R8_UNORM_PACK32;
    host.pixel_format = pixel_format;
    host.attachable = true;
    host.storage = !srgb;
    host.needs_transcode = true;
    return host;
}

struct HeadlessVulkan {
    std::shared_ptr<Common::DynamicLibrary> library;
    std::unique_ptr<Vulkan::vk::InstanceDispatch> dld;
    Vulkan::vk::Instance instance;
    std::unique_ptr<Vulkan::Device> device;
};

std::unique_ptr<HeadlessVulkan> MakeHeadlessVulkan() {
    try {
        auto result = std::make_unique<HeadlessVulkan>();
        result->library = Vulkan::OpenLibrary(nullptr);
        if (!result->library) {
            return nullptr;
        }
        result->dld = std::make_unique<Vulkan::vk::InstanceDispatch>();
        result->instance = Vulkan::CreateInstance(*result->library, *result->dld,
                                                  VK_API_VERSION_1_1,
                                                  Core::Frontend::WindowSystemType::Headless, false);
        const std::vector<VkPhysicalDevice> devices = result->instance.EnumeratePhysicalDevices();
        if (devices.empty()) {
            return nullptr;
        }
        result->device = std::make_unique<Vulkan::Device>(
            *result->instance, Vulkan::vk::PhysicalDevice(devices[0], *result->dld),
            VK_NULL_HANDLE, *result->dld);
        return result;
    } catch (const Vulkan::vk::Exception&) {
        return nullptr;
    } catch (const std::exception&) {
        return nullptr;
    }
}

/// A void-extent LDR block (spec 'Void-Extent Blocks') of one colour, 16 bits per channel.
std::array<u8, 16> VoidExtentBlock(const std::array<u16, 4>& rgba) {
    u64 low = 0x1FC;          // block mode: void extent
    low |= u64{3} << 10;      // reserved bits are ones
    low |= u64{0x1FFF} << 12; // no extent: every coordinate field is all ones
    low |= u64{0x1FFF} << 25;
    low |= u64{0x1FFF} << 38;
    low |= u64{0x1FFF} << 51;
    u64 high = 0;
    for (u32 i = 0; i < 4; ++i) {
        high |= u64{rgba[i]} << (16 * i);
    }
    std::array<u8, 16> block;
    std::memcpy(block.data(), &low, 8);
    std::memcpy(block.data() + 8, &high, 8);
    return block;
}

u8 ToUnorm8(u16 value) {
    return static_cast<u8>(std::lround(static_cast<double>(value) / 65535.0 * 255.0));
}

/// One level of an RGBA8 2D array, layer after layer.
struct Rgba8Level {
    u32 width{};
    u32 height{};
    u32 layers{};
    std::vector<u8> texels;

    u8* Texel(u32 x, u32 y, u32 layer) {
        return &texels[((size_t{layer} * height + y) * width + x) * 4];
    }
    const u8* Texel(u32 x, u32 y, u32 layer) const {
        return &texels[((size_t{layer} * height + y) * width + x) * 4];
    }
};

/// Level 0 from `texel`, then 2x2 box-filtered levels down to 1x1 (what a game's mips look like
/// after an ASTC decode, near enough).
template <typename Texel>
std::vector<Rgba8Level> MakeMipChain(u32 width, u32 height, u32 layers, Texel&& texel) {
    std::vector<Rgba8Level> levels;
    Rgba8Level base{width, height, layers, std::vector<u8>(size_t{width} * height * layers * 4)};
    for (u32 layer = 0; layer < layers; ++layer) {
        for (u32 y = 0; y < height; ++y) {
            for (u32 x = 0; x < width; ++x) {
                const std::array<u8, 4> value = texel(x, y, layer);
                std::memcpy(base.Texel(x, y, layer), value.data(), 4);
            }
        }
    }
    levels.push_back(std::move(base));
    while (levels.back().width > 1 || levels.back().height > 1) {
        const Rgba8Level& above = levels.back();
        Rgba8Level next{(std::max)(above.width / 2, 1u), (std::max)(above.height / 2, 1u), layers,
                        {}};
        next.texels.resize(size_t{next.width} * next.height * layers * 4);
        for (u32 layer = 0; layer < layers; ++layer) {
            for (u32 y = 0; y < next.height; ++y) {
                for (u32 x = 0; x < next.width; ++x) {
                    for (u32 c = 0; c < 4; ++c) {
                        u32 sum = 0;
                        for (u32 dy = 0; dy < 2; ++dy) {
                            for (u32 dx = 0; dx < 2; ++dx) {
                                const u32 sx = (std::min)(x * 2 + dx, above.width - 1);
                                const u32 sy = (std::min)(y * 2 + dy, above.height - 1);
                                sum += above.Texel(sx, sy, layer)[c];
                            }
                        }
                        next.Texel(x, y, layer)[c] = static_cast<u8>((sum + 2) / 4);
                    }
                }
            }
        }
        levels.push_back(std::move(next));
    }
    return levels;
}

struct GpuEncodeResult {
    std::vector<Rgba8Level> decoded; ///< per level, as a sampler reads the BC image
    double gpu_ms{};                 ///< staging copies, encodes and block copies, every level
};

/// Runs `levels` through the recorder's BC upload sequence (Recorder::UploadBcFromStaging and
/// RecordBcUpload: RGBA8 copied into the scratch a chunk of layers at a time, encoded, the blocks
/// copied into the level of a BC image), reads the image back and decodes it on the CPU.
GpuEncodeResult EncodeOnGpu(const Vulkan::Device& device, std::span<const Rgba8Level> levels,
                            AstcStandin kind, u32 quality, bool decode = true) {
    using namespace Vulkan;
    const bool bc7 = kind == AstcStandin::Bc7;
    const u32 block_bytes = kind == AstcStandin::Bc1 ? 8 : 16;
    const VkFormat format = bc7 ? VK_FORMAT_BC7_UNORM_BLOCK
                            : kind == AstcStandin::Bc3 ? VK_FORMAT_BC3_UNORM_BLOCK
                                                       : VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    const u32 width = levels[0].width;
    const u32 height = levels[0].height;
    const u32 layers = levels[0].layers;
    const u32 level_count = static_cast<u32>(levels.size());
    GpuEncodeResult result;
    VmaAllocator vma = device.CreateThreadSafeAllocator();
    {
        MemoryAllocator allocator(device, vma);
        const BcGpuEncoder encoder(device, bc7);
        u32 scratch_layers = 1;
        for (const BcChunk& chunk : SplitBcLayers(0, 0, layers, width, height)) {
            scratch_layers = (std::max)(scratch_layers, chunk.layers);
        }
        const std::shared_ptr<BcScratch> scratch =
            MakeBcScratch(device, allocator, width, height, scratch_layers, bc7);

        u64 staging_size = 0;
        std::vector<u64> staging_offsets;
        for (const Rgba8Level& level : levels) {
            staging_offsets.push_back(staging_size);
            staging_size += level.texels.size();
        }
        vk::Buffer staging = allocator.CreateBuffer(
            VkBufferCreateInfo{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .size = staging_size,
                .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
            },
            MemoryUsage::Upload);
        for (u32 level = 0; level < level_count; ++level) {
            std::memcpy(staging.Mapped().data() + staging_offsets[level],
                        levels[level].texels.data(), levels[level].texels.size());
        }
        staging.Flush();

        vk::Image image = allocator.CreateImage(VkImageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = format,
            .extent = {width, height, 1},
            .mipLevels = level_count,
            .arrayLayers = layers,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                     VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        });

        u64 download_size = 0;
        std::vector<VkBufferImageCopy> regions;
        for (u32 level = 0; level < level_count; ++level) {
            regions.push_back({
                .bufferOffset = download_size,
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, layers},
                .imageOffset = {0, 0, 0},
                .imageExtent = {levels[level].width, levels[level].height, 1},
            });
            download_size +=
                BcBlocksSize(levels[level].width, levels[level].height, layers, block_bytes);
        }
        vk::Buffer download = allocator.CreateBuffer(
            VkBufferCreateInfo{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .size = download_size,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
            },
            MemoryUsage::Download);

        const vk::Device& logical = device.GetLogical();
        vk::QueryPool queries = logical.CreateQueryPool({
            .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queryType = VK_QUERY_TYPE_TIMESTAMP,
            .queryCount = 2,
            .pipelineStatistics = 0,
        });
        vk::CommandPool pool = logical.CreateCommandPool({
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queueFamilyIndex = device.GetGraphicsFamily(),
        });
        vk::CommandBuffers buffers = pool.Allocate(1);
        const VkCommandBuffer raw = buffers[0];
        const vk::CommandBuffer cmd{raw, device.GetDispatchLoader()};
        cmd.Begin({
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            .pInheritanceInfo = nullptr,
        });
        cmd.ResetQueryPool(*queries, 0, 2);
        VkImageMemoryBarrier to_dst{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = *image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
                                 VK_REMAINING_ARRAY_LAYERS},
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                            to_dst);
        cmd.WriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *queries, 0);
        for (u32 level = 0; level < level_count; ++level) {
            const Rgba8Level& texels = levels[level];
            for (const BcChunk& chunk :
                 SplitBcLayers(level, 0, layers, texels.width, texels.height)) {
                const VkMemoryBarrier reuse{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                };
                const VkImageMemoryBarrier to_general{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = 0,
                    .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = *scratch->image,
                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                         VK_REMAINING_ARRAY_LAYERS},
                };
                cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, reuse, {}, to_general);
                const u64 layer_bytes = u64{texels.width} * texels.height * 4;
                const VkBufferImageCopy fill{
                    .bufferOffset = staging_offsets[level] + u64{chunk.first_layer} * layer_bytes,
                    .bufferRowLength = 0,
                    .bufferImageHeight = 0,
                    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, chunk.layers},
                    .imageOffset = {0, 0, 0},
                    .imageExtent = {texels.width, texels.height, 1},
                };
                cmd.CopyBufferToImage(*staging, *scratch->image, VK_IMAGE_LAYOUT_GENERAL, fill);
                const VkMemoryBarrier texels_written{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                };
                cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, texels_written);
                const u64 blocks_size =
                    BcBlocksSize(chunk.width, chunk.height, chunk.layers, block_bytes);
                if (bc7) {
                    encoder.RecordBc7(cmd, *scratch->view, chunk.width, chunk.height,
                                      chunk.layers, quality, *scratch->modes,
                                      BcBlocksSize(chunk.width, chunk.height, chunk.layers, 8),
                                      *scratch->blocks, blocks_size);
                } else {
                    encoder.Record(cmd, *scratch->view, chunk.width, chunk.height, chunk.layers,
                                   kind == AstcStandin::Bc3, *scratch->blocks, blocks_size);
                }
                const VkMemoryBarrier blocks_written{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                };
                cmd.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, blocks_written);
                const VkBufferImageCopy region{
                    .bufferOffset = 0,
                    .bufferRowLength = (chunk.width + 3) / 4 * 4,
                    .bufferImageHeight = (chunk.height + 3) / 4 * 4,
                    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, chunk.first_layer,
                                         chunk.layers},
                    .imageOffset = {0, 0, 0},
                    .imageExtent = {chunk.width, chunk.height, 1},
                };
                cmd.CopyBufferToImage(*scratch->blocks, *image,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region);
            }
        }
        cmd.WriteTimestamp(VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, *queries, 1);
        VkImageMemoryBarrier to_src = to_dst;
        to_src.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                            to_src);
        cmd.CopyImageToBuffer(*image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, *download, regions);
        cmd.End();
        vk::Fence fence = logical.CreateFence({
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
        });
        const VkSubmitInfo submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = nullptr,
            .waitSemaphoreCount = 0,
            .pWaitSemaphores = nullptr,
            .pWaitDstStageMask = nullptr,
            .commandBufferCount = 1,
            .pCommandBuffers = &raw,
            .signalSemaphoreCount = 0,
            .pSignalSemaphores = nullptr,
        };
        REQUIRE(device.GetGraphicsQueue().Submit(submit, *fence) == VK_SUCCESS);
        REQUIRE(fence.Wait() == VK_SUCCESS);
        std::array<u64, 2> ticks{};
        REQUIRE(logical.GetQueryResults(*queries, 0, 2, sizeof(ticks), ticks.data(), sizeof(u64),
                                        VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) ==
                VK_SUCCESS);
        const double period = device.GetPhysical().GetProperties().limits.timestampPeriod;
        result.gpu_ms = static_cast<double>(ticks[1] - ticks[0]) * period / 1e6;

        if (decode) {
            download.Invalidate();
            const u8* blocks = download.Mapped().data();
            for (u32 level = 0; level < level_count; ++level) {
                const Rgba8Level& texels = levels[level];
                Rgba8Level decoded{texels.width, texels.height, layers,
                                   std::vector<u8>(texels.texels.size())};
                const u64 layer_blocks = BcBlocksSize(texels.width, texels.height, 1, block_bytes);
                const size_t layer_texels = size_t{texels.width} * texels.height * 4;
                for (u32 layer = 0; layer < layers; ++layer) {
                    VideoCommon::BufferImageCopy copy{};
                    copy.image_subresource.num_layers = 1;
                    copy.image_extent = {texels.width, texels.height, 1};
                    copy.buffer_row_length = (texels.width + 3) / 4 * 4;
                    VideoCommon::DecompressBCn(
                        std::span<const u8>{blocks, static_cast<size_t>(layer_blocks)},
                        std::span<u8>{decoded.texels.data() + layer * layer_texels, layer_texels},
                        copy,
                        bc7 ? PixelFormat::BC7_UNORM
                            : kind == AstcStandin::Bc3 ? PixelFormat::BC3_UNORM
                                                       : PixelFormat::BC1_RGBA_UNORM);
                    blocks += layer_blocks;
                }
                result.decoded.push_back(std::move(decoded));
            }
        }
    }
    vmaDestroyAllocator(vma);
    return result;
}

struct Psnr {
    double rgb{};
    double alpha{};
};

/// PSNR of one layer of `decoded` against `expected`, RGB together and alpha alone (99 dB when
/// exact).
Psnr LayerPsnr(const Rgba8Level& expected, const Rgba8Level& decoded, u32 layer) {
    double rgb = 0.0;
    double alpha = 0.0;
    for (u32 y = 0; y < expected.height; ++y) {
        for (u32 x = 0; x < expected.width; ++x) {
            const u8* a = expected.Texel(x, y, layer);
            const u8* b = decoded.Texel(x, y, layer);
            for (u32 c = 0; c < 4; ++c) {
                const double d = double(a[c]) - double(b[c]);
                (c < 3 ? rgb : alpha) += d * d;
            }
        }
    }
    const double texels = double(expected.width) * expected.height;
    const auto psnr = [](double mse) {
        return mse <= 0.0 ? 99.0 : 10.0 * std::log10(255.0 * 255.0 / mse);
    };
    return {psnr(rgb / (texels * 3.0)), psnr(alpha / texels)};
}

/// Texels whose alpha a cut-out shader would see differently: 0 or 255 not kept exactly, or the
/// texel on the other side of an alpha test at 64, 128 or 192.
size_t AlphaTestChanges(const Rgba8Level& expected, const Rgba8Level& decoded) {
    size_t changes = 0;
    for (size_t i = 3; i < expected.texels.size(); i += 4) {
        const u32 want = expected.texels[i];
        const u32 got = decoded.texels[i];
        bool changed = (want == 0 || want == 255) && got != want;
        for (const u32 threshold : {64u, 128u, 192u}) {
            changed = changed || (want >= threshold) != (got >= threshold);
        }
        changes += changed ? 1 : 0;
    }
    return changes;
}

// Value noise: smooth bumps at `cell` texels, hashed per lattice point.
double Noise(u32 seed, double x, double y, double cell) {
    const auto hash = [seed](s64 ix, s64 iy) {
        u32 h = static_cast<u32>(ix * 374761393 + iy * 668265263) ^ (seed * 2246822519u);
        h = (h ^ (h >> 13)) * 1274126177u;
        return double((h ^ (h >> 16)) & 0xFFFF) / 65535.0;
    };
    const double fx = x / cell;
    const double fy = y / cell;
    const s64 ix = static_cast<s64>(std::floor(fx));
    const s64 iy = static_cast<s64>(std::floor(fy));
    const double tx = fx - double(ix);
    const double ty = fy - double(iy);
    const double sx = tx * tx * (3.0 - 2.0 * tx);
    const double sy = ty * ty * (3.0 - 2.0 * ty);
    const double top = hash(ix, iy) + (hash(ix + 1, iy) - hash(ix, iy)) * sx;
    const double bottom = hash(ix, iy + 1) + (hash(ix + 1, iy + 1) - hash(ix, iy + 1)) * sx;
    return top + (bottom - top) * sy;
}

u8 Clamp8(double value) {
    return static_cast<u8>(std::clamp(std::lround(value), 0l, 255l));
}

/// A terrain-like material layer: two colours blended by low-frequency noise (often opposite in
/// some channel, which a box diagonal can't follow), fine detail, a few hard edges, and an
/// alpha mask of soft blobs (the blend weights such arrays carry).
std::array<u8, 4> MaterialTexel(u32 x, u32 y, u32 layer) {
    const u32 seed = layer * 7919 + 13;
    const double fx = double(x);
    const double fy = double(y);
    const std::array<double, 3> a{40.0 + 30.0 * (layer % 3), 90.0 + 15.0 * (layer % 5), 30.0};
    const std::array<double, 3> b{170.0, 120.0 - 10.0 * (layer % 4), 60.0 + 25.0 * (layer % 7)};
    const double blend = Noise(seed, fx, fy, 24.0);
    const double detail = Noise(seed + 1, fx, fy, 3.0) - 0.5;
    const bool edge = Noise(seed + 2, fx, fy, 40.0) > 0.7;
    std::array<u8, 4> texel{};
    for (u32 c = 0; c < 3; ++c) {
        const double base = a[c] + (b[c] - a[c]) * blend + detail * 40.0;
        texel[c] = Clamp8(edge ? base * 0.45 : base);
    }
    texel[3] = Clamp8(Noise(seed + 3, fx, fy, 16.0) * 320.0 - 32.0);
    return texel;
}

/// Foliage cut-outs: opaque leaves (alpha 255) with holes (alpha 0) whose edges cut 4x4 blocks:
/// discs, checkers on 5, 6 and 10 texel cells (ASTC footprints, alpha jumping 0 -> 255 between
/// neighbours off the 4x4 grid) and a strip with a soft three-texel edge.
std::array<u8, 4> CutoutTexel(u32 x, u32 y, u32 layer) {
    const u32 seed = layer * 104729 + 7;
    std::array<u8, 4> texel{
        Clamp8(40.0 + 60.0 * Noise(seed, x, y, 5.0)),
        Clamp8(90.0 + 120.0 * Noise(seed + 1, x, y, 7.0)),
        Clamp8(20.0 + 50.0 * Noise(seed + 2, x, y, 4.0)),
        255,
    };
    const u32 band = y / 32;
    if (band % 4 == 0) {
        const u32 cell = band % 3 == 0 ? 5 : (band % 3 == 1 ? 6 : 10);
        texel[3] = ((x / cell + y / cell) & 1) != 0 ? 0 : 255;
    } else if (band % 4 == 1) {
        const double t = (double(x % 37) - 17.0) / 3.0;
        texel[3] = Clamp8(std::clamp(t, 0.0, 1.0) * 255.0);
    } else {
        const double cx = 11.0 + 23.0 * double((x + 5) / 23);
        const double cy = 13.0 + 19.0 * double((y + 3) / 19);
        const double r = 4.5 + 5.0 * Noise(seed + 3, cx, cy, 1.0);
        const double dx = double(x) - cx;
        const double dy = double(y) - cy;
        texel[3] = dx * dx + dy * dy < r * r ? 0 : 255;
    }
    return texel;
}

} // Anonymous namespace

TEST_CASE("NVN ASTC stand-in: the environment overrides the setting", "[nvn]") {
    using Settings::AstcRecompression;
    REQUIRE(ChooseAstcStandin(nullptr, AstcRecompression::Bc3) == AstcStandin::Bc3);
    REQUIRE(ChooseAstcStandin(nullptr, AstcRecompression::Bc1) == AstcStandin::Bc1);
    REQUIRE(ChooseAstcStandin(nullptr, AstcRecompression::Uncompressed) == AstcStandin::Rgba8);
    REQUIRE(ChooseAstcStandin("rgba8", AstcRecompression::Bc3) == AstcStandin::Rgba8);
    REQUIRE(ChooseAstcStandin("bc1", AstcRecompression::Uncompressed) == AstcStandin::Bc1);
    REQUIRE(ChooseAstcStandin("bc3", AstcRecompression::Bc1) == AstcStandin::Bc3);
    REQUIRE(ChooseAstcStandin("bc7", AstcRecompression::Bc3) == AstcStandin::Bc7);
    REQUIRE(ParseBc7Quality(nullptr) == BC7_QUALITY_DEFAULT);
    REQUIRE(ParseBc7Quality("0") == 0);
    REQUIRE(ParseBc7Quality("2") == 2);
    REQUIRE(ParseBc7Quality("3") == BC7_QUALITY_DEFAULT);
    REQUIRE(ParseBc7Quality("12") == BC7_QUALITY_DEFAULT);
    // Anything else is not set.
    REQUIRE(ChooseAstcStandin("", AstcRecompression::Bc1) == AstcStandin::Bc1);
    REQUIRE(ChooseAstcStandin("BC9", AstcRecompression::Uncompressed) == AstcStandin::Rgba8);
}

TEST_CASE("NVN ASTC stand-in: BC formats keep the sRGB-ness and drop render/storage use", "[nvn]") {
    const HostFormat unorm = AstcRgba8StandIn(PixelFormat::ASTC_2D_6X6_UNORM, false);
    const HostFormat srgb = AstcRgba8StandIn(PixelFormat::ASTC_2D_8X8_SRGB, true);

    const HostFormat bc3 = ToBcStandin(unorm, AstcStandin::Bc3);
    REQUIRE(bc3.format == VK_FORMAT_BC3_UNORM_BLOCK);
    REQUIRE(bc3.bc_standin);
    REQUIRE(bc3.needs_transcode);
    REQUIRE_FALSE(bc3.attachable);
    REQUIRE_FALSE(bc3.storage);
    REQUIRE(bc3.storage_view == VK_FORMAT_UNDEFINED);
    REQUIRE(BcStandinBlockBytes(bc3) == 16);

    REQUIRE(ToBcStandin(srgb, AstcStandin::Bc3).format == VK_FORMAT_BC3_SRGB_BLOCK);
    REQUIRE(ToBcStandin(srgb, AstcStandin::Bc1).format == VK_FORMAT_BC1_RGBA_SRGB_BLOCK);
    const HostFormat bc1 = ToBcStandin(unorm, AstcStandin::Bc1);
    REQUIRE(bc1.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK);
    REQUIRE(BcStandinBlockBytes(bc1) == 8);
    const HostFormat bc7 = ToBcStandin(unorm, AstcStandin::Bc7);
    REQUIRE(bc7.format == VK_FORMAT_BC7_UNORM_BLOCK);
    REQUIRE(ToBcStandin(srgb, AstcStandin::Bc7).format == VK_FORMAT_BC7_SRGB_BLOCK);
    REQUIRE(BcStandinBlockBytes(bc7) == 16);
    REQUIRE(BcStandinOf(bc7) == AstcStandin::Bc7);
    REQUIRE(BcStandinOf(bc3) == AstcStandin::Bc3);
    REQUIRE(BcStandinOf(unorm) == AstcStandin::Rgba8);
    REQUIRE(ViewsBcStandin(VK_FORMAT_BC7_SRGB_BLOCK, VK_FORMAT_BC7_UNORM_BLOCK));
    REQUIRE_FALSE(ViewsBcStandin(VK_FORMAT_BC3_UNORM_BLOCK, VK_FORMAT_BC7_UNORM_BLOCK));

    // Rgba8, non-ASTC and native formats are left alone.
    REQUIRE_FALSE(ToBcStandin(unorm, AstcStandin::Rgba8).bc_standin);
    REQUIRE(ToBcStandin(unorm, AstcStandin::Rgba8).format == VK_FORMAT_A8B8G8R8_UNORM_PACK32);
    HostFormat native = unorm;
    native.needs_transcode = false;
    REQUIRE_FALSE(ToBcStandin(native, AstcStandin::Bc3).bc_standin);
    HostFormat bcn = AstcRgba8StandIn(PixelFormat::BC7_UNORM, false);
    REQUIRE_FALSE(ToBcStandin(bcn, AstcStandin::Bc3).bc_standin);
    REQUIRE(BcStandinBlockBytes(unorm) == 0);

    // Views: the same BC format in either encoding, nothing else.
    REQUIRE(ViewsBcStandin(VK_FORMAT_BC3_SRGB_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK));
    REQUIRE(ViewsBcStandin(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC1_RGBA_SRGB_BLOCK));
    REQUIRE_FALSE(ViewsBcStandin(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, VK_FORMAT_BC3_UNORM_BLOCK));
    REQUIRE_FALSE(ViewsBcStandin(VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_BC3_UNORM_BLOCK));
    REQUIRE_FALSE(ViewsBcStandin(VK_FORMAT_A8B8G8R8_UNORM_PACK32, VK_FORMAT_BC3_UNORM_BLOCK));
}

TEST_CASE("NVN ASTC stand-in: BC copies follow the block edge rules", "[nvn]") {
    REQUIRE(BcRegionAligned(0, 0, 70, 45, 70, 45));   // a whole level of any size
    REQUIRE(BcRegionAligned(0, 0, 2, 1, 2, 1));       // a level smaller than a block
    REQUIRE(BcRegionAligned(64, 32, 6, 13, 70, 45));  // reaching the right and bottom edges
    REQUIRE(BcRegionAligned(8, 4, 16, 8, 70, 45));    // inside, whole blocks
    REQUIRE_FALSE(BcRegionAligned(5, 0, 10, 10, 70, 45)); // a 5x5 ASTC block offset
    REQUIRE_FALSE(BcRegionAligned(0, 0, 10, 8, 70, 45));  // ends inside a block
    REQUIRE_FALSE(BcRegionAligned(0, 0, 8, 6, 70, 45));
}

TEST_CASE("NVN ASTC stand-in: chunks and sizes", "[nvn]") {
    REQUIRE(BcBlocksSize(70, 45, 3, 16) == u64{18} * 12 * 3 * 16);
    REQUIRE(BcBlocksSize(1, 1, 1, 8) == 8);
    REQUIRE(BcBlocksSize(2, 3, 1, 16) == 16);

    // OoT's 1365x1365x90 arrays: 7.1 MiB of RGBA8 per layer, nine layers per 64 MiB chunk.
    const std::vector<BcChunk> chunks = SplitBcLayers(0, 0, 90, 1365, 1365);
    REQUIRE(chunks.size() == 10);
    u32 next = 0;
    for (const BcChunk& chunk : chunks) {
        REQUIRE(chunk.first_layer == next);
        REQUIRE(u64{chunk.width} * chunk.height * chunk.layers * 4 <= BC_SCRATCH_BUDGET);
        next += chunk.layers;
    }
    REQUIRE(next == 90);
    // A layer above the budget is one chunk of its own; offsets carry over.
    const std::vector<BcChunk> big = SplitBcLayers(2, 5, 2, 6144, 6144, 4, 8);
    REQUIRE(big.size() == 2);
    REQUIRE(big[1].first_layer == 6);
    REQUIRE(big[1].layers == 1);
    REQUIRE(big[1].level == 2);
    REQUIRE((big[1].x == 4 && big[1].y == 8));

    // What the census counts for such an array: 11 levels, every layer.
    u64 rgba8 = 0;
    u64 bc3 = 0;
    for (u32 level = 0; level < 11; ++level) {
        const u32 side = (std::max)(1365u >> level, 1u);
        rgba8 += u64{side} * side * 4;
        bc3 += BcBlocksSize(side, side, 1, 16);
    }
    REQUIRE(rgba8 * 90 > (u64{850} << 20));
    REQUIRE(bc3 * 90 < (u64{230} << 20));
}

TEST_CASE("NVN ASTC stand-in: block-linear ASTC decodes and re-encodes into a BC image", "[nvn]") {
    using namespace Vulkan;
    auto vulkan = MakeHeadlessVulkan();
    if (!vulkan) {
        SKIP("no Vulkan device");
    }
    const Device& device = *vulkan->device;
    if (!AstcGpuDecoder::Supported(device) || !BcGpuEncoder::Supported(device)) {
        SKIP("the device cannot run the GPU ASTC decoder and BC encoder");
    }

    // ASTC 4x4 UNORM (NVN format 0x79), 2D array of sides that are not block multiples. One colour
    // per ASTC block: every 4x4 BC block is one colour too, which BC holds within its 565/8-bit
    // quantization.
    constexpr s32 FORMAT = 0x79;
    constexpr u32 WIDTH = 70;
    constexpr u32 HEIGHT = 45;
    constexpr u32 LEVELS = 4;
    constexpr u32 LAYERS = 3;
    TextureLayoutDesc desc{
        .target = static_cast<s32>(TexTarget::Dim2DArray),
        .format = FORMAT,
        .flags = 0,
        .samples = 0,
        .width = WIDTH,
        .height = HEIGHT,
        .depth = LAYERS,
        .levels = LEVELS,
        .stride = 0,
        .pkg_block_h = 0,
    };
    const TextureLayout layout = ComputeTextureLayout(desc);
    const BlockInfo block = FormatBlockInfo(FORMAT);
    REQUIRE(block.bytes == 16);

    std::mt19937 random(4321);
    std::vector<u8> guest(static_cast<size_t>(layout.size) + 4096, 0);
    struct Expected {
        u32 level;
        u32 layer;
        u32 width;
        u32 height;
        std::vector<u8> texels; // RGBA8
    };
    std::vector<Expected> expected;
    std::vector<AstcGpuDecoder::Level> levels;
    for (u32 level = 0; level < LEVELS; ++level) {
        const u32 width = (std::max)(WIDTH >> level, 1u);
        const u32 height = (std::max)(HEIGHT >> level, 1u);
        const u32 columns = (width + 3) / 4;
        const u32 rows = (height + 3) / 4;
        u32 tile_h = 0;
        u32 tile_d = 0;
        TextureLevelTiles(desc, level, tile_h, tile_d);
        for (u32 layer = 0; layer < LAYERS; ++layer) {
            std::vector<u8> linear(size_t{columns} * rows * 16);
            Expected texels{level, layer, width, height,
                            std::vector<u8>(size_t{width} * height * 4)};
            for (u32 row = 0; row < rows; ++row) {
                for (u32 column = 0; column < columns; ++column) {
                    const std::array<u16, 4> color{
                        static_cast<u16>(random()), static_cast<u16>(random()),
                        static_cast<u16>(random()), static_cast<u16>(random())};
                    const auto encoded = VoidExtentBlock(color);
                    std::memcpy(&linear[(size_t{row} * columns + column) * 16], encoded.data(), 16);
                    for (u32 y = row * 4; y < (std::min)(row * 4 + 4, height); ++y) {
                        for (u32 x = column * 4; x < (std::min)(column * 4 + 4, width); ++x) {
                            for (u32 c = 0; c < 4; ++c) {
                                texels.texels[(size_t{y} * width + x) * 4 + c] = ToUnorm8(color[c]);
                            }
                        }
                    }
                }
            }
            const u64 offset = u64{layer} * layout.layer_size + TextureLevelOffset(desc, level);
            Tegra::Texture::SwizzleTexture(
                std::span<u8>{guest.data() + offset, guest.size() - static_cast<size_t>(offset)},
                linear, 16, columns, rows, 1, tile_h, tile_d, 0, true);
            expected.push_back(std::move(texels));
        }
        levels.push_back({
            .level = 0, // the scratch's level
            .layers = LAYERS,
            .columns = columns,
            .rows = rows,
            .offset = TextureLevelOffset(desc, level),
            .layer_stride = static_cast<u32>(layout.layer_size),
            .block_width = 4,
            .block_height = 4,
            .tile_height = tile_h,
            .tile_depth = tile_d,
        });
    }

    for (const bool bc3 : {true, false}) {
        INFO((bc3 ? "BC3" : "BC1"));
        const u32 block_bytes = bc3 ? 16 : 8;
        VmaAllocator vma = device.CreateThreadSafeAllocator();
        {
            MemoryAllocator allocator(device, vma);
            const AstcGpuDecoder decoder(device);
            const BcGpuEncoder encoder(device);
            const std::shared_ptr<BcScratch> scratch =
                MakeBcScratch(device, allocator, WIDTH, HEIGHT, LAYERS);

            vk::Buffer input = allocator.CreateBuffer(
                VkBufferCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .size = guest.size(),
                    .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                    .queueFamilyIndexCount = 0,
                    .pQueueFamilyIndices = nullptr,
                },
                MemoryUsage::Upload);
            std::memcpy(input.Mapped().data(), guest.data(), guest.size());
            input.Flush();

            // The stand-in as the replay creates it (ToBcStandin): sampled and copied only.
            vk::Image image = allocator.CreateImage(VkImageCreateInfo{
                .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT,
                .imageType = VK_IMAGE_TYPE_2D,
                .format = bc3 ? VK_FORMAT_BC3_UNORM_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK,
                .extent = {WIDTH, HEIGHT, 1},
                .mipLevels = LEVELS,
                .arrayLayers = LAYERS,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .tiling = VK_IMAGE_TILING_OPTIMAL,
                .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                         VK_IMAGE_USAGE_SAMPLED_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
                .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            });

            u64 download_size = 0;
            std::vector<VkBufferImageCopy> regions;
            for (const Expected& texels : expected) {
                regions.push_back({
                    .bufferOffset = download_size,
                    .bufferRowLength = 0,
                    .bufferImageHeight = 0,
                    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, texels.level, texels.layer, 1},
                    .imageOffset = {0, 0, 0},
                    .imageExtent = {texels.width, texels.height, 1},
                });
                download_size += BcBlocksSize(texels.width, texels.height, 1, block_bytes);
            }
            vk::Buffer download = allocator.CreateBuffer(
                VkBufferCreateInfo{
                    .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                    .pNext = nullptr,
                    .flags = 0,
                    .size = download_size,
                    .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                    .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                    .queueFamilyIndexCount = 0,
                    .pQueueFamilyIndices = nullptr,
                },
                MemoryUsage::Download);

            const vk::Device& logical = device.GetLogical();
            vk::CommandPool pool = logical.CreateCommandPool({
                .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .queueFamilyIndex = device.GetGraphicsFamily(),
            });
            vk::CommandBuffers buffers = pool.Allocate(1);
            const VkCommandBuffer raw = buffers[0];
            const vk::CommandBuffer cmd{raw, device.GetDispatchLoader()};
            cmd.Begin({
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                .pNext = nullptr,
                .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
                .pInheritanceInfo = nullptr,
            });
            VkImageMemoryBarrier to_dst{
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = 0,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = *image,
                .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
                                     VK_REMAINING_ARRAY_LAYERS},
            };
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                to_dst);
            std::vector<vk::ImageView> views;
            // The recorder's sequence (Recorder::RecordBcUpload), one level per scratch fill.
            for (u32 level = 0; level < LEVELS; ++level) {
                const u32 width = (std::max)(WIDTH >> level, 1u);
                const u32 height = (std::max)(HEIGHT >> level, 1u);
                const VkMemoryBarrier reuse{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                };
                const VkImageMemoryBarrier to_general{
                    .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = 0,
                    .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                    .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
                    .newLayout = VK_IMAGE_LAYOUT_GENERAL,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .image = *scratch->image,
                    .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0,
                                         VK_REMAINING_ARRAY_LAYERS},
                };
                cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, reuse, {}, to_general);
                for (vk::ImageView& view :
                     decoder.Record(cmd, *input, 0, guest.size(), *scratch->image,
                                    std::span{&levels[level], 1})) {
                    views.push_back(std::move(view));
                }
                const VkMemoryBarrier texels_written{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                };
                cmd.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, texels_written);
                encoder.Record(cmd, *scratch->view, width, height, LAYERS, bc3, *scratch->blocks,
                               BcBlocksSize(width, height, LAYERS, block_bytes));
                const VkMemoryBarrier blocks_written{
                    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
                };
                cmd.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                    VK_PIPELINE_STAGE_TRANSFER_BIT, 0, blocks_written);
                const VkBufferImageCopy region{
                    .bufferOffset = 0,
                    .bufferRowLength = (width + 3) / 4 * 4,
                    .bufferImageHeight = (height + 3) / 4 * 4,
                    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, level, 0, LAYERS},
                    .imageOffset = {0, 0, 0},
                    .imageExtent = {width, height, 1},
                };
                cmd.CopyBufferToImage(*scratch->blocks, *image,
                                      VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region);
            }
            VkImageMemoryBarrier to_src = to_dst;
            to_src.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            to_src.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                                to_src);
            cmd.CopyImageToBuffer(*image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, *download, regions);
            cmd.End();
            vk::Fence fence = logical.CreateFence({
                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
            });
            const VkSubmitInfo submit{
                .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
                .pNext = nullptr,
                .waitSemaphoreCount = 0,
                .pWaitSemaphores = nullptr,
                .pWaitDstStageMask = nullptr,
                .commandBufferCount = 1,
                .pCommandBuffers = &raw,
                .signalSemaphoreCount = 0,
                .pSignalSemaphores = nullptr,
            };
            REQUIRE(device.GetGraphicsQueue().Submit(submit, *fence) == VK_SUCCESS);
            REQUIRE(fence.Wait() == VK_SUCCESS);
            download.Invalidate();

            // The blocks as a sampler reads them, against the ASTC texels: 565 colour endpoints
            // (5 bits: within 5), BC3's 8-bit alpha (within 1), BC1's punch-through alpha.
            const u8* blocks = download.Mapped().data();
            size_t wrong = 0;
            for (const Expected& texels : expected) {
                const u64 size = BcBlocksSize(texels.width, texels.height, 1, block_bytes);
                std::vector<u8> decoded(texels.texels.size());
                VideoCommon::BufferImageCopy copy{};
                copy.image_subresource.num_layers = 1;
                copy.image_extent = {texels.width, texels.height, 1};
                copy.buffer_row_length = (texels.width + 3) / 4 * 4; // the decoder steps rows by it
                VideoCommon::DecompressBCn(std::span<const u8>{blocks, static_cast<size_t>(size)},
                                           decoded, copy,
                                           bc3 ? PixelFormat::BC3_UNORM
                                               : PixelFormat::BC1_RGBA_UNORM);
                for (size_t i = 0; i < decoded.size(); i += 4) {
                    std::array<int, 4> want{texels.texels[i], texels.texels[i + 1],
                                            texels.texels[i + 2], texels.texels[i + 3]};
                    if (!bc3) {
                        want = want[3] < 128 ? std::array<int, 4>{0, 0, 0, 0}
                                             : std::array<int, 4>{want[0], want[1], want[2], 255};
                    }
                    for (u32 c = 0; c < 4; ++c) {
                        const int difference = std::abs(int(decoded[i + c]) - want[c]);
                        if (difference > (c == 3 ? 1 : 5)) {
                            if (wrong < 8) {
                                WARN("level " << texels.level << " layer " << texels.layer
                                              << " texel " << i / 4 % texels.width << ","
                                              << i / 4 / texels.width << " channel " << c
                                              << ": bc " << int(decoded[i + c]) << " expected "
                                              << want[c]);
                            }
                            ++wrong;
                        }
                    }
                }
                blocks += size;
            }
            REQUIRE(wrong == 0);
        }
        vmaDestroyAllocator(vma);
    }
}

TEST_CASE("NVN ASTC stand-in: BC7 keeps every layer of every level of an array", "[nvn]") {
    auto vulkan = MakeHeadlessVulkan();
    if (!vulkan) {
        SKIP("no Vulkan device");
    }
    const Vulkan::Device& device = *vulkan->device;
    if (!BcGpuEncoder::Supported(device) || !BcGpuEncoder::SupportsBc7(device)) {
        SKIP("the device cannot run the BC7 encoder");
    }
    // Odd sides: every level down to 1x1 ends in partial blocks; the eight layers are encoded
    // in one chunk per level, as the recorder does (BC_SCRATCH_BUDGET).
    REQUIRE(SplitBcLayers(0, 0, 8, 341, 341).size() == 1);
    for (const bool opaque : {true, false}) {
        const std::vector<Rgba8Level> levels =
            MakeMipChain(341, 341, 8, [opaque](u32 x, u32 y, u32 layer) {
                std::array<u8, 4> texel = MaterialTexel(x, y, layer);
                texel[3] = opaque ? 255 : texel[3];
                return texel;
            });
        REQUIRE(levels.size() == 9);
        const char* const content = opaque ? "opaque" : "blend alpha";

        const GpuEncodeResult bc3 = EncodeOnGpu(device, levels, AstcStandin::Bc3, 0);
        for (u32 quality = 0; quality <= BC7_QUALITY_MAX; ++quality) {
            const GpuEncodeResult bc7 = EncodeOnGpu(device, levels, AstcStandin::Bc7, quality);
            double worst_rgb = 99.0;
            double worst_alpha = 99.0;
            for (u32 level = 0; level < levels.size(); ++level) {
                std::string line =
                    fmt::format("{} BC7 q{} level {} ({}x{}) RGB/alpha dB, BC3 in ():",
                                content, quality, level, levels[level].width,
                                levels[level].height);
                for (u32 layer = 0; layer < levels[level].layers; ++layer) {
                    const Psnr ours = LayerPsnr(levels[level], bc7.decoded[level], layer);
                    const Psnr theirs = LayerPsnr(levels[level], bc3.decoded[level], layer);
                    line += fmt::format(" {:.1f}/{:.1f} ({:.1f}/{:.1f})", ours.rgb, ours.alpha,
                                        theirs.rgb, theirs.alpha);
                    worst_rgb = (std::min)(worst_rgb, ours.rgb);
                    worst_alpha = (std::min)(worst_alpha, ours.alpha);
                    INFO("level " << level << " layer " << layer);
                    // The same bounds for every layer at every level, and no worse than the BC3
                    // encode of the same layer: a layer after the first that breaks at a small
                    // level (dark grid lines between tiles) falls far below both.
                    CHECK(ours.rgb >= (opaque ? 33.0 : 27.0));
                    CHECK(ours.alpha >= 28.0);
                    CHECK(ours.rgb >= theirs.rgb - 2.0);
                }
                WARN(line);
            }
            WARN(fmt::format("{} BC7 q{}: worst layer/level RGB {:.2f} dB, alpha {:.2f} dB",
                             content, quality, worst_rgb, worst_alpha));
        }
    }
}

TEST_CASE("NVN ASTC stand-in: BC7 keeps alpha cut-outs", "[nvn]") {
    auto vulkan = MakeHeadlessVulkan();
    if (!vulkan) {
        SKIP("no Vulkan device");
    }
    const Vulkan::Device& device = *vulkan->device;
    if (!BcGpuEncoder::Supported(device) || !BcGpuEncoder::SupportsBc7(device)) {
        SKIP("the device cannot run the BC7 encoder");
    }
    const std::vector<Rgba8Level> levels = MakeMipChain(213, 157, 3, CutoutTexel);
    const GpuEncodeResult bc3 = EncodeOnGpu(device, levels, AstcStandin::Bc3, 0);
    for (u32 quality = 0; quality <= BC7_QUALITY_MAX; ++quality) {
        const GpuEncodeResult bc7 = EncodeOnGpu(device, levels, AstcStandin::Bc7, quality);
        for (u32 level = 0; level < levels.size(); ++level) {
            const size_t ours = AlphaTestChanges(levels[level], bc7.decoded[level]);
            const size_t theirs = AlphaTestChanges(levels[level], bc3.decoded[level]);
            Psnr ours_psnr{99.0, 99.0};
            Psnr theirs_psnr{99.0, 99.0};
            for (u32 layer = 0; layer < levels[level].layers; ++layer) {
                const Psnr a = LayerPsnr(levels[level], bc7.decoded[level], layer);
                const Psnr b = LayerPsnr(levels[level], bc3.decoded[level], layer);
                ours_psnr = {(std::min)(ours_psnr.rgb, a.rgb),
                             (std::min)(ours_psnr.alpha, a.alpha)};
                theirs_psnr = {(std::min)(theirs_psnr.rgb, b.rgb),
                               (std::min)(theirs_psnr.alpha, b.alpha)};
            }
            WARN(fmt::format("cut-outs q{} level {} ({}x{}): alpha-test changes BC7 {} BC3 {}; "
                             "worst layer RGB/alpha dB BC7 {:.1f}/{:.1f} BC3 {:.1f}/{:.1f}",
                             quality, level, levels[level].width, levels[level].height, ours,
                             theirs, ours_psnr.rgb, ours_psnr.alpha, theirs_psnr.rgb,
                             theirs_psnr.alpha));
            INFO("level " << level);
            CHECK(ours == 0);
        }
    }
}

TEST_CASE("NVN ASTC stand-in: BC7 encode cost of a 1365x1365 terrain array chunk", "[nvn]") {
    auto vulkan = MakeHeadlessVulkan();
    if (!vulkan) {
        SKIP("no Vulkan device");
    }
    const Vulkan::Device& device = *vulkan->device;
    if (!BcGpuEncoder::Supported(device) || !BcGpuEncoder::SupportsBc7(device)) {
        SKIP("the device cannot run the BC7 encoder");
    }
    // One recorder chunk of OoT's terrain arrays: nine 1365x1365 layers, eleven levels. The
    // material layers are harsher than a decoded ASTC 8x8 texture (fine noise in every block);
    // the smooth ones keep only their low-frequency blend, closer to what ASTC 8x8 leaves.
    constexpr u32 LAYERS = 9;
    for (const bool smooth : {false, true}) {
        const std::vector<Rgba8Level> levels =
            MakeMipChain(1365, 1365, LAYERS, [smooth](u32 x, u32 y, u32 layer) {
                if (!smooth) {
                    return MaterialTexel(x, y, layer);
                }
                const double blend = Noise(layer * 31 + 5, x, y, 48.0);
                const double grain = Noise(layer, x, y, 9.0);
                return std::array<u8, 4>{Clamp8(60.0 + 120.0 * blend),
                                         Clamp8(110.0 - 40.0 * blend + 20.0 * grain),
                                         Clamp8(40.0 + 50.0 * blend), 255};
            });
        REQUIRE(levels.size() == 11);
        const char* const content = smooth ? "smooth" : "material";
        const auto report = [&](const char* name, const GpuEncodeResult& result) {
            const double per_layer = result.gpu_ms / LAYERS;
            WARN(fmt::format("{} {}: {:.1f} ms for 1365x1365x{} with 11 levels ({:.2f} ms per "
                             "layer, ~{:.0f} ms for 90 layers)",
                             content, name, result.gpu_ms, LAYERS, per_layer, per_layer * 90.0));
        };
        // Warm up the pipelines and clocks, then measure.
        (void)EncodeOnGpu(device, levels, AstcStandin::Bc3, 0, false);
        report("BC3", EncodeOnGpu(device, levels, AstcStandin::Bc3, 0, false));
        for (u32 quality = 0; quality <= BC7_QUALITY_MAX; ++quality) {
            const GpuEncodeResult result =
                EncodeOnGpu(device, levels, AstcStandin::Bc7, quality, false);
            report(quality == 0 ? "BC7 q0" : (quality == 1 ? "BC7 q1" : "BC7 q2"), result);
        }
    }
}

// SPDX-FileCopyrightText: Copyright 2026 Vortex727 (FUKKO)
// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// ASTC stand-ins held as BC1/BC3 (nvn_bc_encode.h, nvn_format.h ToBcStandin): the stand-in choice,
// the chunking and block rules, and a GPU round trip of block-linear ASTC blocks through the
// replay's decoder, the BC scratch and the encoder into a BC image (SKIP without a GPU).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>

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

} // Anonymous namespace

TEST_CASE("NVN ASTC stand-in: the environment overrides the setting", "[nvn]") {
    using Settings::AstcRecompression;
    REQUIRE(ChooseAstcStandin(nullptr, AstcRecompression::Bc3) == AstcStandin::Bc3);
    REQUIRE(ChooseAstcStandin(nullptr, AstcRecompression::Bc1) == AstcStandin::Bc1);
    REQUIRE(ChooseAstcStandin(nullptr, AstcRecompression::Uncompressed) == AstcStandin::Rgba8);
    REQUIRE(ChooseAstcStandin("rgba8", AstcRecompression::Bc3) == AstcStandin::Rgba8);
    REQUIRE(ChooseAstcStandin("bc1", AstcRecompression::Uncompressed) == AstcStandin::Bc1);
    REQUIRE(ChooseAstcStandin("bc3", AstcRecompression::Bc1) == AstcStandin::Bc3);
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

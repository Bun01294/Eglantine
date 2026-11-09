// SPDX-FileCopyrightText: Copyright 2026 Vortex727 (FUKKO)
// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
class Device;
class MemoryAllocator;
} // namespace Vulkan

namespace Vulkan::Nvn {

/// Compresses RGBA8 texels to BC1 or BC3 blocks on the GPU (the emulated path's `bc_encoder.comp`,
/// the shader its `BcEncoderPass` runs), for ASTC stand-ins held as BC (`HostFormat::bc_standin`).
/// The replay records the dispatch into its own command buffers with push descriptors, as
/// `AstcGpuDecoder` does.
class BcGpuEncoder {
public:
    /// Format of the storage views the encoder reads (and the ASTC decoder writes).
    static constexpr VkFormat SOURCE_FORMAT = VK_FORMAT_A8B8G8R8_UNORM_PACK32;

    /// True when the device can run the encoder (push descriptors, a storable RGBA8 format) and
    /// sample and copy into BC1/BC3 images.
    [[nodiscard]] static bool Supported(const Device& device);

    explicit BcGpuEncoder(const Device& device);
    ~BcGpuEncoder();

    /// Records one dispatch into `cmd` (outside a render pass): the first `layers` layers of
    /// `source` (a 2D array storage view in GENERAL layout), `width` x `height` texels each, become
    /// blocks row-major, layer after layer, from the start of `output` (`output_size` bytes).
    void Record(vk::CommandBuffer cmd, VkImageView source, u32 width, u32 height, u32 layers,
                bool bc3, VkBuffer output, u64 output_size) const;

private:
    const Device& device;
    vk::DescriptorSetLayout set_layout;
    vk::PipelineLayout pipeline_layout;
    vk::ShaderModule module;
    vk::Pipeline pipeline;
};

/// The RGBA8 image a BC stand-in upload decodes (or copies) its texels into before the encoder
/// reads them, and the buffer the blocks go to before they are copied into the stand-in. One is
/// reused for every upload; callers keep it alive through the flight that records into it.
struct BcScratch {
    vk::Image image;   ///< SOURCE_FORMAT, 1 level, STORAGE | TRANSFER_DST
    vk::ImageView view; ///< 2D array storage view of every layer
    vk::Buffer blocks; ///< device-local, room for the BC3 blocks of the whole image
    u32 width{};
    u32 height{};
    u32 layers{};

    [[nodiscard]] bool Fits(u32 w, u32 h, u32 l) const {
        return w <= width && h <= height && l <= layers;
    }
};

/// Creates a scratch of `width` x `height` x `layers` texels. Throws vk::Exception.
[[nodiscard]] std::shared_ptr<BcScratch> MakeBcScratch(const Device& device,
                                                       const MemoryAllocator& allocator, u32 width,
                                                       u32 height, u32 layers);

/// Bytes of the BC blocks of a `width` x `height` x `layers` region (`block_bytes` 8 or 16).
[[nodiscard]] constexpr u64 BcBlocksSize(u32 width, u32 height, u32 layers, u32 block_bytes) {
    return u64{(width + 3) / 4} * ((height + 3) / 4) * layers * block_bytes;
}

/// The RGBA8 texels of one scratch fill at most (a single layer may need more: it is never split).
inline constexpr u64 BC_SCRATCH_BUDGET = u64{64} << 20;

/// One scratch fill of a BC stand-in upload: `layers` consecutive layers of one level, written at
/// (`x`, `y`) of the level.
struct BcChunk {
    u32 level{};
    u32 first_layer{};
    u32 layers{1};
    u32 width{};
    u32 height{};
    u32 x{};
    u32 y{};
};

/// Splits `layers` layers of a `width` x `height` region into chunks whose RGBA8 texels fit
/// BC_SCRATCH_BUDGET (at least one layer each).
[[nodiscard]] std::vector<BcChunk> SplitBcLayers(u32 level, u32 first_layer, u32 layers, u32 width,
                                                 u32 height, u32 x = 0, u32 y = 0);

/// Whether a copy of `width` x `height` texels at (`x`, `y`) of a `level_width` x `level_height`
/// level of a BC image is block aligned: offsets on 4-texel boundaries, sizes multiples of 4 unless
/// they reach the level's edge.
[[nodiscard]] constexpr bool BcRegionAligned(u32 x, u32 y, u32 width, u32 height, u32 level_width,
                                             u32 level_height) {
    return x % 4 == 0 && y % 4 == 0 && (width % 4 == 0 || x + width == level_width) &&
           (height % 4 == 0 || y + height == level_height);
}

} // namespace Vulkan::Nvn

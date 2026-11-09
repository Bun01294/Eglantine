// SPDX-FileCopyrightText: Copyright 2026 Vortex727 (FUKKO)
// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
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

/// EDEN_NX2_NVN_BC7_QUALITY: which BC7 modes the encoder tries. 0: modes 4, 5 and 6 (one
/// subset); 1: also the two-subset modes 1, 3 and 7; 2: also the three-subset modes 0 and 2.
inline constexpr u32 BC7_QUALITY_MAX = 2;
inline constexpr u32 BC7_QUALITY_DEFAULT = 1;

/// `EDEN_NX2_NVN_BC7_QUALITY` (`0`..`BC7_QUALITY_MAX`; null or anything else: the default).
[[nodiscard]] u32 ParseBc7Quality(const char* text);

/// Compresses RGBA8 texels to BC1 or BC3 blocks on the GPU (the emulated path's `bc_encoder.comp`,
/// the shader its `BcEncoderPass` runs), or to BC7 (`bc7_encoder.comp`, ported from DirectXTex),
/// for ASTC stand-ins held as BC (`HostFormat::bc_standin`). The replay records the dispatches
/// into its own command buffers with push descriptors, as `AstcGpuDecoder` does.
class BcGpuEncoder {
public:
    /// Format of the storage views the encoder reads (and the ASTC decoder writes).
    static constexpr VkFormat SOURCE_FORMAT = VK_FORMAT_A8B8G8R8_UNORM_PACK32;

    /// True when the device can run the encoder (push descriptors, a storable RGBA8 format) and
    /// sample and copy into BC1/BC3 images.
    [[nodiscard]] static bool Supported(const Device& device);

    /// True when, beyond Supported, the device can sample and copy into BC7 images and push the
    /// BC7 encoder's three descriptors.
    [[nodiscard]] static bool SupportsBc7(const Device& device);

    /// `bc7`: also build the BC7 pipelines (RecordBc7).
    explicit BcGpuEncoder(const Device& device, bool bc7 = false);
    ~BcGpuEncoder();

    /// Records one dispatch into `cmd` (outside a render pass): the first `layers` layers of
    /// `source` (a 2D array storage view in GENERAL layout), `width` x `height` texels each, become
    /// blocks row-major, layer after layer, from the start of `output` (`output_size` bytes).
    void Record(vk::CommandBuffer cmd, VkImageView source, u32 width, u32 height, u32 layers,
                bool bc3, VkBuffer output, u64 output_size) const;

    /// Record for BC7 blocks: the passes `quality` asks for (BC7_QUALITY_MAX), the best mode of
    /// each block kept in `modes` (8 bytes per block, BcScratch::modes) between them.
    void RecordBc7(vk::CommandBuffer cmd, VkImageView source, u32 width, u32 height, u32 layers,
                   u32 quality, VkBuffer modes, u64 modes_size, VkBuffer output,
                   u64 output_size) const;

    /// Whether the BC7 pipelines were built (RecordBc7 can be used).
    [[nodiscard]] bool HasBc7() const {
        return static_cast<bool>(bc7_pipelines[0]);
    }

private:
    const Device& device;
    vk::DescriptorSetLayout set_layout;
    vk::PipelineLayout pipeline_layout;
    vk::ShaderModule module;
    vk::Pipeline pipeline;

    vk::DescriptorSetLayout bc7_set_layout;
    vk::PipelineLayout bc7_pipeline_layout;
    vk::ShaderModule bc7_module;
    std::array<vk::Pipeline, 3> bc7_pipelines; ///< by bc7_encoder.comp STAGE
};

/// The RGBA8 image a BC stand-in upload decodes (or copies) its texels into before the encoder
/// reads them, and the buffer the blocks go to before they are copied into the stand-in. One is
/// reused for every upload; callers keep it alive through the flight that records into it.
struct BcScratch {
    vk::Image image;   ///< SOURCE_FORMAT, 1 level, STORAGE | TRANSFER_DST
    vk::ImageView view; ///< 2D array storage view of every layer
    vk::Buffer blocks; ///< device-local, room for the BC3 blocks of the whole image
    vk::Buffer modes;  ///< device-local, 8 bytes per block for the BC7 passes; null unless BC7
    u32 width{};
    u32 height{};
    u32 layers{};

    [[nodiscard]] bool Fits(u32 w, u32 h, u32 l, bool bc7 = false) const {
        return w <= width && h <= height && l <= layers && (!bc7 || modes);
    }
};

/// Creates a scratch of `width` x `height` x `layers` texels (`bc7`: with `modes`). Throws
/// vk::Exception.
[[nodiscard]] std::shared_ptr<BcScratch> MakeBcScratch(const Device& device,
                                                       const MemoryAllocator& allocator, u32 width,
                                                       u32 height, u32 layers, bool bc7 = false);

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

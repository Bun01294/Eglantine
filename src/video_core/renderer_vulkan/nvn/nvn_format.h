// SPDX-FileCopyrightText: Copyright 2026 Vortex727 (FUKKO)
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>

#include "common/common_types.h"
#include "video_core/surface.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
class Device;
}

namespace Vulkan::Nvn {

/// The `VideoCore::Surface::PixelFormat` an NVNformat value maps to, or nullopt when the NVN format
/// has no counterpart (three-component formats, *_I2F integer-to-float reads, ...).
[[nodiscard]] std::optional<VideoCore::Surface::PixelFormat> NvnToPixelFormat(s32 nvn_format);

struct HostFormat {
    VkFormat format{VK_FORMAT_UNDEFINED};
    VideoCore::Surface::PixelFormat pixel_format{};
    bool attachable{};
    bool storage{};
    /// The device cannot hold the format natively (ASTC on a desktop GPU, BCn on mobile ...) and
    /// `format` is an uncompressed stand-in. The replay decodes the guest's blocks on the CPU when
    /// it uploads the texture (nvn_upload.cpp); nothing else can read or write the texture's guest
    /// bytes through the image (a texture-to-buffer copy would have to re-encode).
    bool needs_transcode{};
    bool depth{};
    bool stencil{};
    /// Depth/stencil whose host texel layout is not the guest's (D24S8, D32S8): copies between
    /// two images and clears are fine, guest bytes cannot be uploaded as they are.
    bool guest_layout_differs{};
    /// The format of the storage-image views of a texture whose own format has no storage support
    /// (VK_FORMAT_UNDEFINED otherwise, or when the device cannot store that format either). NVN
    /// images of an sRGB texture read and write the texel bits as they are, with no sRGB
    /// conversion (the same-size UNORM format); Vulkan expresses that as a UNORM view of an image
    /// created with STORAGE usage and VK_IMAGE_CREATE_EXTENDED_USAGE_BIT.
    VkFormat storage_view{VK_FORMAT_UNDEFINED};
    /// The format cannot be stored but an NVN texture view of another format of the same texel
    /// size can bind its storage as an image (OoT binds an R32UI view of an RGB9E5 texture and
    /// packs the shared exponent itself). Such textures are created with STORAGE usage and
    /// VK_IMAGE_CREATE_EXTENDED_USAGE_BIT, as the views need it; set when the unsigned integer
    /// format of the texel size (`SameSizeUintFormat`) can be stored by the device.
    bool storage_alias{};
};

/// The unsigned integer colour format with `bytes` per texel (R8/R16/R32/R32G32/R32G32B32A32
/// UINT), VK_FORMAT_UNDEFINED for any other size. Every uncompressed colour format of that size
/// is in its Vulkan compatibility class.
[[nodiscard]] VkFormat SameSizeUintFormat(u32 bytes);

/// The UNORM format with the bits of an 8-bit-per-channel sRGB colour format, VK_FORMAT_UNDEFINED
/// for every other format.
[[nodiscard]] VkFormat SrgbStorageViewFormat(VkFormat format);

/// NVNformat -> PixelFormat -> `MaxwellToVK::SurfaceFormat`. Returns nullopt when unsupported.
[[nodiscard]] std::optional<HostFormat> ResolveHostFormat(const Device& device, s32 nvn_format);

} // namespace Vulkan::Nvn

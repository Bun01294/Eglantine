// SPDX-FileCopyrightText: Copyright 2026 Vortex727 (FUKKO)
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <optional>

#include "common/common_types.h"
#include "common/settings_enums.h"
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
    /// An ASTC stand-in held as BC1/BC3/BC7 (`format`) instead of RGBA8 (AstcStandin): the replay
    /// decodes the blocks to RGBA8 in a scratch image and re-encodes them (nvn_bc_encode.h). Set
    /// per storage by the texture creation (ToBcStandin), never by ResolveHostFormat.
    bool bc_standin{};
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

/// What an ASTC stand-in holds on a device without ASTC: the decoded RGBA8 texels, or those
/// re-encoded to BC1 / BC3 / BC7 (an eighth / a quarter / a quarter of the RGBA8 memory).
enum class AstcStandin : u8 { Rgba8, Bc1, Bc3, Bc7 };

/// `EDEN_NX2_NVN_ASTC_STANDIN` (`rgba8`, `bc1`, `bc3`, `bc7`; null or anything else: not set)
/// over the astc_recompression setting (which has no BC7).
[[nodiscard]] AstcStandin ChooseAstcStandin(const char* environment,
                                            Settings::AstcRecompression setting);

[[nodiscard]] const char* AstcStandinName(AstcStandin standin);

/// `host` (an RGBA8 ASTC stand-in) held as `standin`'s BC format, with the sRGB-ness of the ASTC
/// format; `host` unchanged when it is no ASTC stand-in or `standin` is Rgba8.
[[nodiscard]] HostFormat ToBcStandin(const HostFormat& host, AstcStandin standin);

/// What a BC stand-in holds (by its format), Rgba8 for any other host format.
[[nodiscard]] AstcStandin BcStandinOf(const HostFormat& host);

/// Bytes per 4x4 block of a BC stand-in (8 for BC1, 16 for BC3 and BC7), 0 for any other format.
[[nodiscard]] u32 BcStandinBlockBytes(const HostFormat& host);

/// Whether `format` can view a BC stand-in of format `standin` (the same BC format, sRGB or not).
[[nodiscard]] bool ViewsBcStandin(VkFormat format, VkFormat standin);

} // namespace Vulkan::Nvn

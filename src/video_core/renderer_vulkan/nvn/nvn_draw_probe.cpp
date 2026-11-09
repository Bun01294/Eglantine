// SPDX-FileCopyrightText: Copyright 2026 Vortex727 (FUKKO)
// SPDX-License-Identifier: GPL-3.0-or-later

// The replay side of the debug server draw methods `nvn_draws`, `nvn_draw_skip` and
// `nvn_draw_dump` (video_core/nvn_draw_probe.h).

#include <algorithm>
#include <cstring>
#include <string>

#include <fmt/format.h>

#include "common/fs/file.h"
#include "common/logging.h"
#include "video_core/nvn_draw_probe.h"
#include "video_core/renderer_vulkan/nvn/nvn_draw.h"

namespace Vulkan::Nvn {

namespace {

namespace Probe = VideoCommon::NvnDrawProbe;

constexpr const char* StageNames[Probe::StageCount] = {
    "vertex", "fragment", "geometry", "tess_control", "tess_eval", "compute", "task", "mesh"};

void AppendHex(std::string& text, const u8* data, u64 bytes, u64 per_line) {
    for (u64 offset = 0; offset < bytes; offset += per_line) {
        text += fmt::format("  {:06x}:", offset);
        for (u64 j = offset; j < (std::min)(offset + per_line, bytes); ++j) {
            text += fmt::format(" {:02x}", data[j]);
        }
        text += '\n';
    }
}

u32 FloatComponents(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R32_SFLOAT:
        return 1;
    case VK_FORMAT_R32G32_SFLOAT:
        return 2;
    case VK_FORMAT_R32G32B32_SFLOAT:
        return 3;
    case VK_FORMAT_R32G32B32A32_SFLOAT:
        return 4;
    default:
        return 0;
    }
}

} // Anonymous namespace

bool DrawContext::ProbeDraw(u32 mode, u32 element_count, const IndexRange* index,
                            const std::array<BufferRef, MAX_VERTEX_STREAMS>& vertex,
                            u32 used_streams, bool mesh) {
    Probe::DrawInfo info;
    for (u32 stage = 0; stage < Probe::StageCount; ++stage) {
        info.hashes[stage] = stages[stage].program_serial != 0 ? ProfStageHash(stage) : 0;
    }
    info.mode = mode;
    info.count = element_count;
    info.indexed = index != nullptr;
    info.mesh = mesh;
    info.indirect = element_count == 0 && !mesh;
    // The same test PrepareDraw applies when it binds a staged copy.
    const auto staged = [&](u64 address, u64 size, const Pool* pool) {
        if (staged_vertex.empty() || pass_shader_writes || pool == nullptr ||
            pool->backing != Pool::Backing::Imported) {
            return false;
        }
        const auto it = staged_vertex.find(address);
        return it != staged_vertex.end() && it->second.size >= size;
    };
    for (u32 s = 0; s < MAX_VERTEX_STREAMS && info.stream_count < Probe::MaxStreams; ++s) {
        if (!((used_streams >> s) & 1)) {
            continue;
        }
        const VC::BufferRange& range = vertex_buffers[s];
        Probe::Stream& out = info.streams[info.stream_count++];
        out.slot = s;
        out.address = range.address;
        out.size = range.size;
        if (vertex[s].pool != nullptr) {
            out.pool_offset = vertex[s].pool_offset;
            out.pool_address = range.address - vertex[s].pool_offset;
        }
        out.staged = staged(range.address, range.size, vertex[s].pool);
    }
    if (index) {
        info.index_address = index->address;
        info.index_bytes = index->bytes;
        info.index_type = index->type;
        info.index_staged = !staged_vertex.empty() && !pass_shader_writes &&
                            staged_vertex.contains(index->address) &&
                            staged_vertex.find(index->address)->second.size >= index->bytes;
    }
    const bool skip = Probe::OnDraw(info);
    const auto path = Probe::DumpRequested(info);
    if (!path) {
        return skip;
    }

    std::string text = "NVN draw dump\nhashes:";
    for (u32 stage = 0; stage < Probe::StageCount; ++stage) {
        if (info.hashes[stage] != 0) {
            text += fmt::format(" {}={:016x}", StageNames[stage], info.hashes[stage]);
        }
    }
    text += fmt::format("\nmode={} count={} indexed={} indirect={} mesh={} skipped={}\n", mode,
                        element_count, info.indexed, info.indirect, mesh, skip);
    text += "Bytes are the CPU view of guest memory when the draw was recorded (the GPU may read a "
            "staged copy or a VRAM mirror instead, see staged=; it runs later).\n";
    for (u32 b = 0; b < vertex_input.binding_count && b < MAX_VERTEX_STREAMS; ++b) {
        const auto& binding = vertex_input.bindings[b];
        text += fmt::format("binding {} stride {} rate {} divisor {}\n", binding.binding,
                            binding.stride, static_cast<u32>(binding.inputRate), binding.divisor);
    }
    for (u32 a = 0; a < vertex_input.attribute_count && a < MAX_VERTEX_ATTRIBUTES; ++a) {
        const auto& attribute = vertex_input.attributes[a];
        text += fmt::format("attribute location {} binding {} VkFormat {} offset {}\n",
                            attribute.location, attribute.binding,
                            static_cast<s32>(attribute.format), attribute.offset);
    }
    u32 max_stride = 0;
    for (u32 i = 0; i < info.stream_count; ++i) {
        const Probe::Stream& stream = info.streams[i];
        u32 stride = 0;
        for (u32 b = 0; b < vertex_input.binding_count && b < MAX_VERTEX_STREAMS; ++b) {
            if (vertex_input.bindings[b].binding == stream.slot) {
                stride = vertex_input.bindings[b].stride;
            }
        }
        max_stride = (std::max)(max_stride, stride);
        const u64 bytes =
            (std::min)(stream.size, stride != 0 ? u64{stride} * 256 : u64{4096});
        text += fmt::format("stream {} guest {:#x} size {:#x} pool {:#x} + {:#x} staged={} "
                            "stride {}: first {} bytes\n",
                            stream.slot, stream.address, stream.size, stream.pool_address,
                            stream.pool_offset, stream.staged, stride, bytes);
        const u8* data = impl.GuestPointer(stream.address, bytes);
        if (data == nullptr) {
            text += "  (not CPU-readable)\n";
            continue;
        }
        AppendHex(text, data, bytes, 32);
        // 32-bit float attributes of this stream, first 32 vertices.
        for (u32 a = 0; a < vertex_input.attribute_count && a < MAX_VERTEX_ATTRIBUTES; ++a) {
            const auto& attribute = vertex_input.attributes[a];
            const u32 components = FloatComponents(attribute.format);
            if (attribute.binding != stream.slot || components == 0 || stride == 0) {
                continue;
            }
            text += fmt::format("  location {} as float{}:\n", attribute.location, components);
            for (u32 v = 0; v < 32; ++v) {
                const u64 at = u64{v} * stride + attribute.offset;
                if (at + u64{components} * 4 > bytes) {
                    break;
                }
                text += fmt::format("    v{}:", v);
                for (u32 c = 0; c < components; ++c) {
                    f32 value;
                    std::memcpy(&value, data + at + c * 4, sizeof(value));
                    text += fmt::format(" {}", value);
                }
                text += '\n';
            }
        }
    }
    if (index) {
        const u32 size = index->type == 0 ? 1 : (index->type == 1 ? 2 : 4);
        text += fmt::format("index guest {:#x} bytes {:#x} type {} staged={}", index->address,
                            index->bytes, index->type, info.index_staged);
        const u8* data = impl.GuestPointer(index->address, index->bytes);
        if (data != nullptr && index->bytes >= size) {
            u32 lo = ~0u;
            u32 hi = 0;
            for (u64 i = 0; i + size <= index->bytes; i += size) {
                u32 value = 0;
                std::memcpy(&value, data + i, size);
                lo = (std::min)(lo, value);
                hi = (std::max)(hi, value);
            }
            text += fmt::format(" min {} max {} (bytes needed per stream at max stride {}: {})\n",
                                lo, hi, max_stride, u64{hi + 1} * max_stride);
        } else {
            text += " (not CPU-readable)\n";
        }
    }
    for (u32 stage = 0; stage < Probe::StageCount; ++stage) {
        if (stages[stage].program_serial == 0) {
            continue;
        }
        for (size_t slot = 0; slot < stages[stage].uniform.size(); ++slot) {
            const BufferBinding& binding = stages[stage].uniform[slot];
            if (binding.address == 0) {
                continue;
            }
            const u64 bytes = (std::min)(binding.size, u64{256});
            text += fmt::format("{} uniform {} {:#x} size {:#x}\n", StageNames[stage], slot,
                                binding.address, binding.size);
            if (const u8* data = impl.GuestPointer(binding.address, bytes)) {
                AppendHex(text, data, bytes, 32);
            } else {
                text += "  (not CPU-readable)\n";
            }
        }
        for (size_t slot = 0; slot < stages[stage].storage.size(); ++slot) {
            const BufferBinding& binding = stages[stage].storage[slot];
            if (binding.address == 0) {
                continue;
            }
            const u64 bytes = (std::min)(binding.size, u64{64});
            text += fmt::format("{} storage {} {:#x} size {:#x}\n", StageNames[stage], slot,
                                binding.address, binding.size);
            if (const u8* data = impl.GuestPointer(binding.address, bytes)) {
                AppendHex(text, data, bytes, 32);
            } else {
                text += "  (not CPU-readable)\n";
            }
        }
    }
    std::string error;
    if (Common::FS::WriteStringToFile(*path, Common::FS::FileType::BinaryFile, text) !=
        text.size()) {
        error = "cannot write " + *path;
    }
    LOG_INFO(Render_Vulkan, "[diag-draw] dump of a draw with {:016x}: {}",
             info.hashes[mesh ? 7 : 0], error.empty() ? *path : error);
    Probe::DumpDone(error.empty() ? *path : std::string{}, std::move(error));
    return skip;
}

} // namespace Vulkan::Nvn

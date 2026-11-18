// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include "core/file_sys/music_extract.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <fmt/format.h>

#include "common/cityhash.h"
#include "common/fs/fs_util.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "core/file_sys/nup_container.h"
#include "core/file_sys/nup_writer.h"
#include "core/file_sys/vfs/vfs_real.h"

#ifdef EGLANTINE_HAS_FLAC
extern "C" {
#include <FLAC/metadata.h>
#include <FLAC/stream_encoder.h>
}
#endif

#include <opus.h>

namespace FileSys::Music {

namespace {

constexpr std::array MusicPrefixes{"BGM_", "Dm_", "ExampleMelody_"};

struct Entry {
    std::string name;
    VirtualFile file;
};

struct Collected {
    std::vector<Entry> entries;
    std::string game_title;
    // NUP/package cover art (PNG or JPEG bytes), embedded into every track.
    std::vector<u8> icon;
};

std::string Lower(std::string_view value) {
    std::string out{value};
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

bool EndsWithCI(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() &&
           Lower(value.substr(value.size() - suffix.size())) == Lower(suffix);
}

bool IsNupPath(const std::string& path) {
    return EndsWithCI(path, ".nup");
}

bool IsMusicName(const std::string& name) {
    if (!EndsWithCI(name, ".bwav")) {
        return false;
    }
    return std::any_of(MusicPrefixes.begin(), MusicPrefixes.end(),
                       [&name](std::string_view prefix) {
                           return Lower(name).starts_with(Lower(prefix));
                       });
}

VirtualDir FindSubdirCI(const VirtualDir& dir, std::string_view name) {
    if (dir == nullptr) {
        return nullptr;
    }
    if (const auto exact = dir->GetSubdirectory(std::string{name}); exact != nullptr) {
        return exact;
    }
    const auto lower = Lower(name);
    for (const auto& sub : dir->GetSubdirectories()) {
        if (Lower(sub->GetName()) == lower) {
            return sub;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------------------------
// Loose folder dumps
// -------------------------------------------------------------------------------------

std::optional<std::filesystem::path> FindChildCI(const std::filesystem::path& parent,
                                                 std::string_view name) {
    std::error_code ec;
    if (!std::filesystem::is_directory(parent, ec)) {
        return std::nullopt;
    }
    const auto lower = Lower(name);
    for (const auto& entry : std::filesystem::directory_iterator(parent, ec)) {
        if (entry.is_directory(ec) &&
            Lower(Common::FS::PathToUTF8String(entry.path().filename())) == lower) {
            return entry.path();
        }
    }
    return std::nullopt;
}

std::optional<std::filesystem::path> StreamDirOfRomfs(const std::filesystem::path& romfs) {
    const auto sound = FindChildCI(romfs, "Sound");
    if (!sound) {
        return std::nullopt;
    }
    const auto resource = FindChildCI(*sound, "Resource");
    if (!resource) {
        return std::nullopt;
    }
    return FindChildCI(*resource, "Stream");
}

void CollectFromStreamDir(const VirtualDir& stream_dir, std::map<std::string, Entry>& out) {
    if (stream_dir == nullptr) {
        return;
    }
    for (const auto& file : stream_dir->GetFiles()) {
        if (IsMusicName(file->GetName())) {
            out.insert_or_assign(file->GetName(), Entry{file->GetName(), file});
        }
    }
}

bool CollectLoose(const std::filesystem::path& dump_root, RealVfsFilesystem& vfs,
                  Collected& collected) {
    // base/romfs (or a bare romfs/), then every updates/<version>/romfs in ascending
    // order so newer trees override older ones by file name.
    std::vector<std::filesystem::path> romfs_dirs;
    if (const auto base = FindChildCI(dump_root, "base")) {
        if (const auto romfs = FindChildCI(*base, "romfs")) {
            romfs_dirs.push_back(*romfs);
        }
    } else if (const auto romfs = FindChildCI(dump_root, "romfs")) {
        romfs_dirs.push_back(*romfs);
    }
    std::vector<std::pair<u64, std::filesystem::path>> updates;
    if (const auto updates_dir = FindChildCI(dump_root, "updates")) {
        std::error_code ec;
        for (const auto& entry : std::filesystem::directory_iterator(*updates_dir, ec)) {
            u64 version{};
            const auto name = Common::FS::PathToUTF8String(entry.path().filename());
            const auto [ptr, err] =
                std::from_chars(name.data(), name.data() + name.size(), version);
            if (err != std::errc{} || ptr != name.data() + name.size() || !entry.is_directory(ec)) {
                continue;
            }
            updates.emplace_back(version, entry.path());
        }
    }
    std::sort(updates.begin(), updates.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    for (const auto& [version, dir] : updates) {
        if (const auto romfs = FindChildCI(dir, "romfs")) {
            romfs_dirs.push_back(*romfs);
        }
    }
    if (romfs_dirs.empty()) {
        return false;
    }

    std::map<std::string, Entry> merged;
    for (const auto& romfs : romfs_dirs) {
        if (const auto stream = StreamDirOfRomfs(romfs)) {
            CollectFromStreamDir(vfs.OpenDirectory(Common::FS::PathToUTF8String(*stream)),
                                 merged);
        }
    }
    if (merged.empty()) {
        return false;
    }
    collected.entries.reserve(merged.size());
    for (auto& [name, entry] : merged) {
        collected.entries.push_back(std::move(entry));
    }
    const auto probed = Nup::ProbeMetadata(dump_root);
    collected.game_title = !probed.title_name.empty()
                               ? probed.title_name
                               : Common::FS::PathToUTF8String(dump_root.filename());
    collected.icon = probed.icon;
    return true;
}

// ---------------------------------------------------------------------------------------------
// NUP archives
// -------------------------------------------------------------------------------------

bool CollectNup(const std::string& game_path, RealVfsFilesystem& vfs, Collected& collected,
                std::string& error) {
    const auto file = vfs.OpenFile(game_path);
    if (file == nullptr || !NupContainer::IsNupFile(file)) {
        error = "not a NUP archive";
        return false;
    }
    NupContainer container{file};
    if (container.GetStatus() != Loader::ResultStatus::Success) {
        error = fmt::format("NUP container error {} ({})",
                            static_cast<int>(container.GetStatus()),
                            Loader::GetResultStatusString(container.GetStatus()));
        return false;
    }
    if (container.GetPackages().empty()) {
        error = "NUP container has no packages";
        return false;
    }

    // The romfs the game would see: the base package overlaid with the newest update,
    // or the update alone when the container is a standalone update.
    VirtualDir merged;
    if (const auto base = container.FindBasePackage()) {
        merged = container.GetRomFSDir(*base);
    }
    if (const auto update = container.FindBestUpdatePackage()) {
        const auto update_romfs = container.GetRomFSDir(*update);
        if (merged != nullptr && update_romfs != nullptr) {
            merged = NupContainer::MergeShadowDirs(merged, update_romfs);
        } else if (update_romfs != nullptr) {
            merged = update_romfs;
        }
    }
    if (merged == nullptr) {
        error = "NUP container has no RomFS";
        return false;
    }

    const auto sound = FindSubdirCI(merged, "Sound");
    const auto resource = FindSubdirCI(sound, "Resource");
    const auto stream = FindSubdirCI(resource, "Stream");
    if (stream == nullptr) {
        error = "no Sound/Resource/Stream folder in the RomFS";
        return false;
    }
    std::map<std::string, Entry> merged_files;
    CollectFromStreamDir(stream, merged_files);
    if (merged_files.empty()) {
        error = "no music streams in the RomFS";
        return false;
    }
    collected.entries.reserve(merged_files.size());
    for (auto& [name, entry] : merged_files) {
        collected.entries.push_back(std::move(entry));
    }
    for (const auto& package : container.GetPackages()) {
        if (!package.title_name.empty()) {
            collected.game_title = package.title_name;
            break;
        }
    }
    if (collected.game_title.empty()) {
        collected.game_title = std::string{Common::FS::GetFilename(game_path)};
    }
    if (const auto base = container.FindBasePackage()) {
        collected.icon = container.GetPackages()[*base].icon;
    }
    if (collected.icon.empty()) {
        for (const auto& package : container.GetPackages()) {
            if (!package.icon.empty()) {
                collected.icon = package.icon;
                break;
            }
        }
    }
    return true;
}

bool Collect(const std::string& game_path, RealVfsFilesystem& vfs, Collected& collected,
             std::string& error) {
    if (IsNupPath(game_path)) {
        return CollectNup(game_path, vfs, collected, error);
    }
    const auto root = Nup::DumpRootFromMain(std::filesystem::path{game_path});
    if (!root) {
        error = "not a NUP archive or a loose dump";
        return false;
    }
    if (!CollectLoose(*root, vfs, collected)) {
        error = "no music streams in the dump (looked for BGM_/Dm_/ExampleMelody_ .bwav "
                "under Sound/Resource/Stream)";
    }
    return !collected.entries.empty();
}

// ---------------------------------------------------------------------------------------------
// Track classification
// -------------------------------------------------------------------------------------

std::string KindOf(const std::string& name) {
    const std::string stem = EndsWithCI(name, ".bwav") ? name.substr(0, name.size() - 5) : name;
    if (stem.starts_with("Dm_Title")) {
        return "title";
    }
    if (stem.starts_with("Dm_")) {
        return "cutscene";
    }
    // Layers the game mixes live: close/far ambience, vocal or noise stems, final-lap variants.
    for (const std::string_view marker : {"_Close_", "_Far_", "_Noise", "_Pad", "_Vocal_", "_FL"}) {
        if (stem.find(marker) != std::string::npos) {
            return "stems";
        }
    }
    // Sequenced parts: intros, loops, jingles, outros and A/B/C sections.
    static constexpr std::array SegmentMarkers{
        "_Intro",  "_Loop", "_Jingle", "_Outro", "_OneShot", "_Prerace",
    };
    for (const std::string_view marker : SegmentMarkers) {
        if (stem.find(marker) != std::string::npos) {
            return "segments";
        }
    }
    if (stem.size() >= 2 && stem.back() >= 'A' && stem.back() <= 'C' &&
        stem[stem.size() - 2] == '_') {
        return "segments";
    }
    return "bgm";
}

// ---------------------------------------------------------------------------------------------
// BWAV (NintendoWare wave), laid out as vgmstream reads it:
//   0x00 "BWAV", 0x04 BOM, 0x0e channel count, then a 0x4C block per channel from 0x10.
// ---------------------------------------------------------------------------------------------

constexpr std::size_t ChannelBlockSize = 0x4C;
constexpr u16 CodecPcm16 = 0;
constexpr u16 CodecDsp = 1;
constexpr u16 CodecNxOpus = 2;

struct BwavChannel {
    u16 codec{};
    s32 sample_rate{};
    s32 num_samples{};
    std::array<s16, 16> coefs{};
    u32 data_start{};
    s32 loop_end{-1};
    s32 loop_start{};
    s16 hist1{};
    s16 hist2{};
};

struct BwavInfo {
    u16 channels{};
    u32 interleave{};
    std::vector<BwavChannel> channel;
};

u16 Read16(std::span<const u8> data, std::size_t offset) {
    if (offset + 2 > data.size()) {
        return 0;
    }
    u16 value;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

s32 ReadS32(std::span<const u8> data, std::size_t offset) {
    if (offset + 4 > data.size()) {
        return 0;
    }
    s32 value;
    std::memcpy(&value, data.data() + offset, sizeof(value));
    return value;
}

u32 ReadU32(std::span<const u8> data, std::size_t offset) {
    return static_cast<u32>(ReadS32(data, offset));
}

bool ParseBwav(std::span<const u8> data, BwavInfo& info, std::string& note) {
    if (data.size() < 0x10) {
        note = "truncated header";
        return false;
    }
    if (std::memcmp(data.data(), "BWAV", 4) != 0) {
        note = "missing BWAV magic";
        return false;
    }
    info.channels = Read16(data, 0x0e);
    if (info.channels == 0 || info.channels > 8) {
        note = fmt::format("bad channel count {}", info.channels);
        return false;
    }
    info.channel.resize(info.channels);
    for (u16 ch = 0; ch < info.channels; ++ch) {
        const std::size_t base = 0x10 + ChannelBlockSize * ch;
        if (base + ChannelBlockSize > data.size()) {
            note = "truncated channel block";
            return false;
        }
        BwavChannel& channel = info.channel[ch];
        channel.codec = Read16(data, base + 0x00);
        channel.sample_rate = ReadS32(data, base + 0x04);
        channel.num_samples = ReadS32(data, base + 0x0c);
        channel.data_start = ReadU32(data, base + 0x34);
        channel.loop_end = ReadS32(data, base + 0x3c);
        channel.loop_start = ReadS32(data, base + 0x40);
        channel.hist1 = static_cast<s16>(Read16(data, base + 0x46));
        channel.hist2 = static_cast<s16>(Read16(data, base + 0x48));
        for (std::size_t i = 0; i < channel.coefs.size(); ++i) {
            channel.coefs[i] = static_cast<s16>(Read16(data, base + 0x10 + i * 2));
        }
        if (channel.codec != CodecPcm16 && channel.codec != CodecDsp &&
            channel.codec != CodecNxOpus) {
            note = fmt::format("unknown codec {}", channel.codec);
            return false;
        }
        if (channel.sample_rate <= 0 || channel.num_samples < 0) {
            note = "bad rate or sample count";
            return false;
        }
    }
    if (info.channels > 1) {
        // vgmstream: the second channel's data offset sits at 0x8C; the per-channel stride
        // is the difference to the first channel's data start.
        const u32 second_channel = ReadU32(data, 0x8C);
        info.interleave = second_channel > info.channel[0].data_start
                              ? second_channel - info.channel[0].data_start
                              : 0;
    }
    return true;
}

s16 Clamp16(s32 sample) {
    return static_cast<s16>(std::clamp(sample, -0x8000, 0x7FFF));
}

// Byte offset of a channel's data, in the block-interleaved layout vgmstream reads:
// blocks of `interleave` bytes, cycling through the channels. When one block holds the
// whole channel (the usual case in these dumps) it degenerates to a contiguous read.
std::size_t InterleavedOffset(u16 channels, u32 interleave, u16 channel,
                              std::size_t byte_in_channel) {
    if (interleave == 0 || channels <= 1) {
        return byte_in_channel;
    }
    const std::size_t block = byte_in_channel / interleave;
    const std::size_t within = byte_in_channel % interleave;
    return block * interleave * channels + static_cast<std::size_t>(channel) * interleave +
           within;
}

// One channel of Nintendo DSP 4-bit ADPCM. 8-byte frames: a header byte (scale in the
// low nibble, coefficient index in the high one) followed by 14 samples as signed
// nibbles, high nibble first, decoded exactly as vgmstream does.
// Channel positions follow vgmstream: every channel is relative to channel 0's data
// start (the per-channel +0x34 fields hold the same absolute offsets, so mixing them
// with the interleave stride would double-count it).
void DecodeDspChannel(std::span<const u8> data, const BwavChannel& channel, u32 base_start,
                      u16 channel_index, u16 channels, u32 interleave, std::span<s16> out) {
    s32 hist1 = channel.hist1;
    s32 hist2 = channel.hist2;
    std::size_t done = 0;
    while (done < out.size()) {
        const std::size_t offset =
            base_start +
            InterleavedOffset(channels, interleave, channel_index, (done / 14) * 8);
        if (offset + 8 > data.size()) {
            break;
        }
        const u8 header = data[offset];
        const s32 scale = 1 << (header & 0xf);
        u32 coef_index = (header >> 4) & 0xf;
        if (coef_index > 7) {
            coef_index = 7;
        }
        const s32 coef1 = channel.coefs[coef_index * 2 + 0];
        const s32 coef2 = channel.coefs[coef_index * 2 + 1];
        for (std::size_t i = 0; i < 7 && done < out.size(); ++i) {
            const u8 nibbles = data[offset + 1 + i];
            for (const u8 nibble : {u8(nibbles >> 4), u8(nibbles & 0xf)}) {
                if (done >= out.size()) {
                    break;
                }
                const s32 code = nibble < 8 ? nibble : nibble - 16;
                const s64 prediction = (s64{code} * scale << 11) + 1024 +
                                       s64{coef1} * hist1 + s64{coef2} * hist2;
                const s16 sample = Clamp16(static_cast<s32>(prediction >> 11));
                out[done++] = sample;
                hist2 = hist1;
                hist1 = sample;
            }
        }
    }
    // Zero-fill anything the stored data does not cover.
    std::fill(out.begin() + done, out.end(), s16{});
}

void DecodePcm16Channel(std::span<const u8> data, const BwavChannel& channel, u32 base_start,
                        u16 channel_index, u16 channels, u32 interleave, std::span<s16> out) {
    std::size_t done = 0;
    while (done < out.size()) {
        const std::size_t offset =
            base_start + InterleavedOffset(channels, interleave, channel_index, done * 2);
        if (offset + 2 > data.size()) {
            break;
        }
        u16 value;
        std::memcpy(&value, data.data() + offset, sizeof(value));
        out[done++] = static_cast<s16>(value);
    }
    std::fill(out.begin() + done, out.end(), s16{});
}

// ---------------------------------------------------------------------------------------------
// NXOpus (Nintendo Switch Opus, BWAV codec 2), laid out as vgmstream reads it. Every
// BWAV channel block points at its own Opus stream: a 0x80000001 "basic info" chunk,
// then a 0x80000004 "data info" chunk whose region is a series of packets, each with a
// big-endian size in an 8-byte header.
// ---------------------------------------------------------------------------------------------

struct NxOpusStream {
    u16 channels{};
    u32 sample_rate{};
    u32 data_start{};
    u32 data_size{};
    u16 skip{};
};

u32 ReadU32BE(std::span<const u8> data, std::size_t offset) {
    if (offset + 4 > data.size()) {
        return 0;
    }
    return (u32{data[offset]} << 24) | (u32{data[offset + 1]} << 16) |
           (u32{data[offset + 2]} << 8) | u32{data[offset + 3]};
}

bool ParseNxOpus(std::span<const u8> data, u32 stream_start, NxOpusStream& stream,
                 std::string& note) {
    if (stream_start + 0x28 > data.size()) {
        note = "NXOpus header out of range";
        return false;
    }
    if (ReadU32(data, stream_start + 0x00) != 0x80000001) {
        note = "NXOpus: missing basic info chunk";
        return false;
    }
    stream.channels = data[stream_start + 0x09];
    stream.sample_rate = ReadU32(data, stream_start + 0x0c);
    const u32 data_offset = ReadU32(data, stream_start + 0x10);
    stream.skip = Read16(data, stream_start + 0x1c);
    if (stream.channels == 0 || stream.channels > 8) {
        note = fmt::format("NXOpus: bad channel count {}", stream.channels);
        return false;
    }
    // Opus decodes at 48 kHz; games that stored another rate still play 48 kHz audio.
    if (stream.sample_rate != 48000) {
        stream.sample_rate = 48000;
    }
    const u32 data_chunk = stream_start + data_offset;
    if (data_offset == 0 || data_chunk + 8 > data.size() ||
        ReadU32(data, data_chunk) != 0x80000004) {
        note = "NXOpus: missing data info chunk";
        return false;
    }
    stream.data_start = data_chunk + 8;
    stream.data_size = ReadU32(data, data_chunk + 4);
    if (stream.data_start + stream.data_size > data.size()) {
        note = "NXOpus: packet data out of range";
        return false;
    }
    return true;
}

bool DecodeNxOpus(std::span<const u8> data, const NxOpusStream& stream, std::span<s16> out,
                 std::string& note) {
    int opus_error = OPUS_OK;
    std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> decoder{
        opus_decoder_create(stream.sample_rate, stream.channels, &opus_error),
        &opus_decoder_destroy};
    if (!decoder || opus_error != OPUS_OK) {
        note = "NXOpus: decoder init failed";
        return false;
    }
    constexpr int kMaxPacketSamples = 5760;
    std::vector<s16> packet_pcm(static_cast<std::size_t>(kMaxPacketSamples) * stream.channels);
    std::size_t pos = stream.data_start;
    const std::size_t end = pos + stream.data_size;
    std::size_t produced = 0;
    std::size_t skipped = 0;
    while (pos + 8 <= end && produced < out.size()) {
        const u32 size = ReadU32BE(data, pos);
        pos += 8;
        if (pos + size > end) {
            note = "NXOpus: truncated packet";
            break;
        }
        const int decoded = opus_decode(decoder.get(), data.data() + pos,
                                        static_cast<opus_int32>(size), packet_pcm.data(),
                                        kMaxPacketSamples, 0);
        pos += size;
        if (decoded < 0) {
            note = fmt::format("NXOpus: decode error {}", decoded);
            break;
        }
        for (int i = 0; i < decoded * stream.channels && produced < out.size(); ++i) {
            if (skipped < stream.skip) {
                ++skipped;
                continue;
            }
            out[produced++] = packet_pcm[i];
        }
    }
    std::fill(out.begin() + produced, out.end(), s16{});
    return true;
}

struct DecodedTrack {
    std::vector<s16> pcm;
    u32 sample_rate{};
    u16 channels{};
    s64 loop_start{};
    s64 loop_end{-1};
};

bool DecodeBwav(std::span<const u8> data, DecodedTrack& out, std::string& note) {
    BwavInfo info;
    if (!ParseBwav(data, info, note)) {
        return false;
    }
    const s32 num_samples = info.channel[0].num_samples;
    if (num_samples == 0) {
        note = "no samples";
        return false;
    }
    out.sample_rate = static_cast<u32>(info.channel[0].sample_rate);
    out.channels = info.channels;
    out.pcm.assign(static_cast<std::size_t>(num_samples) * info.channels, 0);
    // DSP and PCM channels are positioned from channel 0's data start (the per-channel
    // +0x34 fields hold the same absolute offsets, so mixing them with the interleave
    // stride would double-count it). NXOpus channels instead each own a full Opus stream
    // at their +0x34 offset, and one stream can carry several audio channels.
    const u32 base_start = info.channel[0].data_start;
    u16 track_channel = 0;
    for (u16 ch = 0; ch < info.channels; ++ch) {
        const BwavChannel& channel = info.channel[ch];
        if (track_channel >= info.channels) {
            note = "NXOpus layers exceed the channel count";
            return false;
        }
        std::vector<s16> channel_pcm;
        if (channel.codec == CodecNxOpus) {
            NxOpusStream stream;
            if (!ParseNxOpus(data, channel.data_start, stream, note)) {
                return false;
            }
            if (track_channel + stream.channels > info.channels) {
                note = "NXOpus layers exceed the channel count";
                return false;
            }
            channel_pcm.assign(static_cast<std::size_t>(num_samples) * stream.channels, 0);
            if (!DecodeNxOpus(data, stream, channel_pcm, note)) {
                return false;
            }
            for (std::size_t i = 0; i < static_cast<std::size_t>(num_samples); ++i) {
                for (u16 sub = 0; sub < stream.channels; ++sub) {
                    out.pcm[i * info.channels + track_channel + sub] =
                        channel_pcm[i * stream.channels + sub];
                }
            }
            track_channel += stream.channels;
            continue;
        }
        if (base_start >= data.size()) {
            note = fmt::format("channel {} data out of range", ch);
            return false;
        }
        channel_pcm.assign(static_cast<std::size_t>(num_samples), 0);
        if (channel.codec == CodecPcm16) {
            DecodePcm16Channel(data, channel, base_start, ch, info.channels, info.interleave,
                               channel_pcm);
        } else {
            DecodeDspChannel(data, channel, base_start, ch, info.channels, info.interleave,
                             channel_pcm);
        }
        for (std::size_t i = 0; i < channel_pcm.size(); ++i) {
            out.pcm[i * info.channels + track_channel] = channel_pcm[i];
        }
        ++track_channel;
    }
    out.loop_start = info.channel[0].loop_start;
    out.loop_end = info.channel[0].loop_end;
    return true;
}

// ---------------------------------------------------------------------------------------------
// Clean output names and embedded metadata
// -------------------------------------------------------------------------------------

bool IsAllDigits(std::string_view value) {
    return !value.empty() &&
           std::all_of(value.begin(), value.end(), [](unsigned char c) {
               return std::isdigit(c) != 0;
           });
}

// "BGM_XXX_YYY_0005" becomes folder "XXX" and title "YYY": the category markers
// (BGM, Dm, ExampleMelody) are dropped, a single trailing numeric token goes away,
// the remaining tokens join with spaces, and a lone token lands directly in the
// output folder.
std::pair<std::string, std::string> CleanNameFor(const std::string& entry_name) {
    const std::string stem = EndsWithCI(entry_name, ".bwav")
                                 ? entry_name.substr(0, entry_name.size() - 5)
                                 : entry_name;
    std::vector<std::string_view> tokens;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= stem.size(); ++i) {
        if (i == stem.size() || stem[i] == '_') {
            if (i > start) {
                tokens.push_back(std::string_view{stem}.substr(start, i - start));
            }
            start = i + 1;
        }
    }
    while (!tokens.empty()) {
        const auto head = Lower(tokens.front());
        if (head != "bgm" && head != "dm" && head != "examplemelody") {
            break;
        }
        tokens.erase(tokens.begin());
    }
    if (tokens.size() >= 2 && IsAllDigits(tokens.back())) {
        tokens.pop_back();
    }
    if (tokens.empty()) {
        return {"", stem};
    }
    if (tokens.size() == 1) {
        return {"", std::string{tokens[0]}};
    }
    std::string title;
    for (std::size_t i = 1; i < tokens.size(); ++i) {
        if (!title.empty()) {
            title += ' ';
        }
        title += tokens[i];
    }
    return {std::string{tokens[0]}, title};
}

// What gets embedded into every written track.
struct TrackTags {
    std::string title;
    std::string artist;
    std::string album;
    std::span<const u8> cover{};
    std::string_view cover_mime{};
};

// PNG and JPEG headers carry the pixel size; the FLAC picture block wants it.
void ImageInfo(std::span<const u8> data, std::string_view& mime, u32& width, u32& height) {
    mime = {};
    width = height = 0;
    if (data.size() >= 24 && data[0] == 0x89 && data[1] == 'P' && data[2] == 'N' &&
        data[3] == 'G') {
        width = (u32{data[16]} << 24) | (u32{data[17]} << 16) | (u32{data[18]} << 8) |
                u32{data[19]};
        height = (u32{data[20]} << 24) | (u32{data[21]} << 16) | (u32{data[22]} << 8) |
                 u32{data[23]};
        mime = "image/png";
        return;
    }
    if (data.size() >= 4 && data[0] == 0xFF && data[1] == 0xD8 && data[2] == 0xFF) {
        std::size_t at = 2;
        while (at + 4 < data.size()) {
            if (data[at] != 0xFF) {
                ++at;
                continue;
            }
            const u8 marker = data[at + 1];
            const u32 length = (u32{data[at + 2]} << 8) | u32{data[at + 3]};
            const bool start_of_frame = (marker >= 0xC0 && marker <= 0xCF) &&
                                        marker != 0xC4 && marker != 0xC8 && marker != 0xCC;
            if (start_of_frame && at + 9 < data.size()) {
                height = (u32{data[at + 5]} << 8) | u32{data[at + 6]};
                width = (u32{data[at + 7]} << 8) | u32{data[at + 8]};
                mime = "image/jpeg";
                return;
            }
            at += 2 + length;
        }
    }
}

// ID3v2.4 with synch-safe sizes, written into the WAV "id3 " chunk.
void PutSynchsafe(std::string& out, u32 value) {
    out.push_back(static_cast<char>((value >> 21) & 0x7F));
    out.push_back(static_cast<char>((value >> 14) & 0x7F));
    out.push_back(static_cast<char>((value >> 7) & 0x7F));
    out.push_back(static_cast<char>(value & 0x7F));
}

void PutId3TextFrame(std::string& out, std::string_view id, std::string_view text) {
    std::string payload;
    payload.push_back('\x03'); // UTF-8
    payload.append(text);
    payload.push_back('\0');
    out.append(id);
    PutSynchsafe(out, static_cast<u32>(payload.size()));
    out.push_back('\0');
    out.push_back('\0');
    out.append(payload);
}

std::string MakeId3v2(const TrackTags& tags) {
    std::string frames;
    if (!tags.title.empty()) {
        PutId3TextFrame(frames, "TIT2", tags.title);
    }
    if (!tags.artist.empty()) {
        PutId3TextFrame(frames, "TPE1", tags.artist);
    }
    if (!tags.album.empty()) {
        PutId3TextFrame(frames, "TALB", tags.album);
    }
    if (!tags.cover.empty() && !tags.cover_mime.empty()) {
        std::string payload;
        payload.push_back('\x03'); // UTF-8
        payload.append(tags.cover_mime);
        payload.push_back('\0');
        payload.push_back('\x03'); // front cover
        payload.push_back('\0');  // empty description
        payload.append(reinterpret_cast<const char*>(tags.cover.data()), tags.cover.size());
        frames.append("APIC");
        PutSynchsafe(frames, static_cast<u32>(payload.size()));
        frames.push_back('\0');
        frames.push_back('\0');
        frames.append(payload);
    }
    std::string out = "ID3";
    out.push_back('\x04');
    out.push_back('\x00');
    out.push_back('\x00');
    PutSynchsafe(out, static_cast<u32>(frames.size()));
    out.append(frames);
    return out;
}

// ---------------------------------------------------------------------------------------------
// Output writers
// -------------------------------------------------------------------------------------

bool WriteWav(const std::filesystem::path& path, const DecodedTrack& track,
              const TrackTags& tags) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    const std::string id3 = MakeId3v2(tags);
    const u32 id3_chunk = static_cast<u32>(id3.size()) + (id3.size() % 2);
    const u32 data_size = static_cast<u32>(track.pcm.size() * sizeof(s16));
    const u32 riff_size = 4 + 8 + data_size + 8 + id3_chunk;
    const u32 byte_rate = track.sample_rate * track.channels * 2;
    const u16 block_align = track.channels * 2;
    constexpr u16 format = 1;
    constexpr u32 fmt_size = 16;
    constexpr u16 bits = 16;
    out.write("RIFF", 4);
    out.write(reinterpret_cast<const char*>(&riff_size), 4);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    out.write(reinterpret_cast<const char*>(&fmt_size), 4);
    out.write(reinterpret_cast<const char*>(&format), 2);
    out.write(reinterpret_cast<const char*>(&track.channels), 2);
    out.write(reinterpret_cast<const char*>(&track.sample_rate), 4);
    out.write(reinterpret_cast<const char*>(&byte_rate), 4);
    out.write(reinterpret_cast<const char*>(&block_align), 2);
    out.write(reinterpret_cast<const char*>(&bits), 2);
    out.write("data", 4);
    out.write(reinterpret_cast<const char*>(&data_size), 4);
    out.write(reinterpret_cast<const char*>(track.pcm.data()),
              static_cast<std::streamsize>(data_size));
    if (!id3.empty()) {
        const u32 id3_size = static_cast<u32>(id3.size());
        out.write("id3 ", 4);
        out.write(reinterpret_cast<const char*>(&id3_size), 4);
        out.write(id3.data(), static_cast<std::streamsize>(id3.size()));
        if (id3_chunk > id3_size) {
            out.write("\0", 1);
        }
    }
    return out.good();
}

#ifdef EGLANTINE_HAS_FLAC

bool WriteFlac(const std::filesystem::path& path, const DecodedTrack& track,
               const TrackTags& tags) {
    FLAC__StreamEncoder* encoder = FLAC__stream_encoder_new();
    if (encoder == nullptr) {
        return false;
    }
    FLAC__stream_encoder_set_channels(encoder, track.channels);
    FLAC__stream_encoder_set_sample_rate(encoder, track.sample_rate);
    FLAC__stream_encoder_set_bits_per_sample(encoder, 16);
    FLAC__stream_encoder_set_compression_level(encoder, 5);

    // Vorbis comments plus the NUP cover as a front-cover picture block. The blocks
    // stay alive until the encoder finishes; libFLAC does not copy them.
    std::vector<FLAC__StreamMetadata*> blocks;
    FLAC__StreamMetadata* vorbis =
        FLAC__metadata_object_new(FLAC__METADATA_TYPE_VORBIS_COMMENT);
    if (vorbis != nullptr) {
        const auto add_comment = [&vorbis](const char* name, const std::string& value) {
            if (value.empty()) {
                return;
            }
            FLAC__StreamMetadata_VorbisComment_Entry entry;
            if (FLAC__metadata_object_vorbiscomment_entry_from_name_value_pair(
                    &entry, name, value.c_str()) != 0) {
                FLAC__metadata_object_vorbiscomment_append_comment(vorbis, entry, true);
            }
        };
        add_comment("TITLE", tags.title);
        add_comment("ARTIST", tags.artist);
        add_comment("ALBUM", tags.album);
        blocks.push_back(vorbis);
    }
    FLAC__StreamMetadata* picture = nullptr;
    if (!tags.cover.empty() && !tags.cover_mime.empty()) {
        picture = FLAC__metadata_object_new(FLAC__METADATA_TYPE_PICTURE);
    }
    if (picture != nullptr) {
        std::string_view mime;
        u32 width{};
        u32 height{};
        ImageInfo(tags.cover, mime, width, height);
        if (!mime.empty()) {
            const std::string mime_text{mime};
            FLAC__StreamMetadata_Picture& picture_data = picture->data.picture;
            picture_data.type = FLAC__STREAM_METADATA_PICTURE_TYPE_FRONT_COVER;
            FLAC__metadata_object_picture_set_mime_type(
                picture, const_cast<char*>(mime_text.c_str()), true);
            FLAC__byte empty_description[] = "";
            FLAC__metadata_object_picture_set_description(picture, empty_description, true);
            picture_data.width = width;
            picture_data.height = height;
            picture_data.depth = 24;
            picture_data.colors = 0;
            FLAC__metadata_object_picture_set_data(
                picture, const_cast<FLAC__byte*>(tags.cover.data()),
                static_cast<u32>(tags.cover.size()), true);
            blocks.push_back(picture);
        }
    }
    if (!blocks.empty()) {
        FLAC__stream_encoder_set_metadata(encoder, blocks.data(),
                                          static_cast<u32>(blocks.size()));
    }

    bool ok = FLAC__stream_encoder_init_file(encoder, path.c_str(), nullptr, nullptr) ==
              FLAC__STREAM_ENCODER_INIT_STATUS_OK;
    if (ok) {
        const std::size_t frames_per_chunk = 65536;
        std::vector<FLAC__int32> buffer(frames_per_chunk * track.channels);
        const std::size_t total_frames = track.pcm.size() / track.channels;
        for (std::size_t start = 0; start < total_frames && ok; start += frames_per_chunk) {
            const std::size_t count = std::min(frames_per_chunk, total_frames - start);
            for (std::size_t i = 0; i < count * track.channels; ++i) {
                buffer[i] = track.pcm[start * track.channels + i];
            }
            ok = FLAC__stream_encoder_process_interleaved(
                encoder, buffer.data(), static_cast<unsigned>(count));
        }
        ok = ok && FLAC__stream_encoder_finish(encoder);
    }
    if (picture != nullptr) {
        FLAC__metadata_object_delete(picture);
    }
    if (vorbis != nullptr) {
        FLAC__metadata_object_delete(vorbis);
    }
    FLAC__stream_encoder_delete(encoder);
    return ok;
}

#endif // EGLANTINE_HAS_FLAC

bool WriteEncoded(const std::filesystem::path& path, const DecodedTrack& track,
                  Encoding encoding, const TrackTags& tags) {
    if (encoding == Encoding::Flac) {
#ifdef EGLANTINE_HAS_FLAC
        return WriteFlac(path, track, tags);
#else
        return false;
#endif
    }
    return WriteWav(path, track, tags);
}

std::string_view ExtensionOf(Encoding encoding) {
    return encoding == Encoding::Flac ? ".flac" : ".wav";
}

std::string CsvEscape(std::string_view value) {
    if (value.find(',') == std::string_view::npos &&
        value.find('"') == std::string_view::npos && value.find('\n') == std::string_view::npos) {
        return std::string{value};
    }
    std::string out;
    out.push_back('"');
    for (const char c : value) {
        if (c == '"') {
            out.push_back('"');
        }
        out.push_back(c);
    }
    out.push_back('"');
    return out;
}

std::string Seconds(u64 samples, u32 sample_rate) {
    if (sample_rate == 0) {
        return "";
    }
    return fmt::format("{:.3f}", double(samples) / sample_rate);
}

u64 TrackDigest(const std::vector<s16>& pcm) {
    return Common::CityHash64(reinterpret_cast<const char*>(pcm.data()),
                              pcm.size() * sizeof(s16));
}

} // namespace

bool HasFlacSupport() {
#ifdef EGLANTINE_HAS_FLAC
    return true;
#else
    return false;
#endif
}

bool ScanMusic(const std::string& game_path, SourceInfo& info) {
    RealVfsFilesystem vfs;
    Collected collected;
    std::string error;
    if (!Collect(game_path, vfs, collected, error)) {
        LOG_WARNING(Loader, "Music scan of {} failed: {}", game_path, error);
        return false;
    }
    info.game_title = collected.game_title;
    info.stream_count = collected.entries.size();
    info.cover = collected.icon;
    for (const auto& entry : collected.entries) {
        info.stream_bytes += entry.file != nullptr ? entry.file->GetSize() : 0;
    }
    return true;
}

Result ExtractMusic(const std::string& game_path, const Options& options,
                    const std::function<void(const Progress&)>& progress,
                    const std::atomic_bool* cancel, Summary* summary_out,
                    std::string* error_out) {
    RealVfsFilesystem vfs;
    Collected collected;
    std::string error;
    if (!Collect(game_path, vfs, collected, error)) {
        if (error_out != nullptr) {
            *error_out = error;
        }
        return Result::InvalidSource;
    }
    if (collected.entries.empty()) {
        if (error_out != nullptr) {
            *error_out = "no music streams found (looked for BGM_/Dm_/ExampleMelody_ .bwav "
                         "under Sound/Resource/Stream)";
        }
        return Result::NoMusic;
    }

    std::error_code ec;
    std::filesystem::create_directories(options.out_dir, ec);
    if (ec) {
        if (error_out != nullptr) {
            *error_out = fmt::format("cannot create {}: {}",
                                     Common::FS::PathToUTF8String(options.out_dir), ec.message());
        }
        return Result::IoError;
    }

    Progress state;
    state.files_total = collected.entries.size();
    for (const auto& entry : collected.entries) {
        state.bytes_total += entry.file != nullptr ? entry.file->GetSize() : 0;
    }

    // Output naming: clean titles, a folder from the first remaining token, and
    // 1, 2, 3... when several streams end up with the same name.
    struct OutputPlan {
        std::string folder;
        std::string title;
        std::string file_name;
    };
    const std::string_view extension = ExtensionOf(options.encoding);
    std::map<std::pair<std::string, std::string>, u32> name_groups;
    std::vector<std::pair<std::string, std::string>> clean_names;
    clean_names.reserve(collected.entries.size());
    for (const auto& entry : collected.entries) {
        auto clean = CleanNameFor(entry.name);
        if (options.layout == Layout::Flat) {
            // Flat mode ignores the folder, so collisions are counted by title alone.
            clean.first.clear();
        }
        clean_names.emplace_back(clean.first, clean.second);
        ++name_groups[{clean.first, clean.second}];
    }
    std::map<std::pair<std::string, std::string>, u32> name_seen;
    std::vector<OutputPlan> plans;
    plans.reserve(clean_names.size());
    for (const auto& [folder, title] : clean_names) {
        const auto key = std::make_pair(folder, title);
        OutputPlan plan;
        plan.folder = folder;
        plan.title = title;
        if (name_groups[key] > 1) {
            plan.file_name = fmt::format("{} {}{}", title, ++name_seen[key], extension);
        } else {
            plan.file_name = fmt::format("{}{}", title, extension);
        }
        plans.push_back(std::move(plan));
    }

    // The same tags go into every track; only the title changes. The cover comes
    // from the chosen file, or from the NUP/package icon when none was chosen.
    TrackTags base_tags;
    base_tags.artist = collected.game_title.empty()
                           ? "Original OST"
                           : fmt::format("{} - Original OST", collected.game_title);
    base_tags.album = base_tags.artist;
    base_tags.cover = options.cover_override.empty() ? collected.icon : options.cover_override;
    u32 cover_width{};
    u32 cover_height{};
    ImageInfo(base_tags.cover, base_tags.cover_mime, cover_width, cover_height);

    Summary summary;
    std::ofstream manifest(options.out_dir / "manifest.csv", std::ios::binary | std::ios::trunc);
    const bool have_manifest = manifest.good();
    if (have_manifest) {
        manifest << "file,output,kind,channels,sample_rate,duration_seconds,loop_start_seconds,"
                    "loop_end_seconds,digest,duplicate_of,note\n";
    }
    std::map<u64, std::string> seen_digests;

    for (std::size_t index = 0; index < collected.entries.size(); ++index) {
        const Entry& entry = collected.entries[index];
        const OutputPlan& plan = plans[index];
        if (cancel != nullptr && cancel->load()) {
            break;
        }
        state.current = entry.name;
        if (progress) {
            progress(state);
        }

        DecodedTrack track;
        std::string note;
        u64 digest{};
        bool decoded = false;
        if (entry.file == nullptr) {
            note = "file could not be opened";
        } else {
            const auto data = entry.file->ReadAllBytes();
            if (data.empty()) {
                note = "empty stream";
            } else if (DecodeBwav(data, track, note)) {
                decoded = true;
                digest = TrackDigest(track.pcm);
            }
        }

        const std::string kind = KindOf(entry.name);
        const std::string output_rel =
            plan.folder.empty() ? plan.file_name : plan.folder + "/" + plan.file_name;

        std::string duplicate_of;
        if (decoded) {
            const auto sub = plan.folder.empty()
                                 ? options.out_dir
                                 : options.out_dir / std::filesystem::path{plan.folder};
            if (!plan.folder.empty()) {
                std::filesystem::create_directories(sub, ec);
            }
            if (ec) {
                decoded = false;
                note = fmt::format("cannot create {}: {}", Common::FS::PathToUTF8String(sub),
                                   ec.message());
            } else {
                TrackTags tags = base_tags;
                tags.title = plan.title;
                if (!WriteEncoded(sub / plan.file_name, track, options.encoding, tags)) {
                    decoded = false;
                    note = "write failed (is the disk full?)";
                }
            }
            if (decoded) {
                const auto seen = seen_digests.find(digest);
                if (seen != seen_digests.end()) {
                    ++summary.duplicates;
                    duplicate_of = seen->second;
                } else {
                    seen_digests.emplace(digest, output_rel);
                }
            }
        }

        if (decoded) {
            ++summary.files_ok;
        } else {
            ++summary.files_failed;
        }
        if (have_manifest) {
            manifest << CsvEscape(entry.name) << ',' << CsvEscape(output_rel) << ','
                     << CsvEscape(kind) << ','
                     << (decoded ? std::to_string(track.channels) : "") << ','
                     << (decoded ? std::to_string(track.sample_rate) : "") << ','
                     << (decoded ? Seconds(track.pcm.size() / track.channels, track.sample_rate)
                                 : "")
                     << ','
                     << (decoded && track.loop_end >= 0
                             ? Seconds(static_cast<u64>(track.loop_start), track.sample_rate)
                             : "")
                     << ','
                     << (decoded && track.loop_end >= 0
                             ? Seconds(static_cast<u64>(track.loop_end), track.sample_rate)
                             : "")
                     << ',' << fmt::format("{:016x}", digest) << ','
                     << CsvEscape(duplicate_of) << ',' << CsvEscape(note) << '\n';
        }

        state.files_done++;
        state.bytes_done += entry.file != nullptr ? entry.file->GetSize() : 0;
        if (progress) {
            progress(state);
        }
    }

    if (summary_out != nullptr) {
        *summary_out = summary;
    }
    if (cancel != nullptr && cancel->load()) {
        return Result::Cancelled;
    }
    if (summary.files_ok == 0) {
        if (error_out != nullptr) {
            *error_out = "every stream failed to decode (see manifest.csv)";
        }
        return Result::IoError;
    }
    return Result::Success;
}

} // namespace FileSys::Music

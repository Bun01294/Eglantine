// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Extraction of the Alto-family music streams (romfs/Sound/Resource/Stream/*.bwav)
// from a loose dump and from the NUP archive built from it.

#include <catch2/catch_test_macros.hpp>

#include <opus.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <fmt/format.h>

#include "core/file_sys/music_extract.h"
#include "core/file_sys/nup_writer.h"

namespace {

namespace fs = std::filesystem;

struct TempDir {
    fs::path path = fs::temp_directory_path() /
                    ("music-test-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    TempDir() {
        fs::create_directories(path);
    }
    ~TempDir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

void Put(const fs::path& p, std::string_view value) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << value;
}

std::string ReadAll(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

void Put16(std::string& out, std::size_t at, u16 value) {
    std::memcpy(out.data() + at, &value, sizeof(value));
}

void Put32(std::string& out, std::size_t at, s32 value) {
    std::memcpy(out.data() + at, &value, sizeof(value));
}

// A repeating signed-nibble ramp (values in [-8, 7]): with zero coefficients and a
// scale of 1, a DSP frame decodes each sample back to exactly this value.
std::vector<s16> NibblePattern(s16 start, u32 count) {
    std::vector<s16> out;
    for (u32 i = 0; i < count; ++i) {
        s16 value = static_cast<s16>(((start % 16) + 16 + static_cast<s16>(i)) % 16);
        if (value > 7) {
            value = static_cast<s16>(value - 16);
        }
        out.push_back(value);
    }
    return out;
}

u8 NibbleOf(s16 sample) {
    return sample >= 0 ? static_cast<u8>(sample) : static_cast<u8>(sample + 16);
}

// A stereo BWAV with the DSP codec: two 8-byte frames (28 samples) per channel.
// Zero coefficients and a scale of 1 in every frame header make the decoded PCM
// equal to the signed nibbles, so the expected output is known exactly.
std::string MakeDspBwav(const std::vector<s16>& left, const std::vector<s16>& right,
                        s32 loop_start = -1, s32 loop_end = -1) {
    REQUIRE(left.size() == 28);
    REQUIRE(right.size() == 28);
    for (const s16 sample : left) {
        REQUIRE(sample >= -8);
        REQUIRE(sample <= 7);
    }
    constexpr u32 kDataStart = 0xC0;
    constexpr u32 kInterleave = 2 * 8;
    std::string out(0xC0 + kInterleave * 2, '\0');
    std::memcpy(out.data(), "BWAV", 4);
    Put16(out, 0x04, 0xFEFF);
    Put16(out, 0x06, 0x0001);
    Put16(out, 0x0e, 2);
    for (u16 ch = 0; ch < 2; ++ch) {
        const std::size_t base = 0x10 + 0x4C * ch;
        // Real streams store each channel's own absolute data start in +0x34 (channel 1
        // at kDataStart + kInterleave), which the decoder must not combine with the
        // interleave stride again.
        const u32 own_start = kDataStart + ch * kInterleave;
        Put16(out, base + 0x00, 1);               // DSP codec
        Put32(out, base + 0x04, 48000);            // sample rate
        Put32(out, base + 0x0c, 28);               // samples
        Put32(out, base + 0x30, own_start);        // full-file offset
        Put32(out, base + 0x34, own_start);        // data start
        Put32(out, base + 0x38, 1);               // flag
        Put32(out, base + 0x3c, loop_end);         // loop end
        Put32(out, base + 0x40, loop_start);      // loop start
        Put16(out, base + 0x46, 0);               // hist1
        Put16(out, base + 0x48, 0);               // hist2
    }
    Put32(out, 0x8C, kDataStart + kInterleave); // second channel's data offset
    const auto channel_data = [&](u16 ch) {
        const std::vector<s16>& samples = ch == 0 ? left : right;
        std::size_t at = kDataStart + ch * kInterleave;
        for (u32 frame = 0; frame < 2; ++frame) {
            // Frame header 0x00: scale 1<<0 and coefficient index 0, so with zero
            // coefficients every decoded sample equals its signed nibble.
            out[at++] = '\x00';
            for (u32 pair = 0; pair < 7; ++pair) {
                const s16 high = samples[frame * 14 + pair * 2];
                const s16 low = samples[frame * 14 + pair * 2 + 1];
                out[at++] = static_cast<char>((NibbleOf(high) << 4) | NibbleOf(low));
            }
        }
    };
    channel_data(0);
    channel_data(1);
    return out;
}

std::string MakePcm16Bwav(const std::vector<s16>& left, const std::vector<s16>& right) {
    REQUIRE(left.size() == 28);
    REQUIRE(right.size() == 28);
    constexpr u32 kDataStart = 0xC0;
    constexpr u32 kInterleave = 2 * 28;
    std::string out(0xC0 + kInterleave * 2, '\0');
    std::memcpy(out.data(), "BWAV", 4);
    Put16(out, 0x04, 0xFEFF);
    Put16(out, 0x06, 0x0001);
    Put16(out, 0x0e, 2);
    for (u16 ch = 0; ch < 2; ++ch) {
        const std::size_t base = 0x10 + 0x4C * ch;
        const u32 own_start = kDataStart + ch * kInterleave;
        Put16(out, base + 0x00, 0);           // PCM16 codec
        Put32(out, base + 0x04, 32000);       // sample rate
        Put32(out, base + 0x0c, 28);          // samples
        Put32(out, base + 0x30, own_start); // full-file offset
        Put32(out, base + 0x34, own_start);  // data start
        Put32(out, base + 0x38, 1);           // flag
        Put32(out, base + 0x3c, -1);          // no loop
    }
    Put32(out, 0x8C, kDataStart + kInterleave);
    // Block-interleaved layout: the whole first channel, then the whole second.
    std::size_t at = kDataStart;
    for (const s16 sample : left) {
        std::memcpy(out.data() + at, &sample, sizeof(s16));
        at += sizeof(s16);
    }
    for (const s16 sample : right) {
        std::memcpy(out.data() + at, &sample, sizeof(s16));
        at += sizeof(s16);
    }
    return out;
}

std::vector<s16> Ramp(s16 first, s16 step, u32 count) {
    std::vector<s16> out;
    s16 value = first;
    for (u32 i = 0; i < count; ++i) {
        out.push_back(value);
        value = static_cast<s16>(value + step);
    }
    return out;
}

std::vector<s16> Interleave(const std::vector<s16>& left, const std::vector<s16>& right) {
    std::vector<s16> out;
    out.reserve(left.size() * 2);
    for (u32 i = 0; i < left.size(); ++i) {
        out.push_back(left[i]);
        out.push_back(right[i]);
    }
    return out;
}

std::string PcmPayload(const std::vector<s16>& pcm) {
    return std::string(reinterpret_cast<const char*>(pcm.data()), pcm.size() * sizeof(s16));
}

constexpr u64 TitleId = 0x0400ABCD00020000ULL;

std::string Npdm(u64 program_id) {
    std::string npdm(0x100, '\0');
    npdm.replace(0x80, 4, "ACI0");
    for (int i = 0; i < 8; ++i) {
        npdm[0x90 + i] = static_cast<char>(program_id >> (8 * i));
    }
    return npdm;
}

// A dump with two music streams per tree, ignored effects and voice, and an update
// that replaces one stream (base had DSP, the update ships PCM16) and adds another.
fs::path MakeDump(const fs::path& root) {
    const fs::path base = root / "base";
    const fs::path stream = base / "romfs/Sound/Resource/Stream";
    Put(base / "exefs/main", "main");
    Put(base / "exefs/main.npdm", Npdm(TitleId));
    Put(base / "romfs/readme.txt", "not audio");
    Put(stream / "BGM_Main.bwav",
        MakeDspBwav(NibblePattern(1, 28), NibblePattern(9, 28), 14, 28));
    Put(stream / "Dm_Title_0005.bwav", MakeDspBwav(NibblePattern(2, 28), NibblePattern(10, 28)));
    Put(stream / "ExampleMelody_Test.bwav",
        MakeDspBwav(NibblePattern(3, 28), NibblePattern(11, 28)));
    Put(stream / "SE_Ignored.bwav", MakeDspBwav(NibblePattern(4, 28), NibblePattern(12, 28)));
    Put(stream / "NV_Ignored.bwav", MakeDspBwav(NibblePattern(5, 28), NibblePattern(13, 28)));
    Put(stream / "IgnoreMe.bwav", MakeDspBwav(NibblePattern(6, 28), NibblePattern(14, 28)));
    const fs::path update = root / "updates" / "7" / "romfs/Sound/Resource/Stream";
    Put(root / "updates" / "7" / "exefs/main", "update main");
    Put(root / "updates" / "7" / "exefs/main.npdm", Npdm(TitleId));
    Put(update / "BGM_Main.bwav", MakePcm16Bwav(Ramp(7, 1, 28), Ramp(-7, -1, 28)));
    Put(update / "BGM_New.bwav",
        MakeDspBwav(NibblePattern(8, 28), NibblePattern(0, 28), 5, 28));
    return base / "exefs/main";
}

std::string WavOf(const fs::path& out_dir, std::string_view sub, std::string_view name) {
    return ReadAll(out_dir / sub / fmt::format("{}.wav", name));
}

std::string WavOf(const fs::path& out_dir, std::string_view name) {
    return ReadAll(out_dir / fmt::format("{}.wav", name));
}

} // namespace

TEST_CASE("FileSys::Music round-trips through a loose dump", "[core][music]") {
    TempDir dump_dir;
    TempDir out_dir;
    const fs::path main = MakeDump(dump_dir.path);

    FileSys::Music::SourceInfo info;
    REQUIRE(FileSys::Music::ScanMusic(main.string(), info));
    // The update replaces BGM_Main and adds BGM_New; Dm_Title and ExampleMelody count
    // too, the plain effect and voice files do not.
    REQUIRE(info.stream_count == 4);

    FileSys::Music::Options options;
    options.out_dir = out_dir.path;
    options.layout = FileSys::Music::Layout::ByKind;
    options.encoding = FileSys::Music::Encoding::Wav;

    FileSys::Music::Summary summary;
    std::string error;
    const auto result =
        FileSys::Music::ExtractMusic(main.string(), options, {}, nullptr, &summary, &error);
    REQUIRE(result == FileSys::Music::Result::Success);
    REQUIRE(summary.files_ok == 4);
    REQUIRE(summary.files_failed == 0);
    REQUIRE(summary.duplicates == 0);
    REQUIRE(error.empty());

    // Clean names: category markers and the numeric suffix are gone, a lone token
    // lands directly in the output folder.
    REQUIRE(fs::exists(out_dir.path / "Main.wav"));
    REQUIRE(fs::exists(out_dir.path / "New.wav"));
    REQUIRE(fs::exists(out_dir.path / "Test.wav"));
    REQUIRE(fs::exists(out_dir.path / "Title.wav"));
    REQUIRE(!fs::exists(out_dir.path / "SE_Ignored.wav"));
    REQUIRE(!fs::exists(out_dir.path / "NV_Ignored.wav"));
    REQUIRE(!fs::exists(out_dir.path / "IgnoreMe.wav"));

    // BGM_Main comes from the update tree (PCM16), not the base (DSP).
    const auto main_pcm = WavOf(out_dir.path, "Main");
    const auto expected_main = PcmPayload(Interleave(Ramp(7, 1, 28), Ramp(-7, -1, 28)));
    REQUIRE(main_pcm.size() > 44 + expected_main.size()); // ID3 chunk trails the data
    REQUIRE(main_pcm.substr(44, expected_main.size()) == expected_main);
    REQUIRE(std::memcmp(main_pcm.data(), "RIFF", 4) == 0);
    REQUIRE(std::memcmp(main_pcm.data() + 8, "WAVE", 4) == 0);

    // The DSP track decodes back to its signed nibbles.
    const auto new_pcm = WavOf(out_dir.path, "New");
    REQUIRE(new_pcm.substr(44, expected_main.size()) ==
            PcmPayload(Interleave(NibblePattern(8, 28), NibblePattern(0, 28))));

    // Sample rate straight from the BWAV channel block.
    u32 rate{};
    std::memcpy(&rate, main_pcm.data() + 24, sizeof(rate));
    REQUIRE(rate == 32000);

    // Embedded metadata: clean title plus "<game> - Original OST".
    REQUIRE(main_pcm.find("TIT2") != std::string::npos);
    REQUIRE(main_pcm.find("Main") != std::string::npos);
    REQUIRE(main_pcm.find(" - Original OST") != std::string::npos);

    // manifest.csv lists every stream with its clean output name, kind and loop points.
    const auto manifest = ReadAll(out_dir.path / "manifest.csv");
    REQUIRE(manifest.find("BGM_Main.bwav,Main.wav,bgm,2,32000,0.001,,") != std::string::npos);
    REQUIRE(manifest.find("BGM_New.bwav,New.wav,bgm,2,48000,0.001,0.000,0.001,") !=
            std::string::npos);
    REQUIRE(manifest.find("Dm_Title_0005.bwav,Title.wav,title,2,48000,") != std::string::npos);
    REQUIRE(manifest.find("SE_Ignored") == std::string::npos);
}

TEST_CASE("FileSys::Music flat layout keeps every track in the output folder",
          "[core][music]") {
    TempDir dump_dir;
    TempDir out_dir;
    const fs::path main = MakeDump(dump_dir.path);

    FileSys::Music::Options options;
    options.out_dir = out_dir.path;
    options.layout = FileSys::Music::Layout::Flat;
    options.encoding = FileSys::Music::Encoding::Wav;

    FileSys::Music::Summary summary;
    std::string error;
    REQUIRE(FileSys::Music::ExtractMusic(main.string(), options, {}, nullptr, &summary,
                                         &error) == FileSys::Music::Result::Success);
    REQUIRE(fs::exists(out_dir.path / "Main.wav"));
    REQUIRE(fs::exists(out_dir.path / "New.wav"));
    REQUIRE(fs::exists(out_dir.path / "Title.wav"));
    REQUIRE(fs::exists(out_dir.path / "Test.wav"));
}

TEST_CASE("FileSys::Music reads the same tracks from a packed NUP", "[core][music]") {
    TempDir dump_dir;
    TempDir out_dir;
    const fs::path main = MakeDump(dump_dir.path);
    const fs::path archive = dump_dir.path / "music.nup";
    std::string pack_error;
    REQUIRE(FileSys::Nup::PackDump(dump_dir.path, archive, {}, {}, nullptr, &pack_error) ==
            FileSys::Nup::PackResult::Success);

    FileSys::Music::SourceInfo info;
    REQUIRE(FileSys::Music::ScanMusic(archive.string(), info));
    REQUIRE(info.stream_count == 4);

    FileSys::Music::Options options;
    options.out_dir = out_dir.path;
    options.layout = FileSys::Music::Layout::ByKind;
    options.encoding = FileSys::Music::Encoding::Wav;

    FileSys::Music::Summary summary;
    std::string error;
    REQUIRE(FileSys::Music::ExtractMusic(archive.string(), options, {}, nullptr, &summary,
                                         &error) == FileSys::Music::Result::Success);
    REQUIRE(summary.files_ok == 4);

    // Same bytes as the loose run: the update package overlays the base inside the
    // archive, and the zstd member decompresses to the original stream.
    const auto main_pcm = WavOf(out_dir.path, "Main");
    REQUIRE(main_pcm.substr(44, 112) == PcmPayload(Interleave(Ramp(7, 1, 28), Ramp(-7, -1, 28))));
    REQUIRE(fs::exists(out_dir.path / "Title.wav"));
}

TEST_CASE("FileSys::Music flags identical tracks and skips unknown sources",
          "[core][music]") {
    TempDir dump_dir;
    TempDir out_dir;
    const fs::path stream = dump_dir.path / "base/romfs/Sound/Resource/Stream";
    Put(dump_dir.path / "base/exefs/main", "main");
    Put(dump_dir.path / "base/exefs/main.npdm", Npdm(TitleId));
    const std::string bytes = MakeDspBwav(NibblePattern(1, 28), NibblePattern(9, 28));
    Put(stream / "BGM_First.bwav", bytes);
    Put(stream / "BGM_Same.bwav", bytes);
    Put(stream / "BGM_Other.bwav", MakeDspBwav(NibblePattern(2, 28), NibblePattern(10, 28)));
    // Two streams whose clean name is the same: "BGM_Scene_Song_0001" and
    // "..._0002" both become Scene/Song, so they are numbered apart.
    Put(stream / "BGM_Scene_Song_0001.bwav", bytes);
    Put(stream / "BGM_Scene_Song_0002.bwav", bytes);

    const fs::path main = dump_dir.path / "base/exefs/main";
    FileSys::Music::SourceInfo info;
    REQUIRE(FileSys::Music::ScanMusic(main.string(), info));
    REQUIRE(info.stream_count == 5);

    FileSys::Music::Options options;
    options.out_dir = out_dir.path;
    options.layout = FileSys::Music::Layout::ByKind;
    options.encoding = FileSys::Music::Encoding::Wav;

    FileSys::Music::Summary summary;
    std::string error;
    REQUIRE(FileSys::Music::ExtractMusic(main.string(), options, {}, nullptr, &summary,
                                         &error) == FileSys::Music::Result::Success);
    REQUIRE(summary.files_ok == 5);
    REQUIRE(summary.duplicates == 3);
    // Collisions get numbered; unique names stay clean.
    REQUIRE(fs::exists(out_dir.path / "Scene/Song 1.wav"));
    REQUIRE(fs::exists(out_dir.path / "Scene/Song 2.wav"));
    REQUIRE(fs::exists(out_dir.path / "First.wav"));
    REQUIRE(fs::exists(out_dir.path / "Same.wav"));
    REQUIRE(fs::exists(out_dir.path / "Other.wav"));
    const auto manifest = ReadAll(out_dir.path / "manifest.csv");
    // BGM_Same decodes to the same PCM as BGM_First and points at it.
    REQUIRE(manifest.find("BGM_Same.bwav,Same.wav,bgm,2,48000,0.001,,") != std::string::npos);
    REQUIRE(manifest.find("First.wav") != std::string::npos);
    REQUIRE(manifest.find("BGM_Scene_Song_0001.bwav,Scene/Song 1.wav,") != std::string::npos);
    REQUIRE(manifest.find("BGM_Scene_Song_0002.bwav,Scene/Song 2.wav,") != std::string::npos);

    // Neither a plain folder nor a missing NUP is a music source.
    FileSys::Music::SourceInfo bad;
    REQUIRE(!FileSys::Music::ScanMusic((dump_dir.path / "base").string(), bad));
    REQUIRE(!FileSys::Music::ScanMusic((dump_dir.path / "missing.nup").string(), bad));
}

TEST_CASE("FileSys::Music reports cancellation and per-file failures", "[core][music]") {
    TempDir dump_dir;
    TempDir out_dir;
    const fs::path stream = dump_dir.path / "base/romfs/Sound/Resource/Stream";
    Put(dump_dir.path / "base/exefs/main", "main");
    Put(dump_dir.path / "base/exefs/main.npdm", Npdm(TitleId));
    Put(stream / "BGM_Good.bwav", MakeDspBwav(NibblePattern(1, 28), NibblePattern(9, 28)));
    Put(stream / "BGM_Bad.bwav", "not a bwav at all");

    FileSys::Music::Options options;
    options.out_dir = out_dir.path;
    options.layout = FileSys::Music::Layout::ByKind;
    options.encoding = FileSys::Music::Encoding::Wav;

    FileSys::Music::Summary summary;
    std::string error;
    REQUIRE(FileSys::Music::ExtractMusic(
                (dump_dir.path / "base/exefs/main").string(), options, {}, nullptr, &summary,
                &error) == FileSys::Music::Result::Success);
    REQUIRE(summary.files_ok == 1);
    REQUIRE(summary.files_failed == 1);
    const auto manifest = ReadAll(out_dir.path / "manifest.csv");
    REQUIRE(manifest.find("BGM_Bad.bwav,Bad.wav,bgm,,,") != std::string::npos);
    REQUIRE(manifest.find("missing BWAV magic") != std::string::npos);

    // Cancellation stops before the first file.
    std::atomic_bool cancel{true};
    FileSys::Music::Summary cancelled;
    REQUIRE(FileSys::Music::ExtractMusic((dump_dir.path / "base/exefs/main").string(), options,
                                         {}, &cancel, &cancelled, &error) ==
            FileSys::Music::Result::Cancelled);
    REQUIRE(cancelled.files_ok == 0);
}

// Opt-in run against a real game archive: set EDEN_MUSIC_TEST_NUP to a .nup file and
// EDEN_MUSIC_TEST_OUT to a writable folder. Extracts every stream the way the UI does
// and fails when any track cannot be decoded.
TEST_CASE("FileSys::Music extracts a real NUP archive", "[core][music]") {
    const char* nup = std::getenv("EDEN_MUSIC_TEST_NUP");
    const char* out = std::getenv("EDEN_MUSIC_TEST_OUT");
    if (nup == nullptr || out == nullptr || *nup == '\0' || *out == '\0') {
        SKIP("set EDEN_MUSIC_TEST_NUP and EDEN_MUSIC_TEST_OUT to run this");
    }

    FileSys::Music::SourceInfo info;
    REQUIRE(FileSys::Music::ScanMusic(nup, info));
    REQUIRE(info.stream_count > 0);

    FileSys::Music::Options options;
    options.out_dir = out;
    options.layout = FileSys::Music::Layout::ByKind;
    options.encoding =
        std::getenv("EDEN_MUSIC_TEST_WAV") != nullptr || !FileSys::Music::HasFlacSupport()
            ? FileSys::Music::Encoding::Wav
            : FileSys::Music::Encoding::Flac;
    if (const char* cover = std::getenv("EDEN_MUSIC_TEST_COVER");
        cover != nullptr && *cover != '\0') {
        std::ifstream in(cover, std::ios::binary);
        options.cover_override = {std::istreambuf_iterator<char>(in), {}};
        REQUIRE(!options.cover_override.empty());
    }

    FileSys::Music::Summary summary;
    std::string error;
    REQUIRE(FileSys::Music::ExtractMusic(nup, options, {}, nullptr, &summary, &error) ==
            FileSys::Music::Result::Success);
    // Streams the decoder does not support (NXOpus cutscene takes in TotK-era audio)
    // are recorded in manifest.csv instead of failing the run.
    REQUIRE(summary.files_ok + summary.files_failed == info.stream_count);
    REQUIRE(summary.files_ok > 0);
    REQUIRE(fs::exists(options.out_dir / "manifest.csv"));
    fmt::print(stderr, "extracted {} stream(s), {} skipped (see manifest), {} duplicate(s), "
                       "title \"{}\"\n",
               summary.files_ok, summary.files_failed, summary.duplicates, info.game_title);
}

// ---------------------------------------------------------------------------------------------
// BWAV codec 2 (NXOpus), the TotK-era layout: each channel block points at its own
// Nintendo Switch Opus stream.
// ---------------------------------------------------------------------------------------------

namespace {

std::vector<std::vector<u8>> EncodeOpusFrames(const std::vector<s16>& pcm) {
    int error = OPUS_OK;
    std::unique_ptr<OpusEncoder, decltype(&opus_encoder_destroy)> encoder{
        opus_encoder_create(48000, 1, OPUS_APPLICATION_AUDIO, &error),
        &opus_encoder_destroy};
    REQUIRE(encoder != nullptr);
    REQUIRE(error == OPUS_OK);
    opus_encoder_ctl(encoder.get(), OPUS_SET_BITRATE(96000));

    std::vector<u8> buffer(4000);
    std::vector<std::vector<u8>> packets;
    for (std::size_t off = 0; off + 960 <= pcm.size(); off += 960) {
        const int size = opus_encode(encoder.get(), pcm.data() + off, 960, buffer.data(),
                                     static_cast<opus_int32>(buffer.size()));
        REQUIRE(size > 0);
        packets.emplace_back(buffer.begin(), buffer.begin() + size);
    }
    return packets;
}

std::string MakeNxOpusStream(const std::vector<std::vector<u8>>& packets, u16 skip) {
    u32 data_size = 0;
    for (const auto& packet : packets) {
        data_size += 8 + static_cast<u32>(packet.size());
    }
    std::string stream(0x2C + data_size, '\0');
    Put32(stream, 0x00, 0x80000001); // basic info chunk
    Put32(stream, 0x04, 0x24);       // chunk size
    stream[0x09] = 1;                 // channels
    Put32(stream, 0x0c, 48000);      // sample rate
    Put32(stream, 0x10, 0x24);       // data info chunk offset
    Put16(stream, 0x1c, skip);       // pre-skip
    Put32(stream, 0x24, 0x80000004); // data info chunk
    Put32(stream, 0x28, data_size);
    std::size_t at = 0x2C;
    for (const auto& packet : packets) {
        const u32 size = static_cast<u32>(packet.size());
        stream[at++] = static_cast<char>(size >> 24);
        stream[at++] = static_cast<char>(size >> 16);
        stream[at++] = static_cast<char>(size >> 8);
        stream[at++] = static_cast<char>(size);
        at += 4;
        std::memcpy(stream.data() + at, packet.data(), packet.size());
        at += packet.size();
    }
    return stream;
}

std::string MakeNxOpusBwav(const std::string& left, const std::string& right, u16 skip,
                           s32 num_samples) {
    constexpr u32 kLeftStart = 0xC0;
    const u32 kRightStart = kLeftStart + static_cast<u32>(left.size());
    std::string out(kRightStart + right.size(), '\0');
    std::memcpy(out.data(), "BWAV", 4);
    Put16(out, 0x04, 0xFEFF);
    Put16(out, 0x06, 0x0001);
    Put16(out, 0x0e, 2);
    for (u16 ch = 0; ch < 2; ++ch) {
        const std::size_t base = 0x10 + 0x4C * ch;
        const u32 own_start = ch == 0 ? kLeftStart : kRightStart;
        Put16(out, base + 0x00, 2);           // NXOpus codec
        Put32(out, base + 0x04, 48000);       // sample rate
        Put32(out, base + 0x0c, num_samples); // samples
        Put32(out, base + 0x30, own_start);
        Put32(out, base + 0x34, own_start);
        Put32(out, base + 0x38, 1);
        Put32(out, base + 0x3c, -1);
    }
    std::memcpy(out.data() + kLeftStart, left.data(), left.size());
    std::memcpy(out.data() + kRightStart, right.data(), right.size());
    return out;
}

std::vector<s16> DecodeOpusFrames(const std::vector<std::vector<u8>>& packets, u16 skip,
                                  s32 take) {
    int error = OPUS_OK;
    std::unique_ptr<OpusDecoder, decltype(&opus_decoder_destroy)> decoder{
        opus_decoder_create(48000, 1, &error), &opus_decoder_destroy};
    REQUIRE(decoder != nullptr);
    std::vector<s16> buffer(5760);
    std::vector<s16> all;
    for (const auto& packet : packets) {
        const int decoded = opus_decode(decoder.get(), packet.data(),
                                        static_cast<opus_int32>(packet.size()), buffer.data(),
                                        5760, 0);
        REQUIRE(decoded > 0);
        all.insert(all.end(), buffer.begin(), buffer.begin() + decoded);
    }
    REQUIRE(all.size() >= skip + static_cast<std::size_t>(take));
    return {all.begin() + skip, all.begin() + skip + take};
}

} // namespace

TEST_CASE("FileSys::Music decodes NXOpus BWAV streams", "[core][music]") {
    // Two distinct mono signals, encoded as real Opus packets.
    constexpr s32 kFrames = 4;
    constexpr s32 kFrameSamples = 960;
    constexpr u16 kSkip = 5;
    constexpr s32 kSamples = kFrames * kFrameSamples - kSkip;
    std::vector<s16> left(kFrames * kFrameSamples);
    std::vector<s16> right(kFrames * kFrameSamples);
    for (u32 i = 0; i < left.size(); ++i) {
        left[i] = static_cast<s16>(i % 1000);
        right[i] = static_cast<s16>(-800 + (i * 7) % 1600);
    }
    const auto left_packets = EncodeOpusFrames(left);
    const auto right_packets = EncodeOpusFrames(right);
    const std::string bwav = MakeNxOpusBwav(MakeNxOpusStream(left_packets, kSkip),
                                            MakeNxOpusStream(right_packets, kSkip), kSkip,
                                            kSamples);

    TempDir dump_dir;
    TempDir out_dir;
    const fs::path stream = dump_dir.path / "base/romfs/Sound/Resource/Stream";
    Put(dump_dir.path / "base/exefs/main", "main");
    Put(dump_dir.path / "base/exefs/main.npdm", Npdm(TitleId));
    Put(stream / "BGM_Opus.bwav", bwav);

    FileSys::Music::Options options;
    options.out_dir = out_dir.path;
    options.layout = FileSys::Music::Layout::ByKind;
    options.encoding = FileSys::Music::Encoding::Wav;

    FileSys::Music::Summary summary;
    std::string error;
    REQUIRE(FileSys::Music::ExtractMusic((dump_dir.path / "base/exefs/main").string(), options,
                                         {}, nullptr, &summary, &error) ==
            FileSys::Music::Result::Success);
    REQUIRE(summary.files_ok == 1);
    REQUIRE(summary.files_failed == 0);

    // The extractor must produce exactly what decoding the same packets directly does,
    // with the pre-skip dropped.
    const auto expected_left = DecodeOpusFrames(left_packets, kSkip, kSamples);
    const auto expected_right = DecodeOpusFrames(right_packets, kSkip, kSamples);
    const auto wav = WavOf(out_dir.path, "Opus");
    REQUIRE(wav.substr(44, 2 * kSamples * 2) ==
            PcmPayload(Interleave(expected_left, expected_right)));
}
TEST_CASE("FileSys::Music embeds a chosen cover into every track", "[core][music]") {
    // A 1x1 transparent PNG, small enough to spell out in full.
    constexpr std::array<u8, 67> kPng{
        0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49,
        0x48, 0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06,
        0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4, 0x89, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x44,
        0x41, 0x54, 0x78, 0x9C, 0x62, 0x00, 0x01, 0x00, 0x00, 0x05, 0x00, 0x01, 0x0D,
        0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42,
        0x60, 0x82,
    };
    TempDir dump_dir;
    TempDir out_dir;
    const fs::path stream = dump_dir.path / "base/romfs/Sound/Resource/Stream";
    Put(dump_dir.path / "base/exefs/main", "main");
    Put(dump_dir.path / "base/exefs/main.npdm", Npdm(TitleId));
    Put(stream / "BGM_Cover.bwav", MakeDspBwav(NibblePattern(1, 28), NibblePattern(9, 28)));

    FileSys::Music::Options options;
    options.out_dir = out_dir.path;
    options.layout = FileSys::Music::Layout::ByKind;
    options.encoding = FileSys::Music::Encoding::Wav;
    options.cover_override.assign(kPng.begin(), kPng.end());

    FileSys::Music::Summary summary;
    std::string error;
    REQUIRE(FileSys::Music::ExtractMusic((dump_dir.path / "base/exefs/main").string(), options,
                                         {}, nullptr, &summary, &error) ==
            FileSys::Music::Result::Success);
    REQUIRE(summary.files_ok == 1);

    // The WAV carries an ID3 chunk with the cover as an APIC frame.
    const auto wav = WavOf(out_dir.path, "Cover");
    REQUIRE(wav.find("APIC") != std::string::npos);
    REQUIRE(wav.find("image/png") != std::string::npos);
    const std::string png(reinterpret_cast<const char*>(kPng.data()), kPng.size());
    REQUIRE(wav.find(png) != std::string::npos);
    REQUIRE(wav.find("TIT2") != std::string::npos);
    REQUIRE(wav.find("Cover") != std::string::npos);
}

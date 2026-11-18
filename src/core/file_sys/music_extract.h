// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include "common/common_types.h"

// Pulls the music streams out of a game the emulator can open from a NUP archive
// or a loose folder dump. Works on the Alto-family audio layout used by modern
// Nintendo titles: romfs/Sound/Resource/Stream/BGM_*.bwav are the songs, Dm_*
// cutscene music and ExampleMelody_* reference performances, while other
// prefixes in the same folder are voice, effects and ambience.
namespace FileSys::Music {

enum class Layout {
    // One subfolder per kind: bgm, cutscene, title, segments, stems.
    ByKind,
    // Every track directly in the output folder.
    Flat,
};

enum class Encoding {
    Wav,
    Flac,
};

struct Options {
    std::filesystem::path out_dir;
    Layout layout{Layout::ByKind};
    Encoding encoding{Encoding::Wav};
    // Cover art override; empty means whatever the NUP carries, if anything.
    std::vector<u8> cover_override;
};

struct Progress {
    u64 files_done{};
    u64 files_total{};
    u64 bytes_done{};
    u64 bytes_total{};
    std::string current;
};

enum class Result { Success, Cancelled, InvalidSource, NoMusic, IoError };

struct Summary {
    u64 files_ok{};
    u64 files_failed{};
    u64 duplicates{};
};

struct SourceInfo {
    std::string game_title;
    u64 stream_count{};
    u64 stream_bytes{};
    // The NUP/package icon when one is stored; used as the cover art by default.
    std::vector<u8> cover;
};

// Whether this build can encode FLAC (needs libFLAC at build time); WAV is always available.
bool HasFlacSupport();

// A quick look at what ExtractMusic would pull from the game. Returns false when the
// path is neither a NUP archive nor a loose dump; stream_count can still be 0.
bool ScanMusic(const std::string& game_path, SourceInfo& info);

// Decodes every music stream to the output folder (plus manifest.csv) and never
// touches the source. Tracks that fail to decode are recorded in the manifest and
// counted in the summary rather than aborting the run.
Result ExtractMusic(const std::string& game_path, const Options& options,
                    const std::function<void(const Progress&)>& progress = {},
                    const std::atomic_bool* cancel = nullptr, Summary* summary_out = nullptr,
                    std::string* error_out = nullptr);

} // namespace FileSys::Music

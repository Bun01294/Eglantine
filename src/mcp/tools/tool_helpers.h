// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

// Pieces shared between tool files (composite tools such as `act` reuse the single-purpose ones).

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "mcp/timeline.h"
#include "mcp/tool.h"

namespace Core {
class System;
}

namespace Mcp {

class Host;
struct ToolContext;

// act_tools.cpp
/// Gate for tools (or tool arguments) that change state but are annotated read_only so they do
/// not hold the global mutating-tools mutex: returns an error when Allow Writes is off right now
/// or another session holds the control lease, otherwise renews the caller's lease and returns
/// nullopt. Long-running handlers call it before every further input they send.
std::optional<ToolResult> RequireWritesAndControl(ToolContext& ctx);
/// Same check as a message ("" when allowed; the lease is renewed).
std::string WriteGateError(ToolContext& ctx);
/// Refusal text for a tool argument that needs writes (`what` e.g. "remove").
ToolResult NeedsWritesError(std::string_view what);
/// Called when Allow Writes is switched off: stops background routes and route recording, TAS
/// playback/recording, and releases every virtual button and stick, so nothing keeps sending
/// input after the user revokes writes.
/// Call it on the frontend's UI thread: it touches the TAS engine directly (the thread its
/// hotkeys run on) and only asks worker threads to stop, it never waits for them.
void StopWriteWork(Host& host, Core::System& system);
/// `path` relative to the Eden data directory with forward slashes (e.g. "config/mcp/routes/x.json"),
/// or just the file name when it lies elsewhere. Tool output uses it instead of absolute paths,
/// which contain the OS user name.
std::string DataRelativePath(const std::filesystem::path& path);

// server.cpp
/// Masks key material, serials and similar secrets in a message before it leaves the server.
std::string RedactKeys(std::string_view message);

/// True for Windows reserved device names (CON, PRN, AUX, NUL, COM1-9, LPT1-9), case-insensitive,
/// with or without an extension ("nul.txt"): files with these names open the device, not a file.
inline bool IsReservedDeviceName(std::string_view name) {
    std::string stem{name.substr(0, name.find('.'))};
    while (!stem.empty() && (stem.back() == ' ')) {
        stem.pop_back();
    }
    std::transform(stem.begin(), stem.end(), stem.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    if (stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") {
        return true;
    }
    return stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) &&
           stem[3] >= '0' && stem[3] <= '9';
}

// input_tools.cpp
/// Schema of the `player` argument (1-8 = P1-P8, 9 = handheld).
json PlayerSchema();
/// Schema of one input_sequence-style step list.
json InputStepsSchema();
/// npad index from the `player` argument (1-8 = P1-P8, 9 = handheld).
size_t NpadIndex(const json& args);
/// Runs input_sequence steps on the virtual gamepad. Returns an error message, or "" on success.
std::string RunInputSteps(ToolContext& ctx, size_t npad, const json& steps);
/// Error text when `steps` is not a valid input step list (types, ranges, known buttons), or "".
/// For route steps from hand-editable files; RunInputSteps checks again before sending anything.
std::string ValidateInputSteps(const json& steps);
/// Buttons and sticks as the game sees them.
json ControllerStateJson(ToolContext& ctx, size_t npad);

// core_tools.cpp
/// One timeline event as JSON (seq, t_ms, frame, kind, source, msg; level for logs).
json EventJson(const Event& event);
/// Filter properties shared by timeline, wait_for and act.
json FilterProperties();
/// Builds a timeline query from FilterProperties-style arguments. Unknown kinds and levels are
/// ignored.
Timeline::Query ParseQuery(const json& args);
/// Frontend fps, emulation speed, frame time and shader builds, or null when there is no host
/// or no valid sample yet.
json PerfJson(ToolContext& ctx);

/// Ends memory watches and their sampling thread (server shutdown).
void StopMemoryWork();
// emulation_tools.cpp
/// PNG of the capture layer (works paused/hidden), scaled to max_width.
std::optional<std::vector<u8>> RawCapturePng(ToolContext& ctx, u32 max_width, std::string& error);

/// Set while the server shuts down: long waits in tools check it and return early.
inline std::atomic<bool> g_server_stopping{false};
/// Whether the server is shutting down (reads g_server_stopping).
[[nodiscard]] inline bool ServerStopping() {
    return g_server_stopping.load(std::memory_order_relaxed);
}
/// Sleeps up to `duration`, returning early (false) when the server is stopping.
inline bool SleepUnlessStopping(std::chrono::milliseconds duration) {
    const auto until = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < until) {
        if (ServerStopping()) {
            return false;
        }
        std::this_thread::sleep_for(
            std::min<std::chrono::milliseconds>(std::chrono::milliseconds{50},
                                                std::chrono::duration_cast<std::chrono::milliseconds>(
                                                    until - std::chrono::steady_clock::now()) +
                                                    std::chrono::milliseconds{1}));
    }
    return true;
}

/// Guest frames presented so far.
u64 PresentedFrames();
/// Waits until `frames` more frames are presented. Returns the number actually presented
/// (fewer on timeout, e.g. when paused or loading).
u64 WaitFrames(u64 frames, std::chrono::milliseconds timeout);

} // namespace Mcp

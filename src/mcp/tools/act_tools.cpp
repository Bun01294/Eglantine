// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// `act`: input, wait and observe in one call, so a "press A and see what happened" step costs one
// agent turn instead of four or five.

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <thread>

#include <fmt/format.h>

#include "common/fs/path_util.h"
#include "input_common/drivers/tas_input.h"
#include "input_common/drivers/touch_screen.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/main.h"
#include "mcp/context.h"
#include "mcp/control.h"
#include "mcp/host.h"
#include "mcp/server.h"
#include "mcp/timeline.h"
#include "mcp/tool_registry.h"
#include "mcp/tools/tool_helpers.h"

namespace Mcp {

namespace {

/// Whether the `capture` array names `what`.
bool Wants(const json& capture, std::string_view what) {
    for (const auto& c : capture) {
        if (c.get<std::string>() == what) {
            return true;
        }
    }
    return false;
}

/// Screen labels and probes are not ported to this build: act reports this for them.
constexpr const char* kNotInThisBuild = "not available in this build";

/// Longest a single act call waits for events, frames, screens and time together.
constexpr auto kMaxTotalWait = std::chrono::seconds{60};

/// Input from concurrent act calls must not interleave on the virtual gamepad. `act` is annotated
/// read-only (so a long observe-only wait does not hold the global mutating-tools mutex), which
/// is why the input part takes its own lock.
std::mutex g_act_input_mutex;

/// act tool: runs optional input, waits (event, screen, frames, then ms, each with its own
/// timeout), then captures what was asked for. Blocks the calling thread for the whole wait.
/// Input needs writes and the control lease; a timed-out wait is reported, not an error.
ToolResult Act(ToolContext& ctx, const json& args) {
    using namespace std::chrono;
    const auto started = steady_clock::now();
    const bool has_input = args.contains("input");

    // Input changes emulator state: same gates as the input tools, checked here because act is
    // also useful read-only (wait + capture). Writes are asked live, not from the copy taken
    // when the call arrived.
    if (has_input) {
        if (auto refused = RequireWritesAndControl(ctx)) {
            refused->AddText("(act without input works without writes or the lease.)");
            return std::move(*refused);
        }
    }

    const u64 since_seq = ctx.timeline ? ctx.timeline->LastSeq() : 0;
    const u64 frame_before = PresentedFrames();
    json out;

    if (has_input) {
        const size_t npad = NpadIndex(args);
        std::scoped_lock lock{g_act_input_mutex};
        if (std::string error = RunInputSteps(ctx, npad, args["input"]); !error.empty()) {
            return ToolResult::Error("input failed: " + error);
        }
    }

    // Wait: for an event (if given), then for the screen, frames, then wall time. They share one
    // 60 s budget so a call cannot hold a worker thread for minutes.
    const json wait = args.value("wait", json{{"frames", 10}});
    const auto wait_deadline = steady_clock::now() + kMaxTotalWait;
    const auto budget = [&](int requested_ms) {
        const auto left = duration_cast<milliseconds>(wait_deadline - steady_clock::now());
        return std::clamp(milliseconds{requested_ms}, milliseconds{0},
                          std::max(left, milliseconds{0}));
    };
    json waited = json::object();
    if (wait.contains("event") && ctx.timeline && !ServerStopping()) {
        Timeline::Query q = ParseQuery(wait["event"]);
        q.since_seq = since_seq;
        const auto timeout = budget(wait["event"].value("timeout_ms", 10000));
        if (const auto found = ctx.timeline->WaitFor(q, timeout)) {
            waited["event"] = EventJson(*found);
        } else {
            waited["event"] = nullptr;
            waited["event_timed_out"] = true;
        }
    }
    if (wait.contains("screen")) {
        // Screen labels (screen_state) are not part of this build.
        waited["screen"] = nullptr;
        waited["screen_error"] = kNotInThisBuild;
    }
    if (wait.contains("frames") && !ServerStopping()) {
        const u64 want = wait["frames"].get<u64>();
        const auto timeout = budget(wait.value("frames_timeout_ms", 20000));
        const u64 got = WaitFrames(want, timeout);
        waited["frames"] = got;
        if (got < want) {
            waited["frames_timed_out"] = true; // paused, loading or very slow
        }
    }
    if (wait.contains("ms")) {
        SleepUnlessStopping(budget(wait["ms"].get<int>()));
        waited["ms"] = wait["ms"];
    }
    out["waited"] = std::move(waited);

    const json capture = args.value("capture", json::array({"screenshot", "timeline", "perf"}));
    ToolResult result;

    if (Wants(capture, "timeline") && ctx.timeline) {
        json filter = args.value("timeline", json::object());
        if (!filter.contains("min_level")) {
            filter["min_level"] = "warning"; // info logs drown everything else
        }
        Timeline::Query q = ParseQuery(filter);
        q.since_seq = since_seq;
        q.limit = filter.value("limit", size_t{60});
        const auto r = ctx.timeline->Read(q);
        json events = json::array();
        for (const auto& e : r.events) {
            events.push_back(EventJson(e));
        }
        out["timeline"] = {{"events", std::move(events)}, {"next_seq", r.next_seq}};
        if (r.truncated) {
            out["timeline"]["truncated"] = true;
        }
        if (r.dropped) {
            out["timeline"]["dropped"] = r.dropped;
        }
    }
    if (Wants(capture, "controller")) {
        out["controller"] = ControllerStateJson(ctx, NpadIndex(args));
    }
    if (Wants(capture, "perf")) {
        out["perf"] = PerfJson(ctx);
    }
    if (Wants(capture, "screen_state")) {
        out["screen_state"] = json{{"error", kNotInThisBuild}};
    }
    if (Wants(capture, "probes")) {
        out["probes"] = json{{"error", kNotInThisBuild}};
    }
    out["frames"] = {{"before", frame_before}, {"after", PresentedFrames()}};

    if (Wants(capture, "screenshot")) {
        if (!ctx.host) {
            out["screenshot_error"] = "no frontend host";
        } else {
            std::string error;
            const u32 max_width = args.value("max_width", 960U);
            if (const auto png = CaptureScreenshot(ctx, max_width, milliseconds{5000}, error)) {
                result.AddImage(*png, "image/png");
            } else {
                out["screenshot_error"] = error;
            }
        }
    }
    out["elapsed_ms"] = duration_cast<milliseconds>(steady_clock::now() - started).count();

    // Structured data first (as text too), then the image.
    ToolResult structured = ToolResult::Structured(std::move(out));
    for (auto& block : result.content) {
        structured.content.push_back(std::move(block));
    }
    return structured;
}

} // namespace

std::string DataRelativePath(const std::filesystem::path& path) {
    const auto base = Common::FS::GetEdenPath(Common::FS::EdenPath::EdenDir);
    auto relative = path.lexically_relative(base);
    if (relative.empty() || *relative.begin() == "..") {
        relative = path.filename();
    }
    std::string text = Common::FS::PathToUTF8String(relative);
    std::replace(text.begin(), text.end(), '\\', '/');
    return text;
}

std::string WriteGateError(ToolContext& ctx) {
    // A call without a session is the user's own action (the Routes menu runs tools through
    // Server::CallTool; MCP requests always carry a session): neither writes nor the lease apply.
    if (ctx.session_id.empty()) {
        return {};
    }
    if (!ctx.WritesAllowed()) {
        return "this needs MCP writes (Tools > MCP Server > Allow Writes)";
    }
    if (ctx.control) {
        ControlLease::Holder holder;
        if (!ctx.control->Touch(ctx.session_id, holder)) {
            const auto left = std::chrono::duration_cast<std::chrono::seconds>(
                                  holder.expires - ControlLease::Clock::now())
                                  .count();
            return fmt::format(
                "Another agent controls the emulator ({}session {}..., lease expires in {} s "
                "unless it keeps acting). Call control_status to check, or acquire_control once "
                "it is free.",
                holder.label.empty() ? "" : holder.label + ", ", holder.session_id.substr(0, 8),
                left);
        }
    }
    return {};
}

std::optional<ToolResult> RequireWritesAndControl(ToolContext& ctx) {
    if (std::string error = WriteGateError(ctx); !error.empty()) {
        return ToolResult::Error(std::move(error));
    }
    return std::nullopt;
}

ToolResult NeedsWritesError(std::string_view what) {
    return ToolResult::Error(
        fmt::format("{} needs MCP writes (Tools > MCP Server > Allow Writes)", what));
}

void StopWriteWork([[maybe_unused]] Host& host, [[maybe_unused]] Core::System& system) {
    // Called on the frontend's UI thread (the Allow Writes toggle): the threads below are only
    // asked to stop, never joined, because they may be waiting for that very thread (screenshots
    // and frame captures are marshalled to it).
    if (auto* input = host.GetInputSubsystem()) {
        // The TAS engine is not synchronized; its hotkeys run on this thread too, so calling it
        // directly is safe here (Host::WithTas would block on the UI thread waiting for itself).
        if (auto* tas = input->GetTas()) {
            const auto [state, frame, lengths] = tas->GetStatus();
            if (state == InputCommon::TasInput::TasState::Recording) {
                tas->Record(); // toggles off; the recording is not saved
            } else if (state == InputCommon::TasInput::TasState::Running) {
                tas->Stop();
            }
        }
        if (auto* pad = input->GetVirtualGamepad()) {
            pad->ResetControllers();
        }
        if (auto* touch = input->GetTouchScreen()) {
            touch->ReleaseAllTouch();
        }
    }
}

void RegisterActTools(ToolRegistry& registry) {
    json event_filter = FilterProperties();
    event_filter.erase("since_seq");
    event_filter["timeout_ms"] = {{"type", "integer"},
                                  {"minimum", 0},
                                  {"maximum", 60000},
                                  {"description", "How long to wait for it (default 10000)."}};

    json timeline_filter = FilterProperties();
    timeline_filter.erase("since_seq");
    timeline_filter["limit"] = {{"type", "integer"},
                                {"minimum", 1},
                                {"maximum", 500},
                                {"description", "Max events (default 60)."}};

    registry.Add(Tool{
        .name = "act",
        .title = "Act and observe",
        .description =
            "One call for 'do something and show me what happened': runs optional input steps "
            "(same format as input_sequence), waits (for a timeline event, then N guest frames, "
            "then ms; default 10 frames), and returns what you ask for in capture: a screenshot, "
            "the timeline events since the call started (logs at warning+ by default), controller "
            "state, perf. Without input it needs no writes or control lease, so it doubles as "
            "'wait and look'.",
        .input_schema =
            {{"type", "object"},
             {"properties",
              {{"input", InputStepsSchema()},
               {"player", PlayerSchema()},
               {"wait",
                {{"type", "object"},
                 {"properties",
                  {{"event",
                    {{"type", "object"},
                     {"properties", event_filter},
                     {"additionalProperties", false},
                     {"description", "Wait for the first timeline event matching these filters."}}},
                   {"frames",
                    {{"type", "integer"},
                     {"minimum", 0},
                     {"maximum", 3600},
                     {"description", "Guest frames to wait (frame-locked, not wall time)."}}},
                   {"frames_timeout_ms", {{"type", "integer"}, {"minimum", 0}, {"maximum", 60000}}},
                   {"screen",
                    {{"type", "array"},
                     {"maxItems", 32},
                     {"items", {{"type", "string"}, {"maxLength", 64}}},
                     {"description", "Screen labels: not available in this build."}}},
                   {"screen_timeout_ms", {{"type", "integer"}, {"minimum", 0}, {"maximum", 60000}}},
                   {"ms", {{"type", "integer"}, {"minimum", 0}, {"maximum", 60000}}}}},
                 {"additionalProperties", false}}},
               {"capture",
                {{"type", "array"},
                 {"maxItems", 8},
                 {"items",
                  {{"type", "string"},
                   {"enum",
                    {"screenshot", "timeline", "controller", "perf", "screen_state", "probes"}}}},
                 {"description", "Default [screenshot, timeline, perf]. screen_state and probes "
                                 "are not available in this build."}}},
               {"timeline",
                {{"type", "object"},
                 {"properties", timeline_filter},
                 {"additionalProperties", false},
                 {"description", "Filters for the captured timeline events."}}},
               {"max_width",
                {{"type", "integer"},
                 {"minimum", 160},
                 {"maximum", 3840},
                 {"description", "Screenshot width cap (default 960)."}}}}},
             {"additionalProperties", false}},
        // Annotated read-only on purpose: observe-only calls must not hold the global
        // mutating-tools mutex while they wait. The input part gates itself (writes + lease).
        .annotations = {.read_only = true},
        .requires_game = true,
        .handler = Act,
    });
}

} // namespace Mcp

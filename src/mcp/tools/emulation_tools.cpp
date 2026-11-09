// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Lifecycle and screen tools, implemented through the frontend Host.

#include <algorithm>
#include <cctype>

#include <fmt/format.h>

#include "common/stb.h"
#include "core/core.h"
#include "mcp/context.h"
#include "mcp/host.h"
#include "mcp/timeline.h"
#include "mcp/tool_registry.h"
#include "mcp/tools/tool_helpers.h"
#include "video_core/capture.h"
#include "video_core/gpu.h"
#include "video_core/textures/decoders.h"

namespace Mcp {

namespace {

ToolResult HostMissing() {
    return ToolResult::Error("no frontend host is attached");
}

/// Shared handler for pause/resume: calls the given Host action and logs a State event on
/// success. `what` names the action in the reply and the timeline.
ToolResult Lifecycle(ToolContext& ctx, std::string (Host::*action)(), std::string_view what) {
    if (!ctx.host) {
        return HostMissing();
    }
    if (const std::string error = (ctx.host->*action)(); !error.empty()) {
        return ToolResult::Error(fmt::format("{} failed: {}", what, error));
    }
    if (ctx.timeline) {
        ctx.timeline->Push(EventKind::State, 0, "mcp", std::string{what});
    }
    return ToolResult::Text(fmt::format("{}: ok", what));
}

/// screenshot: PNG of the next presented frame plus the guest present count at reply time.
ToolResult Screenshot(ToolContext& ctx, const json& args) {
    if (!ctx.host) {
        return HostMissing();
    }
    const u32 max_width = args.value("max_width", 1280U);
    const auto timeout = std::chrono::milliseconds{args.value("timeout_ms", 5000)};
    std::string error;
    const auto png = ctx.host->CaptureScreenshotPng(max_width, timeout, error);
    if (!png) {
        return ToolResult::Error("screenshot failed: " + error);
    }
    ToolResult result;
    result.AddImage(*png, "image/png");
    result.AddText(fmt::format("guest frame {}",
                               Core::g_nx2_present_count.load(std::memory_order_relaxed)));
    return result;
}

/// Runs N frames then pauses again (resume, count presented frames, pause). The pause lands on
/// the frame after the Nth presentation at best; expect +/- a frame or two.
ToolResult FrameAdvance(ToolContext& ctx, const json& args) {
    if (!ctx.host) {
        return HostMissing();
    }
    const u64 frames = std::clamp<u64>(args.value("frames", u64{1}), 1, 600);
    if (!ctx.system->IsPaused()) {
        if (std::string error = ctx.host->Pause(); !error.empty()) {
            return ToolResult::Error("pause failed: " + error);
        }
    }
    const u64 start = PresentedFrames();
    if (std::string error = ctx.host->Resume(); !error.empty()) {
        return ToolResult::Error("resume failed: " + error);
    }
    const u64 got = WaitFrames(
        frames, std::chrono::milliseconds{std::clamp(args.value("timeout_ms", 10000), 100, 60000)});
    ctx.host->Pause();
    ToolResult result = ToolResult::Structured(
        json{{"requested", frames}, {"advanced", PresentedFrames() - start}, {"counted", got},
             {"paused", true}});
    if (args.value("screenshot", true)) {
        std::string error;
        // Paused: the renderer only fills screenshots on present, so use the capture layer.
        if (const auto png = RawCapturePng(ctx, 640, error)) {
            result.AddImage(*png, "image/png");
        } else {
            result.AddText("no screenshot: " + error);
        }
    }
    return result;
}

/// Total time one capture_frames call may take, however many frames it waits for.
constexpr auto kCaptureBudget = std::chrono::seconds{60};

/// capture_frames: takes `count` screenshots, waiting every_frames presents (up to 20 s) between
/// them. A failed capture is reported in its slot and does not stop the burst.
ToolResult CaptureFrames(ToolContext& ctx, const json& args) {
    if (!ctx.host) {
        return HostMissing();
    }
    const u64 every = args.value("every_frames", u64{30});
    const int count = std::clamp(args.value("count", 4), 1, 12);
    const u32 width = args.value("max_width", 480U);
    const auto deadline = std::chrono::steady_clock::now() + kCaptureBudget;
    const auto remaining = [&] {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
    };
    ToolResult images;
    json frames = json::array();
    bool truncated = false;
    for (int i = 0; i < count; ++i) {
        if (ServerStopping() || remaining() <= std::chrono::milliseconds{0}) {
            truncated = true;
            break;
        }
        if (i > 0) {
            WaitFrames(every, std::min(std::chrono::milliseconds{20000}, remaining()));
            if (ServerStopping() || remaining() <= std::chrono::milliseconds{0}) {
                truncated = true;
                break;
            }
        }
        std::string error;
        if (const auto png = ctx.host->CaptureScreenshotPng(
                width, std::min(std::chrono::milliseconds{3000}, remaining()), error)) {
            images.AddImage(*png, "image/png");
            frames.push_back(PresentedFrames());
        } else {
            frames.push_back(json{{"error", error}});
        }
    }
    json summary{{"frames", std::move(frames)}};
    if (truncated) {
        summary["truncated"] = true;
    }
    ToolResult result = ToolResult::Structured(std::move(summary));
    for (auto& block : images.content) {
        result.content.push_back(std::move(block));
    }
    return result;
}

/// The last frame of the applet capture layer: 1280x720, no post-processing, works with the
/// window hidden or the game paused.
std::optional<Frame> RawCapture(ToolContext& ctx, std::string& error) {
    namespace Cap = VideoCore::Capture;
    // The capture is a sync operation on the GPU thread, which no longer runs once the game has
    // stopped: it would block forever.
    if (!ctx.GameRunning()) {
        error = "no game is running";
        return std::nullopt;
    }
    const std::vector<u8> tiled = ctx.system->GPU().GetAppletCaptureBuffer();
    if (tiled.size() < Cap::TiledSize) {
        error = "applet capture buffer unavailable";
        return std::nullopt;
    }
    Frame frame{.width = Cap::LinearWidth, .height = Cap::LinearHeight};
    frame.bgra.resize(size_t{frame.width} * frame.height * Cap::BytesPerPixel);
    Tegra::Texture::UnswizzleTexture(frame.bgra, tiled, Cap::BytesPerPixel, Cap::LinearWidth,
                                     Cap::LinearHeight, Cap::LinearDepth, Cap::BlockHeight,
                                     Cap::BlockDepth);
    if (std::all_of(frame.bgra.begin(), frame.bgra.end(), [](u8 b) { return b == 0; })) {
        error = "applet capture buffer is empty";
        return std::nullopt;
    }
    // The layer holds R, G, B, A bytes (despite Capture::PixelFormat): swap to B, G, R, A.
    for (size_t i = 0; i + 3 < frame.bgra.size(); i += 4) {
        std::swap(frame.bgra[i], frame.bgra[i + 2]);
    }
    return frame;
}

/// PNG of a BGRA frame scaled to at most max_width.
std::vector<u8> EncodePng(const Frame& frame, u32 max_width) {
    u32 w = frame.width;
    u32 h = frame.height;
    std::vector<u8> rgba(frame.bgra.size());
    for (size_t i = 0; i + 3 < frame.bgra.size(); i += 4) {
        rgba[i + 0] = frame.bgra[i + 2];
        rgba[i + 1] = frame.bgra[i + 1];
        rgba[i + 2] = frame.bgra[i + 0];
        rgba[i + 3] = 255;
    }
    if (max_width < w) {
        const u32 nh = h * max_width / w;
        std::vector<u8> scaled(size_t{max_width} * nh * 4);
        stbir_resize_uint8(rgba.data(), static_cast<int>(w), static_cast<int>(h), 0,
                           scaled.data(), static_cast<int>(max_width), static_cast<int>(nh), 0, 4);
        rgba = std::move(scaled);
        w = max_width;
        h = nh;
    }
    std::vector<u8> png;
    stbi_write_png_to_func(
        [](void* context, void* data, int size) {
            auto* out = static_cast<std::vector<u8>*>(context);
            const auto* bytes = static_cast<const u8*>(data);
            out->insert(out->end(), bytes, bytes + size);
        },
        &png, static_cast<int>(w), static_cast<int>(h), 4, rgba.data(), static_cast<int>(w * 4));
    return png;
}


} // namespace

std::optional<std::vector<u8>> RawCapturePng(ToolContext& ctx, u32 max_width, std::string& error) {
    const auto frame = RawCapture(ctx, error);
    if (!frame) {
        return std::nullopt;
    }
    return EncodePng(*frame, max_width);
}

void RegisterEmulationTools(ToolRegistry& registry) {
    registry.Add(Tool{
        .name = "frame_advance",
        .title = "Advance frames",
        .description = "Pause (if needed), run N presented frames, pause again, and return a "
                       "screenshot. Good for stepping through an effect or a glitch; accuracy is "
                       "about a frame (pausing isn't frame-locked).",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"frames", {{"type", "integer"}, {"minimum", 1}, {"maximum", 600}}},
                           {"timeout_ms", {{"type", "integer"}, {"minimum", 100}, {"maximum", 60000}}},
                           {"screenshot", {{"type", "boolean"}}}}},
                         {"additionalProperties", false}},
        .requires_game = true,
        .requires_write = true,
        .requires_control = true,
        .handler = FrameAdvance,
    });
    registry.Add(Tool{
        .name = "capture_frames",
        .title = "Capture a frame burst",
        .description = "Take count screenshots (max 12), one every every_frames presented frames: "
                       "a cheap substitute for video when watching an animation or a glitch.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"every_frames", {{"type", "integer"}, {"minimum", 1}, {"maximum", 3600}}},
                           {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 12}}},
                           {"max_width", {{"type", "integer"}, {"minimum", 160}, {"maximum", 1280}}}}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true},
        .requires_game = true,
        .handler = CaptureFrames,
    });
    registry.Add(Tool{
        .name = "pause",
        .title = "Pause emulation",
        .description = "Pause the running game.",
        .annotations = {.idempotent = true},
        .requires_game = true,
        .requires_write = true,
        .requires_control = true,
        .handler = [](ToolContext& ctx, const json&) { return Lifecycle(ctx, &Host::Pause, "pause"); },
    });
    registry.Add(Tool{
        .name = "resume",
        .title = "Resume emulation",
        .description = "Resume a paused game.",
        .annotations = {.idempotent = true},
        .requires_game = true,
        .requires_write = true,
        .requires_control = true,
        .handler = [](ToolContext& ctx, const json&) { return Lifecycle(ctx, &Host::Resume, "resume"); },
    });
    registry.Add(Tool{
        .name = "screenshot",
        .title = "Screenshot",
        .description = "PNG of the next presented frame (as displayed, including post-processing), "
                       "scaled to max_width (16:9). Fails if no frame is presented within "
                       "timeout_ms, e.g. while paused or loading.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"max_width", {{"type", "integer"}, {"minimum", 160}, {"maximum", 3840}}},
                           {"timeout_ms", {{"type", "integer"}, {"minimum", 100}, {"maximum", 60000}}}}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true},
        .requires_game = true,
        .handler = Screenshot,
    });
}

} // namespace Mcp

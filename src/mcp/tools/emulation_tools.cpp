// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Lifecycle and screen tools, implemented through the frontend Host.

#include <algorithm>
#include <cctype>
#include <mutex>

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
    const auto png = CaptureScreenshot(ctx, max_width, timeout, error);
    if (!png) {
        // Paused: nothing is presented, so no screenshot request is ever filled.
        if (ctx.system != nullptr && ctx.system->IsPaused()) {
            std::string source;
            std::string fallback_error;
            if (const auto fallback = FallbackFrame(ctx, max_width, source, fallback_error)) {
                ToolResult result;
                result.AddImage(*fallback, "image/png");
                result.AddText("paused, no frame presented; image: " + source);
                return result;
            }
            error += "; " + fallback_error;
        }
        return ToolResult::Error("screenshot failed: " + error);
    }
    ToolResult result;
    result.AddImage(*png, "image/png");
    result.AddText(fmt::format("guest frame {}",
                               Core::g_nx2_present_count.load(std::memory_order_relaxed)));
    return result;
}

/// Runs N frames then pauses again (resume, count presented frames, pause). The pause lands on
/// the frame after the Nth presentation at best; expect +/- a frame or two. The image is the Nth
/// frame, captured as it is presented, before pausing: renderers (the VTX-HLE replay too) only
/// fill screenshot requests on present, so none would arrive once paused. If that fails it falls
/// back to the capture layer, then to the last presented screenshot, and says which it used.
ToolResult FrameAdvance(ToolContext& ctx, const json& args) {
    using std::chrono::milliseconds;
    if (!ctx.host) {
        return HostMissing();
    }
    const u64 frames = std::clamp<u64>(args.value("frames", u64{1}), 1, 600);
    const bool want_image = args.value("screenshot", true);
    const auto deadline = std::chrono::steady_clock::now() +
                          milliseconds{std::clamp(args.value("timeout_ms", 10000), 100, 60000)};
    const auto remaining = [&] {
        return std::max(milliseconds{0}, std::chrono::duration_cast<milliseconds>(
                                             deadline - std::chrono::steady_clock::now()));
    };
    if (!ctx.system->IsPaused()) {
        if (std::string error = ctx.host->Pause(); !error.empty()) {
            return ToolResult::Error("pause failed: " + error);
        }
    }
    const u64 start = PresentedFrames();
    if (std::string error = ctx.host->Resume(); !error.empty()) {
        return ToolResult::Error("resume failed: " + error);
    }
    u64 got = 0;
    std::optional<std::vector<u8>> png;
    std::string shot_error;
    if (want_image) {
        if (frames > 1) {
            got = WaitFrames(frames - 1, remaining());
        }
        // Filled when the next (the Nth) frame is presented.
        png = CaptureScreenshot(ctx, 640, std::min(remaining(), milliseconds{5000}), shot_error);
        got += png ? u64{1} : WaitFrames(1, remaining());
    } else {
        got = WaitFrames(frames, remaining());
    }
    ctx.host->Pause();
    json out{{"requested", frames},
             {"advanced", PresentedFrames() - start},
             {"counted", got},
             {"paused", true}};
    std::string source;
    if (want_image) {
        if (png) {
            source = "presented frame (the last advanced frame, captured before pausing)";
        } else {
            std::string fallback_error;
            png = FallbackFrame(ctx, 640, source, fallback_error);
            source = png ? fmt::format("{} (presented-frame screenshot failed: {})", source,
                                       shot_error)
                         : fmt::format("none (presented-frame screenshot failed: {}; {})",
                                       shot_error, fallback_error);
        }
        out["image_source"] = source;
    }
    ToolResult result = ToolResult::Structured(std::move(out));
    if (want_image) {
        if (png) {
            result.AddImage(*png, "image/png");
        }
        result.AddText("image: " + source);
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
    if (ctx.system != nullptr && ctx.system->IsPaused()) {
        return ToolResult::Error(
            "emulation is paused: capture_frames waits for presented frames and none come while "
            "paused. Resume first, or step with frame_advance (it returns the frame it stops on).");
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
        if (const auto png = CaptureScreenshot(
                ctx, width, std::min(std::chrono::milliseconds{3000}, remaining()), error)) {
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

namespace {
/// The newest presented-frame screenshot any tool took (CaptureScreenshot), for FallbackFrame.
std::mutex g_last_screenshot_mutex;
std::optional<CachedScreenshot> g_last_screenshot;
} // namespace

void RememberScreenshot(std::vector<u8> png, u64 frame) {
    std::scoped_lock lock{g_last_screenshot_mutex};
    g_last_screenshot = CachedScreenshot{
        .png = std::move(png), .frame = frame, .taken = std::chrono::steady_clock::now()};
}

std::optional<CachedScreenshot> LastScreenshot() {
    std::scoped_lock lock{g_last_screenshot_mutex};
    return g_last_screenshot;
}

std::optional<std::vector<u8>> CaptureScreenshot(ToolContext& ctx, u32 max_width,
                                                 std::chrono::milliseconds timeout,
                                                 std::string& error) {
    if (!ctx.host) {
        error = "no frontend host is attached";
        return std::nullopt;
    }
    auto png = ctx.host->CaptureScreenshotPng(max_width, timeout, error);
    if (png) {
        RememberScreenshot(*png, PresentedFrames());
    }
    return png;
}

std::optional<std::vector<u8>> FallbackFrame(ToolContext& ctx, u32 max_width, std::string& source,
                                             std::string& error) {
    // The VTX-HLE replay presents through its own chain and never renders the applet capture
    // layer, so the capture layer is empty there; the emulated GPU path fills it.
    std::string raw_error;
    if (auto png = RawCapturePng(ctx, max_width, raw_error)) {
        source = "capture layer (applet capture buffer)";
        return png;
    }
    if (auto last = LastScreenshot()) {
        const auto age = std::chrono::duration_cast<std::chrono::milliseconds>(
                             std::chrono::steady_clock::now() - last->taken)
                             .count();
        source = fmt::format("last presented screenshot (guest frame {}, taken {} ms ago; capture "
                             "layer: {})",
                             last->frame, age, raw_error);
        return std::move(last->png);
    }
    error = fmt::format("capture layer: {}; no presented screenshot taken yet", raw_error);
    return std::nullopt;
}

void RegisterEmulationTools(ToolRegistry& registry) {
    registry.Add(Tool{
        .name = "frame_advance",
        .title = "Advance frames",
        .description = "Pause (if needed), run N presented frames, pause again, and return an "
                       "image of the last advanced frame (image_source says where it came from). "
                       "Good for stepping through an effect or a glitch; accuracy is about a "
                       "frame (pausing isn't frame-locked).",
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
                       "a cheap substitute for video when watching an animation or a glitch. "
                       "Refused while paused (use frame_advance).",
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
                       "timeout_ms (e.g. loading); while paused it returns the capture layer or "
                       "the last presented screenshot and says which.",
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

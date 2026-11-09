// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Controller input through the virtual gamepad engine, which every emulated controller always
// listens to on its own port (no remapping needed), plus readback of what HID actually sees.

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <thread>

#include <fmt/format.h>

#include "core/core.h"
#include "hid_core/frontend/emulated_controller.h"
#include "hid_core/hid_core.h"
#include "input_common/drivers/touch_screen.h"
#include "input_common/drivers/virtual_gamepad.h"
#include "input_common/main.h"
#include "mcp/context.h"
#include "mcp/host.h"
#include "mcp/timeline.h"
#include "mcp/tool_registry.h"
#include "mcp/tools/tool_helpers.h"

namespace Mcp {

namespace {

using VirtualButton = InputCommon::VirtualGamepad::VirtualButton;
using VirtualStick = InputCommon::VirtualGamepad::VirtualStick;

/// Button names accepted by the tools, mapped to virtual gamepad buttons.
constexpr std::array<std::pair<std::string_view, VirtualButton>, 20> kButtons{{
    {"A", VirtualButton::ButtonA},
    {"B", VirtualButton::ButtonB},
    {"X", VirtualButton::ButtonX},
    {"Y", VirtualButton::ButtonY},
    {"LSTICK", VirtualButton::StickL},
    {"RSTICK", VirtualButton::StickR},
    {"L", VirtualButton::TriggerL},
    {"R", VirtualButton::TriggerR},
    {"ZL", VirtualButton::TriggerZL},
    {"ZR", VirtualButton::TriggerZR},
    {"PLUS", VirtualButton::ButtonPlus},
    {"MINUS", VirtualButton::ButtonMinus},
    {"DLEFT", VirtualButton::ButtonLeft},
    {"DUP", VirtualButton::ButtonUp},
    {"DRIGHT", VirtualButton::ButtonRight},
    {"DDOWN", VirtualButton::ButtonDown},
    {"SL", VirtualButton::ButtonSL},
    {"SR", VirtualButton::ButtonSR},
    {"HOME", VirtualButton::ButtonHome},
    {"CAPTURE", VirtualButton::ButtonCapture},
}};

/// JSON array of every button name in kButtons, for schema enums.
json ButtonEnum() {
    json names = json::array();
    for (const auto& [name, button] : kButtons) {
        names.push_back(name);
    }
    return names;
}

/// Looks up a button by its exact (upper case) name. Callers dereference the result directly,
/// relying on the schema enum to have rejected unknown names.
std::optional<VirtualButton> ParseButton(std::string_view name) {
    for (const auto& [n, button] : kButtons) {
        if (n == name) {
            return button;
        }
    }
    return std::nullopt;
}

/// Returns the frontend's virtual gamepad, or nullptr with error set if there is no host or
/// input subsystem.
InputCommon::VirtualGamepad* Gamepad(ToolContext& ctx, std::string& error) {
    if (!ctx.host) {
        error = "no frontend host";
        return nullptr;
    }
    auto* input = ctx.host->GetInputSubsystem();
    if (!input) {
        error = "this frontend has no input subsystem";
        return nullptr;
    }
    return input->GetVirtualGamepad();
}

/// Sleeps for ms clamped to 0..60000, returning early if the server is stopping.
/// Sleeps `ms`; returns false as soon as Allow Writes is switched off or the server stops, so a
/// long hold cannot outlive the user's revocation (the caller releases what it pressed).
bool InputSleep(ToolContext& ctx, int ms) {
    const auto until = std::chrono::steady_clock::now() +
                       std::chrono::milliseconds{std::clamp(ms, 0, 60000)};
    for (;;) {
        if (ServerStopping() || !ctx.WritesAllowed()) {
            return false;
        }
        const auto left = until - std::chrono::steady_clock::now();
        if (left <= std::chrono::steady_clock::duration::zero()) {
            return true;
        }
        std::this_thread::sleep_for(
            std::min<std::chrono::steady_clock::duration>(left, std::chrono::milliseconds{20}));
    }
}

std::string StoppedReason() {
    return ServerStopping() ? "stopped: the MCP server is shutting down"
                            : "stopped: writes disabled (Tools > MCP Server > Allow Writes)";
}

/// Records an Input event on the timeline, if one is attached.
void LogInput(ToolContext& ctx, const std::string& what) {
    if (ctx.timeline) {
        ctx.timeline->Push(EventKind::Input, 0, "mcp", what);
    }
}

json StickArray() {
    return json{{"type", "array"},
                {"items", {{"type", "number"}, {"minimum", -1}, {"maximum", 1}}},
                {"minItems", 2},
                {"maxItems", 2},
                {"description", "[x, y], each in -1..1 (up is +y)."}};
}

/// Joins a JSON array of button names with "+" (e.g. "ZL+ZR") for log and reply text.
std::string JoinButtons(const json& buttons) {
    std::string out;
    for (const auto& b : buttons) {
        out += (out.empty() ? "" : "+") + b.get<std::string>();
    }
    return out;
}

/// press_buttons: holds all buttons together for hold_ms, releases them, and repeats with gap_ms
/// between presses. Blocks the calling worker thread for the whole duration.
ToolResult PressButtons(ToolContext& ctx, const json& args) {
    std::string error;
    auto* pad = Gamepad(ctx, error);
    if (!pad) {
        return ToolResult::Error(error);
    }
    const size_t npad = NpadIndex(args);
    std::vector<VirtualButton> buttons;
    for (const auto& b : args.at("buttons")) {
        buttons.push_back(*ParseButton(b.get<std::string>()));
    }
    const int hold_ms = args.value("hold_ms", 120);
    const int repeat = args.value("repeat", 1);
    const int gap_ms = args.value("gap_ms", 120);
    const auto release = [&] {
        for (const auto b : buttons) {
            pad->SetButtonState(npad, b, false);
        }
    };
    for (int i = 0; i < repeat; ++i) {
        // Writes can be revoked or the lease lost between presses: ask again before each one.
        if (std::string refused = WriteGateError(ctx); !refused.empty()) {
            return ToolResult::Error("stopped: " + refused);
        }
        for (const auto b : buttons) {
            pad->SetButtonState(npad, b, true);
        }
        const bool held = InputSleep(ctx, hold_ms);
        release();
        if (!held || (i + 1 < repeat && !InputSleep(ctx, gap_ms))) {
            return ToolResult::Error(StoppedReason());
        }
    }
    LogInput(ctx, fmt::format("press {} x{} hold {}ms (player npad {})",
                              JoinButtons(args.at("buttons")), repeat, hold_ms, npad));
    return ToolResult::Text(fmt::format("pressed {} x{}", JoinButtons(args.at("buttons")), repeat));
}

/// set_stick: moves a stick and either recenters it after hold_ms or, with hold_ms 0, leaves it
/// latched until the next call.
ToolResult SetStick(ToolContext& ctx, const json& args) {
    std::string error;
    auto* pad = Gamepad(ctx, error);
    if (!pad) {
        return ToolResult::Error(error);
    }
    const size_t npad = NpadIndex(args);
    const auto stick = args.at("stick").get<std::string>() == "right" ? VirtualStick::Right
                                                                      : VirtualStick::Left;
    const float x = args.at("x").get<float>();
    const float y = args.at("y").get<float>();
    const int hold_ms = args.value("hold_ms", 0);
    pad->SetStickPosition(npad, stick, x, y);
    if (hold_ms > 0) {
        const bool held = InputSleep(ctx, hold_ms);
        pad->SetStickPosition(npad, stick, 0.0f, 0.0f);
        if (!held) {
            return ToolResult::Error(StoppedReason());
        }
    }
    LogInput(ctx, fmt::format("stick {} ({:.2f},{:.2f}) {}", args.at("stick").get<std::string>(),
                              x, y, hold_ms > 0 ? fmt::format("for {}ms", hold_ms) : "latched"));
    return ToolResult::Text(hold_ms > 0 ? "stick moved and released"
                                        : "stick latched (call again with 0,0 to center)");
}

/// Error text for steps that are not what InputStepsSchema describes. Routes come from files
/// the user can edit by hand, so the handlers do not trust the schema alone. "" when fine.
std::string ValidateSteps(const json& steps) {
    // Route steps (hand-editable files) may be longer than the tool schema's 200; the 60 s total
    // in RunInputSteps still bounds a run.
    if (!steps.is_array() || steps.size() > 4000) {
        return "input steps must be an array of at most 4000 steps";
    }
    const auto bad_stick = [](const json& v) {
        return !v.is_array() || v.size() != 2 || !v[0].is_number() || !v[1].is_number() ||
               std::abs(v[0].get<double>()) > 1.0 || std::abs(v[1].get<double>()) > 1.0;
    };
    const auto bad_ms = [](const json& step, const char* key) {
        if (!step.contains(key)) {
            return false;
        }
        const json& v = step[key];
        return !v.is_number_integer() || v.get<long long>() < 0 || v.get<long long>() > 30000;
    };
    for (const auto& step : steps) {
        if (!step.is_object()) {
            return "input step is not an object";
        }
        if (step.contains("buttons")) {
            if (!step["buttons"].is_array() || step["buttons"].size() > 20) {
                return "input step: buttons must be an array";
            }
            for (const auto& b : step["buttons"]) {
                if (!b.is_string() || !ParseButton(b.get<std::string>())) {
                    return "input step: unknown button";
                }
            }
        }
        for (const char* stick : {"left", "right"}) {
            if (step.contains(stick) && bad_stick(step[stick])) {
                return "input step: sticks are [x, y] numbers in -1..1";
            }
        }
        if (bad_ms(step, "hold_ms") || bad_ms(step, "wait_ms")) {
            return "input step: hold_ms/wait_ms are integers in 0..30000";
        }
    }
    return {};
}

/// Total wall-clock length of a step list in ms, using the same defaults as RunInputSteps.
long long StepsDurationMs(const json& steps) {
    long long total_ms = 0;
    for (const auto& step : steps) {
        total_ms += step.value("hold_ms", 120) + step.value("wait_ms", 0);
    }
    return total_ms;
}

ToolResult InputSequence(ToolContext& ctx, const json& args) {
    const auto& steps = args.at("steps");
    if (std::string error = RunInputSteps(ctx, NpadIndex(args), steps); !error.empty()) {
        return ToolResult::Error(error);
    }
    return ToolResult::Text(
        fmt::format("ran {} steps in ~{} ms", steps.size(), StepsDurationMs(steps)));
}

/// release_all: releases every virtual button and centers all sticks on every player.
ToolResult ReleaseAll(ToolContext& ctx, const json&) {
    std::string error;
    auto* pad = Gamepad(ctx, error);
    if (!pad) {
        return ToolResult::Error(error);
    }
    pad->ResetControllers();
    LogInput(ctx, "release all");
    return ToolResult::Text("all virtual buttons released and sticks centered");
}

} // namespace

json PlayerSchema() {
    return json{{"description", "1-8 for players 1-8 (default 1), or 9 for handheld mode."},
                {"type", "integer"},
                {"minimum", 1},
                {"maximum", 9}};
}

size_t NpadIndex(const json& args) {
    const int player = args.value("player", 1);
    return static_cast<size_t>(player - 1); // 9 -> 8 = handheld
}

json InputStepsSchema() {
    return json{{"type", "array"},
                {"minItems", 1},
                {"maxItems", 200},
                {"items",
                 {{"type", "object"},
                  {"properties",
                   {{"buttons", {{"type", "array"},
                                 {"maxItems", 20},
                                 {"items", {{"type", "string"}, {"enum", ButtonEnum()}}}}},
                    {"left", StickArray()},
                    {"right", StickArray()},
                    {"hold_ms", {{"type", "integer"}, {"minimum", 0}, {"maximum", 30000}}},
                    {"wait_ms", {{"type", "integer"}, {"minimum", 0}, {"maximum", 30000}}}}},
                  {"additionalProperties", false}}}};
}

std::string RunInputSteps(ToolContext& ctx, size_t npad, const json& steps) {
    if (npad > 8) {
        return "player must be 1-9";
    }
    if (std::string invalid = ValidateSteps(steps); !invalid.empty()) {
        return invalid;
    }
    std::string error;
    auto* pad = Gamepad(ctx, error);
    if (!pad) {
        return error;
    }
    const long long total_ms = StepsDurationMs(steps);
    if (total_ms > 60000) {
        return "sequence longer than 60 s; split it into several calls";
    }
    for (const auto& step : steps) {
        // Writes can be revoked (or the lease lost) while a long sequence runs: ask again before
        // every input this function sends, which also renews the lease.
        if (std::string refused = WriteGateError(ctx); !refused.empty()) {
            return "stopped: " + refused;
        }
        std::vector<VirtualButton> buttons;
        for (const auto& b : step.value("buttons", json::array())) {
            buttons.push_back(*ParseButton(b.get<std::string>()));
        }
        for (const auto b : buttons) {
            pad->SetButtonState(npad, b, true);
        }
        if (step.contains("left")) {
            pad->SetStickPosition(npad, VirtualStick::Left, step["left"][0].get<float>(),
                                  step["left"][1].get<float>());
        }
        if (step.contains("right")) {
            pad->SetStickPosition(npad, VirtualStick::Right, step["right"][0].get<float>(),
                                  step["right"][1].get<float>());
        }
        const bool held = InputSleep(ctx, step.value("hold_ms", 120));
        for (const auto b : buttons) {
            pad->SetButtonState(npad, b, false);
        }
        if (step.contains("left")) {
            pad->SetStickPosition(npad, VirtualStick::Left, 0.0f, 0.0f);
        }
        if (step.contains("right")) {
            pad->SetStickPosition(npad, VirtualStick::Right, 0.0f, 0.0f);
        }
        if (!held || !InputSleep(ctx, step.value("wait_ms", 0))) {
            return StoppedReason();
        }
    }
    LogInput(ctx, fmt::format("sequence of {} steps ({} ms)", steps.size(), total_ms));
    return {};
}

std::string ValidateInputSteps(const json& steps) {
    return ValidateSteps(steps);
}

json ControllerStateJson(ToolContext& ctx, size_t npad) {
    if (!ctx.system) {
        return nullptr;
    }
    auto* controller = ctx.system->HIDCore().GetEmulatedControllerByIndex(npad);
    if (!controller) {
        return nullptr;
    }
    const auto state = controller->GetNpadButtons();
    json pressed = json::array();
    const u64 raw = static_cast<u64>(state.raw);
    const auto bit = [raw](u32 index) { return ((raw >> index) & 1) != 0; };
    static constexpr std::array<std::string_view, 16> names{
        "A", "B", "X", "Y", "LSTICK", "RSTICK", "L", "R",
        "ZL", "ZR", "PLUS", "MINUS", "DLEFT", "DUP", "DRIGHT", "DDOWN"};
    for (u32 i = 0; i < names.size(); ++i) {
        if (bit(i)) {
            pressed.push_back(names[i]);
        }
    }
    const auto sticks = controller->GetSticks();
    return json{
        {"player", npad == 8 ? "handheld" : std::to_string(npad + 1)},
        {"connected", controller->IsConnected()},
        {"style", static_cast<int>(controller->GetNpadStyleIndex())},
        {"buttons", pressed},
        {"left_stick", {sticks.left.x, sticks.left.y}},
        {"right_stick", {sticks.right.x, sticks.right.y}},
    };
}

void RegisterInputTools(ToolRegistry& registry) {
    const json button_list = {{"type", "array"},
                              {"items", {{"type", "string"}, {"enum", ButtonEnum()}}},
                              {"minItems", 1},
                              {"maxItems", 8}};

    registry.Add(Tool{
        .name = "press_buttons",
        .title = "Press buttons",
        .description = "Press and release controller buttons together (e.g. [\"A\"] or "
                       "[\"ZL\",\"ZR\"]). hold_ms is wall-clock: games read input once per frame, "
                       "so keep it >= 50 ms (default 120). Physical controllers keep working; the "
                       "last pressed source wins per button.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"buttons", button_list},
                           {"hold_ms", {{"type", "integer"}, {"minimum", 16}, {"maximum", 10000}}},
                           {"repeat", {{"type", "integer"}, {"minimum", 1}, {"maximum", 50}}},
                           {"gap_ms", {{"type", "integer"}, {"minimum", 16}, {"maximum", 10000}}},
                           {"player", PlayerSchema()}}},
                         {"required", {"buttons"}},
                         {"additionalProperties", false}},
        .annotations = {},
        .requires_game = true,
        .requires_write = true,
        .requires_control = true,
        .handler = PressButtons,
    });

    registry.Add(Tool{
        .name = "set_stick",
        .title = "Move a stick",
        .description = "Set the left or right stick to (x, y) in -1..1 (up is +y). With hold_ms "
                       "it returns to center afterwards; without, it stays there until moved again.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"stick", {{"type", "string"}, {"enum", {"left", "right"}}}},
                           {"x", {{"type", "number"}, {"minimum", -1}, {"maximum", 1}}},
                           {"y", {{"type", "number"}, {"minimum", -1}, {"maximum", 1}}},
                           {"hold_ms", {{"type", "integer"}, {"minimum", 0}, {"maximum", 30000}}},
                           {"player", PlayerSchema()}}},
                         {"required", {"stick", "x", "y"}},
                         {"additionalProperties", false}},
        .requires_game = true,
        .requires_write = true,
        .requires_control = true,
        .handler = SetStick,
    });

    registry.Add(Tool{
        .name = "input_sequence",
        .title = "Run an input sequence",
        .description = "Run timed steps in order, each pressing buttons and/or holding sticks for "
                       "hold_ms then waiting wait_ms (max 200 steps, 60 s total). For "
                       "frame-exact input, use the TAS tools (tas_write_script, tas_play).",
        .input_schema = {{"type", "object"},
                         {"properties", {{"steps", InputStepsSchema()}, {"player", PlayerSchema()}}},
                         {"required", {"steps"}},
                         {"additionalProperties", false}},
        .requires_game = true,
        .requires_write = true,
        .requires_control = true,
        .handler = InputSequence,
    });

    registry.Add(Tool{
        .name = "release_all",
        .title = "Release all inputs",
        .description = "Release every virtual button and center both sticks on all players.",
        .annotations = {.idempotent = true},
        .requires_game = true,
        .requires_write = true,
        .requires_control = true,
        .handler = ReleaseAll,
    });

    const json coord = {{"type", "number"}, {"minimum", 0}, {"maximum", 1}};
    const json point = {{"type", "array"}, {"items", coord}, {"minItems", 2}, {"maxItems", 2},
                        {"description", "[x, y] in 0..1 of the screen (0,0 = top left)."}};

}

} // namespace Mcp

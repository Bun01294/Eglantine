// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Determinism presets (applied by name through Settings::values.linkage, restricted to an
// allowlist of graphics/CPU/audio/timing settings) and the EDEN_ env switches in effect.
// Credentials, paths, network, logging and the MCP server's own settings are never exposed or
// changed.

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdlib>
#include <string_view>

#include <fmt/format.h>

#include "common/settings.h"
#include "mcp/context.h"
#include "mcp/host.h"
#include "mcp/server.h"
#include "mcp/timeline.h"
#include "mcp/tool_registry.h"
#include "mcp/tools/tool_helpers.h"

#ifdef _WIN32
#include <stdlib.h> // _environ
#else
extern char** environ;
#endif

namespace Mcp {

namespace {

/// ASCII lower-case copy of s, for case-insensitive matching.
std::string Lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// Settings are exposed by ALLOWLIST, not by hiding the dangerous ones: a setting added later
/// stays unreachable until someone decides it is safe here.
///
/// Allowed: whole categories that only tune how the guest is rendered, executed or heard
/// (Renderer, RendererAdvanced, RendererHacks, RendererExtensions, Cpu, CpuDebug, CpuUnsafe,
/// Audio, SystemAudio, Core) plus the System settings that make runs repeatable (RNG seed, RTC,
/// language, region, time zone, docked mode, fast CPU/GPU time).
///
/// Never allowed, whatever the category (checked by name on top of the allowlist): anything that
/// opens the GDB stub, the network or UDP, names a file or directory, dumps or logs (shaders,
/// NSO/ExeFS, macros, audio commands, FS access, GPU logs, log_filter), identity and secrets
/// (username, device name, serials, tokens, passwords, keys, dev keys), the web service, the MCP
/// server's own settings, audio/IO device ids, and the post-process shader chain/preset (file
/// names). The categories Debugging, DebuggingGraphics, RendererDebug, Paths, DataStorage,
/// Network, WebService, Miscellaneous, Controls, Ui*, Services, Multiplayer, AddOns, Android and
/// GpuDriver are not on the allowlist at all.
bool Allowed(const Settings::BasicSetting& s) {
    const std::string name = Lower(s.GetLabel());
    static constexpr std::array<std::string_view, 17> kDeniedFragments{
        "dump", "token",    "password", "passwd",  "secret", "serial", "key",   "mcp",        "gdb",
        "udp",  "airplane", "username", "network", "log",    "web",    "debug", "post_shader"};
    static constexpr std::array<std::string_view, 8> kDeniedSuffixes{
        "_path", "_dir", "_file", "_url", "_host", "_server", "_port", "_device"};
    for (const auto fragment : kDeniedFragments) {
        if (name.find(fragment) != std::string::npos) {
            return false;
        }
    }
    for (const auto suffix : kDeniedSuffixes) {
        if (name.ends_with(suffix) && name != "vulkan_device") {
            return false;
        }
    }
    switch (s.GetCategory()) {
    case Settings::Category::Renderer:
    case Settings::Category::RendererAdvanced:
    case Settings::Category::RendererHacks:
    case Settings::Category::RendererExtensions:
    case Settings::Category::Cpu:
    case Settings::Category::CpuDebug:
    case Settings::Category::CpuUnsafe:
    case Settings::Category::Audio:
    case Settings::Category::SystemAudio:
    case Settings::Category::Core:
        return true;
    case Settings::Category::System: {
        static constexpr std::array<std::string_view, 11> kSystem{
            "custom_rtc",       "custom_rtc_enabled", "custom_rtc_offset", "fast_cpu_time",
            "fast_gpu_time",    "language_index",     "region_index",      "rng_seed",
            "rng_seed_enabled", "time_zone_index",    "use_docked_mode"};
        return std::find(kSystem.begin(), kSystem.end(), name) != kSystem.end();
    }
    default:
        return false;
    }
}

/// Looks up a setting by its exact config key. Returns nullptr if unknown or Hidden.
Settings::BasicSetting* Find(const std::string& name) {
    auto& by_key = Settings::values.linkage.by_key;
    const auto it = by_key.find(name);
    if (it == by_key.end() || !Allowed(*it->second)) {
        return nullptr;
    }
    return it->second;
}

/// Sets a setting from text; enum settings also accept their names (LoadString takes indices).
std::string SetFromString(Settings::BasicSetting& s, const std::string& value) {
    const std::string before = s.ToString();
    if (s.IsEnum() && !value.empty() && !std::isdigit(static_cast<unsigned char>(value[0]))) {
        for (int i = 0; i < 64; ++i) {
            s.LoadString(std::to_string(i));
            if (Lower(s.Canonicalize()) == Lower(value)) {
                return {};
            }
        }
        s.LoadString(before);
        return fmt::format("'{}' is not a value of {}", value, s.GetLabel());
    }
    s.LoadString(value);
    return {};
}

/**
 * Sets each name -> value pair in changes on the calling thread, logging each change on the
 * timeline. Failures are reported per entry and do not stop the others.
 * @param apply When true and a game is running, asks the host to apply the new settings.
 * @return Per-setting before/after or error, with reboot_needed if a changed setting is not
 *         runtime-modifiable. is_error is set only when a single requested change failed.
 */
ToolResult ApplyChanges(ToolContext& ctx, const json& changes, bool apply) {
    if (!changes.is_object() || changes.size() > 64) {
        return ToolResult::Error("values must be an object of at most 64 settings");
    }
    json results = json::array();
    bool any_error = false;
    bool needs_reboot = false;
    for (const auto& [name, value] : changes.items()) {
        auto* s = Find(name);
        if (!s) {
            results.push_back({{"name", name}, {"error", "unknown or not an exposed setting"}});
            any_error = true;
            continue;
        }
        if (!(value.is_string() || value.is_number() || value.is_boolean()) ||
            (value.is_string() && value.get_ref<const std::string&>().size() > 128)) {
            results.push_back({{"name", name},
                               {"error", "value must be a number, boolean or "
                                         "short string"}});
            any_error = true;
            continue;
        }
        const std::string before = s->IsEnum() ? s->Canonicalize() : s->ToString();
        const std::string text = value.is_string()    ? value.get<std::string>()
                                 : value.is_boolean() ? (value.get<bool>() ? "true" : "false")
                                                      : value.dump();
        if (std::string error = SetFromString(*s, text); !error.empty()) {
            results.push_back({{"name", name}, {"error", error}});
            any_error = true;
            continue;
        }
        json entry{{"name", name},
                   {"before", before},
                   {"after", s->IsEnum() ? s->Canonicalize() : s->ToString()}};
        if (ctx.GameRunning() && !s->RuntimeModifiable()) {
            entry["note"] = "not runtime-modifiable: takes effect on the next boot";
            needs_reboot = true;
        }
        results.push_back(std::move(entry));
        if (ctx.timeline) {
            ctx.timeline->Push(EventKind::State, 0, "mcp",
                               fmt::format("setting {}: {} -> {}", name, before,
                                           results.back()["after"].get<std::string>()));
        }
    }
    if (apply && ctx.host && ctx.GameRunning()) {
        ctx.host->ApplySettings();
    }
    json out{{"results", std::move(results)},
             {"note", "Changed in memory only: this tool never writes the config file (the "
                      "emulator itself may save its settings when it exits)."}};
    if (needs_reboot) {
        out["reboot_needed"] = true;
    }
    ToolResult r = ToolResult::Structured(std::move(out));
    r.is_error = any_error && changes.size() == 1;
    return r;
}

/// set_determinism: maps rng_seed, rtc, language and region onto their settings (enabling the
/// seed/RTC overrides) and applies them via ApplyChanges. clear wins over rng_seed and rtc.
ToolResult SetDeterminism(ToolContext& ctx, const json& args) {
    json changes = json::object();
    if (args.contains("rng_seed")) {
        changes["rng_seed_enabled"] = true;
        changes["rng_seed"] = args["rng_seed"].get<u32>();
    }
    if (args.contains("rtc")) {
        changes["custom_rtc_enabled"] = true;
        changes["custom_rtc"] = args["rtc"].get<s64>();
    }
    if (args.value("clear", false)) {
        changes["rng_seed_enabled"] = false;
        changes["custom_rtc_enabled"] = false;
    }
    if (args.contains("language")) {
        changes["language_index"] = args["language"];
    }
    if (args.contains("region")) {
        changes["region_index"] = args["region"];
    }
    if (changes.empty()) {
        return ToolResult::Error("pass rng_seed, rtc, language, region or clear");
    }
    return ApplyChanges(ctx, changes, true);
}

/// Home directory prefixes in a value are shown as "~": paths in EDEN_ variables otherwise
/// carry the OS user name.
std::string ScrubHome(std::string value) {
    for (const char* home_var : {"USERPROFILE", "HOME"}) {
        const char* home = std::getenv(home_var);
        if (home == nullptr || *home == '\0' || std::string_view{home}.size() < 2) {
            continue;
        }
        for (size_t at = value.find(home); at != std::string::npos; at = value.find(home, at)) {
            value.replace(at, std::string_view{home}.size(), "~");
        }
    }
    return value;
}

/// env_switches: environment variables of this process whose names start with prefix
/// (case-sensitive, default "EDEN_").
ToolResult EnvSwitches(ToolContext&, const json& args) {
    // Only EDEN_ variables, never PATH, USERPROFILE or whatever secrets the environment holds.
    const std::string prefix = args.value("prefix", "EDEN_");
    if (!prefix.starts_with("EDEN_") || prefix.size() > 64) {
        return ToolResult::Error("prefix must start with EDEN_");
    }
    json vars = json::object();
#ifdef _WIN32
    char** env = _environ;
#else
    char** env = environ;
#endif
    for (; env != nullptr && *env != nullptr; ++env) {
        const std::string_view entry{*env};
        if (!entry.starts_with(prefix)) {
            continue;
        }
        const auto eq = entry.find('=');
        const std::string name{entry.substr(0, eq)};
        std::string upper = name;
        std::transform(upper.begin(), upper.end(), upper.begin(),
                       [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (upper.find("TOKEN") != std::string::npos || upper.find("KEY") != std::string::npos ||
            upper.find("SECRET") != std::string::npos || upper.find("PASS") != std::string::npos ||
            upper.find("PWD") != std::string::npos) {
            continue;
        }
        std::string value = eq == std::string_view::npos ? "" : std::string{entry.substr(eq + 1)};
        if (value.size() > 256) {
            value.resize(256);
        }
        vars[name] = ScrubHome(std::move(value));
    }
    return ToolResult::Structured(json{
        {"variables", std::move(vars)},
        {"note", "Only EDEN_* variables, secrets-looking names skipped, values cut at 256 "
                 "characters. Most EDEN_NX2_* switches are read once at first use: changing them "
                 "needs a relaunch with the variable set (the perf HUD is live: perf_report)."}});
}

} // namespace

void RegisterSettingsTools(ToolRegistry& registry) {
    registry.Add(Tool{
        .name = "set_determinism",
        .title = "Determinism settings",
        .description = "Make runs repeatable: fixed RNG seed, fixed RTC (unix seconds), language "
                       "and region (names or indices); clear:true turns the seed and RTC back off. "
                       "Most take effect on the next boot.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"rng_seed",
                            {{"type", "integer"}, {"minimum", 0}, {"maximum", 4294967295LL}}},
                           {"rtc", {{"type", "integer"}}},
                           {"language",
                            {{"type", "string"},
                             {"maxLength", 32},
                             {"description", "e.g. EnglishAmerican, Japanese, or an index"}}},
                           {"region",
                            {{"type", "string"},
                             {"maxLength", 32},
                             {"description", "e.g. Usa, Europe, Japan, or an index"}}},
                           {"clear", {{"type", "boolean"}}}}},
                         {"additionalProperties", false}},
        .requires_write = true,
        .requires_control = true,
        .handler = SetDeterminism,
    });
    registry.Add(Tool{
        .name = "env_switches",
        .title = "Environment switches",
        .description = "EDEN_* environment variables in effect for this emulator process, i.e. "
                       "which EDEN_NX2_* diagnostics and workarounds are on. Other variables "
                       "are never shown; names containing TOKEN, KEY, SECRET, PASS or PWD are "
                       "skipped; values are cut at 256 characters.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"prefix",
                            {{"type", "string"},
                             {"maxLength", 64},
                             {"description", "Must start with EDEN_ (default EDEN_)."}}}}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true, .idempotent = true},
        .handler = EnvSwitches,
    });
}

} // namespace Mcp

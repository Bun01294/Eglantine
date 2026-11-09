// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// The running application's memory map (a JSON version of the GDB stub's monitor command).

#include <mutex>
#include <optional>
#include <limits>

#include <fmt/format.h>

#include "core/arm/debug.h"
#include "core/core.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_light_lock.h"
#include "core/hle/kernel/k_thread.h"
#include "mcp/control.h"
#include "mcp/context.h"
#include "mcp/tools/app_process.h"
#include "mcp/host.h"
#include "mcp/tool_registry.h"
#include "mcp/tools/tool_helpers.h"

namespace Mcp {

namespace {

std::string Hex(u64 value) {
    return fmt::format("{:#x}", value);
}

/// Name of a memory state as used in memory_map output and its "state" filter.
const char* MemoryStateName(Kernel::Svc::MemoryState state) {
    using S = Kernel::Svc::MemoryState;
    switch (state) {
    case S::Free: return "Free";
    case S::Io: return "Io";
    case S::Static: return "Static";
    case S::Code: return "Code";
    case S::CodeData: return "CodeData";
    case S::Normal: return "Normal";
    case S::Shared: return "Shared";
    case S::AliasCode: return "AliasCode";
    case S::AliasCodeData: return "AliasCodeData";
    case S::Ipc: return "Ipc";
    case S::Stack: return "Stack";
    case S::ThreadLocal: return "ThreadLocal";
    case S::Transferred: return "Transferred";
    case S::SharedTransferred: return "SharedTransferred";
    case S::SharedCode: return "SharedCode";
    case S::Inaccessible: return "Inaccessible";
    case S::NonSecureIpc: return "NonSecureIpc";
    case S::NonDeviceIpc: return "NonDeviceIpc";
    case S::Kernel: return "Kernel";
    case S::GeneratedCode: return "GeneratedCode";
    case S::CodeOut: return "CodeOut";
    case S::Coverage: return "Coverage";
    default: return "Unknown";
    }
}

/// "rwx"-style permission string. Free regions and any permission other than r-x, r-- or rw-
/// show as "---".
const char* PermissionString(const Kernel::Svc::MemoryInfo& info) {
    if (info.state == Kernel::Svc::MemoryState::Free) {
        return "---";
    }
    switch (info.permission) {
    case Kernel::Svc::MemoryPermission::ReadExecute: return "r-x";
    case Kernel::Svc::MemoryPermission::Read: return "r--";
    case Kernel::Svc::MemoryPermission::ReadWrite: return "rw-";
    default: return "---";
    }
}

/// The application process, or null when there is no system or no application running.
/// The application process (kept alive for the tool's duration), or nullopt while it is still
/// booting or shutting down.
std::optional<AppProcessRef> Process(ToolContext& ctx) {
    std::optional<AppProcessRef> process;
    if (ctx.system != nullptr) {
        process.emplace(*ctx.system);
        if (!*process) {
            process.reset();
        }
    }
    return process;
}

/// memory_map: walks the page table from address 0 with QueryInfo. Free and Inaccessible regions
/// are skipped unless include_free, the trailing Inaccessible region always is, and the walk stops
/// once more than 20000 regions are collected.
ToolResult MemoryMap(ToolContext& ctx, const json& args) {
    const auto app = Process(ctx);
    if (!app) {
        return ToolResult::Error(kNoApplicationProcess);
    }
    auto* process = app->Get();
    const bool include_free = args.value("include_free", false);
    const std::string state_filter = args.value("state", "");
    auto& pt = process->GetPageTable();
    json regions = json::array();
    u64 addr = 0;
    while (true) {
        Kernel::KMemoryInfo mem_info{};
        Kernel::Svc::PageInfo page_info{};
        if (pt.QueryInfo(std::addressof(mem_info), std::addressof(page_info), addr) !=
            ResultSuccess) {
            break;
        }
        const auto info = mem_info.GetSvcMemoryInfo();
        const bool last = info.base_address + info.size - 1 == (std::numeric_limits<u64>::max)();
        const bool is_free = info.state == Kernel::Svc::MemoryState::Free ||
                             info.state == Kernel::Svc::MemoryState::Inaccessible;
        const char* state = MemoryStateName(info.state);
        if ((include_free || !is_free) && (state_filter.empty() || state_filter == state) &&
            !(last && info.state == Kernel::Svc::MemoryState::Inaccessible)) {
            regions.push_back({{"base", Hex(info.base_address)},
                               {"end", Hex(info.base_address + info.size)},
                               {"size", info.size},
                               {"perm", PermissionString(info)},
                               {"state", state}});
        }
        const u64 next = info.base_address + info.size;
        if (next <= addr || regions.size() > 20000) {
            break;
        }
        addr = next;
    }
    return ToolResult::Structured(json{{"regions", std::move(regions)}});
}

} // namespace

/// Registers memory_map.
void RegisterProcessTools(ToolRegistry& registry) {
    registry.Add(Tool{
        .name = "memory_map",
        .title = "Memory map",
        .description = "The process's mapped regions (base, end, size, permissions, state such as "
                       "Code/CodeData/Normal/Stack/Shared). Optionally filter by state.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"include_free", {{"type", "boolean"}}},
                           {"state", {{"type", "string"}, {"maxLength", 32},
                                      {"description", "e.g. Normal, Code, Stack"}}}}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true, .idempotent = true},
        .requires_game = true,
        .handler = MemoryMap,
    });
}

} // namespace Mcp

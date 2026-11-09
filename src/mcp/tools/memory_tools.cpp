// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Guest memory: read/write, pattern search and frame-sampled watches (changes become timeline
// events), so an agent can find addresses, not just read them.

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>

#include <fmt/format.h>

#include "core/arm/debug.h"
#include "core/core.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/debugger/mapped_range_hook.h"
#include "core/memory.h"
#include "mcp/context.h"
#include "mcp/timeline.h"
#include "mcp/tool_registry.h"
#include "mcp/tools/app_process.h"
#include "mcp/tools/tool_helpers.h"

namespace Mcp {

namespace {

/// Value types the typed read, write, search, watch and scan tools accept.
enum class ValueType { U8, U16, U32, U64, S8, S16, S32, S64, F32, F64 };

/// Type names as used in tool arguments, in ValueType order (indexed by the enum value).
constexpr std::array<std::pair<std::string_view, ValueType>, 10> kTypes{{
    {"u8", ValueType::U8},   {"u16", ValueType::U16}, {"u32", ValueType::U32},
    {"u64", ValueType::U64}, {"s8", ValueType::S8},   {"s16", ValueType::S16},
    {"s32", ValueType::S32}, {"s64", ValueType::S64}, {"f32", ValueType::F32},
    {"f64", ValueType::F64},
}};

/// The type names as a JSON array, for schema "enum" lists.
json TypeEnum() {
    json names = json::array();
    for (const auto& [name, type] : kTypes) {
        names.push_back(name);
    }
    return names;
}

/// ValueType for a type name; unknown names give U32.
ValueType ParseType(const std::string& name) {
    for (const auto& [n, type] : kTypes) {
        if (n == name) {
            return type;
        }
    }
    return ValueType::U32;
}

/// Size of a value of `type` in bytes (1, 2, 4 or 8).
size_t TypeSize(ValueType type) {
    switch (type) {
    case ValueType::U8:
    case ValueType::S8:
        return 1;
    case ValueType::U16:
    case ValueType::S16:
        return 2;
    case ValueType::U32:
    case ValueType::S32:
    case ValueType::F32:
        return 4;
    default:
        return 8;
    }
}

/// Raw little-endian bits of a value of `type` read from bytes.
u64 LoadBits(const u8* p, ValueType type) {
    u64 bits = 0;
    std::memcpy(&bits, p, TypeSize(type));
    return bits;
}

/// Numeric value for comparisons and display.
double AsDouble(u64 bits, ValueType type) {
    switch (type) {
    case ValueType::U8: return static_cast<double>(static_cast<u8>(bits));
    case ValueType::U16: return static_cast<double>(static_cast<u16>(bits));
    case ValueType::U32: return static_cast<double>(static_cast<u32>(bits));
    case ValueType::U64: return static_cast<double>(bits);
    case ValueType::S8: return static_cast<double>(static_cast<s8>(bits));
    case ValueType::S16: return static_cast<double>(static_cast<s16>(bits));
    case ValueType::S32: return static_cast<double>(static_cast<s32>(bits));
    case ValueType::S64: return static_cast<double>(static_cast<s64>(bits));
    case ValueType::F32: return static_cast<double>(std::bit_cast<f32>(static_cast<u32>(bits)));
    case ValueType::F64: return std::bit_cast<f64>(bits);
    }
    return 0.0;
}

/// JSON number for a value: u64 stays unsigned, floats become doubles, other integers are signed
/// (sign-extended for signed types).
json ValueJson(u64 bits, ValueType type) {
    switch (type) {
    case ValueType::U64: return bits;
    case ValueType::S64: return static_cast<s64>(bits);
    case ValueType::F32:
    case ValueType::F64: return AsDouble(bits, type);
    default: return static_cast<s64>(AsDouble(bits, type));
    }
}

/// Bits of `value` (a JSON number) stored as `type`.
u64 ToBits(const json& value, ValueType type) {
    switch (type) {
    case ValueType::F32: return std::bit_cast<u32>(value.get<f32>());
    case ValueType::F64: return std::bit_cast<u64>(value.get<f64>());
    case ValueType::U64: return value.get<u64>();
    default: {
        const s64 v = value.get<s64>();
        const size_t bytes = TypeSize(type);
        return bytes == 8 ? static_cast<u64>(v) : static_cast<u64>(v) & ((u64{1} << (bytes * 8)) - 1);
    }
    }
}

std::string Hex(u64 v) {
    return fmt::format("{:#x}", v);
}

/// Address from a JSON integer or a string in any base std::stoull accepts ("0x..." for hex).
/// Returns nullopt for other types or unparsable strings.
std::optional<u64> ParseAddress(const json& v) {
    if (v.is_number_unsigned() || v.is_number_integer()) {
        return v.get<u64>();
    }
    if (v.is_string()) {
        try {
            return std::stoull(v.get<std::string>(), nullptr, 0);
        } catch (...) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

json AddressSchema(const char* what) {
    return json{{"description", fmt::format("{} as a hex string (\"0x8004a000\") or integer.", what)}};
}

/// The value of one hex digit, or nullopt.
std::optional<u8> HexNibble(char c) {
    if (c >= '0' && c <= '9') {
        return static_cast<u8>(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return static_cast<u8>(c - 'a' + 10);
    }
    if (c >= 'A' && c <= 'F') {
        return static_cast<u8>(c - 'A' + 10);
    }
    return std::nullopt;
}

/// Strict hex bytes: optional "0x", whitespace ignored, then an even, non-empty run of hex digits.
std::optional<std::vector<u8>> ParseHexBytes(std::string text) {
    std::erase_if(text, [](char c) { return c == ' ' || c == '\n' || c == '\t' || c == '\r'; });
    if (text.starts_with("0x")) {
        text = text.substr(2);
    }
    if (text.empty() || text.size() % 2 != 0) {
        return std::nullopt;
    }
    std::vector<u8> out;
    out.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        const auto hi = HexNibble(text[i]);
        const auto lo = HexNibble(text[i + 1]);
        if (!hi || !lo) {
            return std::nullopt;
        }
        out.push_back(static_cast<u8>(*hi << 4 | *lo));
    }
    return out;
}

/// Lowercase hex string of the bytes, two digits each, no separators.
std::string ToHex(std::span<const u8> bytes) {
    std::string out;
    out.reserve(bytes.size() * 2);
    for (const u8 b : bytes) {
        out += fmt::format("{:02x}", b);
    }
    return out;
}

/// A contiguous mapped range of guest virtual memory.
struct Region {
    u64 base;
    u64 size;
};

/// Readable regions to search: writable data by default ("data"), "heap", "code" (r-x), or
/// "all" readable.
std::vector<Region> SearchRegions(Kernel::KProcess& process, const std::string& which) {
    using S = Kernel::Svc::MemoryState;
    using P = Kernel::Svc::MemoryPermission;
    auto& pt = process.GetPageTable();
    const u64 heap_start = GetInteger(pt.GetHeapRegionStart());
    const u64 heap_end = heap_start + pt.GetHeapRegionSize();
    std::vector<Region> out;
    u64 addr = 0;
    while (true) {
        Kernel::KMemoryInfo mem_info{};
        Kernel::Svc::PageInfo page_info{};
        if (pt.QueryInfo(std::addressof(mem_info), std::addressof(page_info), addr) !=
            ResultSuccess) {
            break;
        }
        const auto info = mem_info.GetSvcMemoryInfo();
        const bool mapped = info.state != S::Free && info.state != S::Inaccessible &&
                            info.state != S::Io && info.state != S::Kernel;
        const bool readable = mapped && (info.permission == P::Read ||
                                         info.permission == P::ReadWrite ||
                                         info.permission == P::ReadExecute);
        bool take = false;
        if (which == "all") {
            take = readable;
        } else if (which == "code") {
            take = readable && info.permission == P::ReadExecute;
        } else if (which == "heap") {
            take = readable && info.permission == P::ReadWrite && info.base_address >= heap_start &&
                   info.base_address < heap_end;
        } else {
            take = readable && info.permission == P::ReadWrite;
        }
        if (take) {
            out.push_back({info.base_address, info.size});
        }
        const u64 next = info.base_address + info.size;
        if (next <= addr) {
            break;
        }
        addr = next;
    }
    return out;
}

/// The application process, or null when there is no system or no application running.
/// True when address + length wraps around the 64-bit address space (the range checks below
/// would then validate a different, wrapped range than the one being accessed).
bool RangeWraps(u64 address, u64 length) {
    return length > std::numeric_limits<u64>::max() - address;
}

// ---- read / write -------------------------------------------------------------------------

/// read_memory: reads `length` bytes (default 64) as hex or ascii, or `count` values of a type.
/// Fails if the address is bad, the size is over 64 KiB, or the range is not fully mapped.
ToolResult ReadMemory(ToolContext& ctx, const json& args) {
    const auto address = ParseAddress(args.at("address"));
    if (!address) {
        return ToolResult::Error("bad address");
    }
    const std::string format = args.value("format", "hex");
    const size_t count = args.value("count", size_t{1});
    size_t length = args.value("length", size_t{0});
    std::optional<ValueType> type;
    if (format != "hex" && format != "ascii") {
        if (count == 0 || count > 8192) {
            return ToolResult::Error("count must be 1..8192");
        }
        type = ParseType(format);
        length = TypeSize(*type) * count;
    } else if (length == 0) {
        length = 64;
    }
    if (length > 65536) {
        return ToolResult::Error("at most 64 KiB per read");
    }
    if (RangeWraps(*address, length)) {
        return ToolResult::Error("address + length wraps around the address space");
    }
    const AppProcessRef process{*ctx.system};
    if (!process) {
        return ToolResult::Error(kNoApplicationProcess);
    }
    auto& memory = process.Memory();
    if (!memory.IsValidVirtualAddressRange(*address, length)) {
        return ToolResult::Error(fmt::format("{} + {:#x} is not fully mapped (memory_map)",
                                             Hex(*address), length));
    }
    std::vector<u8> bytes(length);
    memory.ReadBlock(*address, bytes.data(), length);
    json out{{"address", Hex(*address)}, {"length", length}};
    if (format == "hex") {
        out["hex"] = ToHex(bytes);
    } else if (format == "ascii") {
        std::string text;
        for (const u8 b : bytes) {
            text += b >= 0x20 && b < 0x7f ? static_cast<char>(b) : '.';
        }
        out["ascii"] = std::move(text);
    } else {
        json values = json::array();
        for (size_t i = 0; i < count; ++i) {
            values.push_back(ValueJson(LoadBits(bytes.data() + i * TypeSize(*type), *type), *type));
        }
        out["type"] = format;
        out["values"] = std::move(values);
    }
    return ToolResult::Structured(std::move(out));
}

/// write_memory: writes hex bytes or one typed value, invalidates the instruction cache for the
/// range, and logs old and new bytes as a 'watch' event. Returns the bytes before and after.
/// Fails if the range is not fully mapped.
ToolResult WriteMemory(ToolContext& ctx, const json& args) {
    const auto address = ParseAddress(args.at("address"));
    if (!address) {
        return ToolResult::Error("bad address");
    }
    std::vector<u8> bytes;
    if (args.contains("hex")) {
        auto parsed = ParseHexBytes(args["hex"].get<std::string>());
        if (!parsed || parsed->empty()) {
            return ToolResult::Error("hex must be an even number of hex digits");
        }
        bytes = std::move(*parsed);
    } else if (args.contains("value") && args.contains("type")) {
        const ValueType type = ParseType(args["type"].get<std::string>());
        const u64 bits = ToBits(args["value"], type);
        bytes.resize(TypeSize(type));
        std::memcpy(bytes.data(), &bits, bytes.size());
    } else {
        return ToolResult::Error("pass hex, or value + type");
    }
    if (RangeWraps(*address, bytes.size())) {
        return ToolResult::Error("address + length wraps around the address space");
    }
    const AppProcessRef process{*ctx.system};
    if (!process) {
        return ToolResult::Error(kNoApplicationProcess);
    }
    auto& memory = process.Memory();
    if (!memory.IsValidVirtualAddressRange(*address, bytes.size())) {
        return ToolResult::Error("range not fully mapped");
    }
    std::vector<u8> before(bytes.size());
    memory.ReadBlock(*address, before.data(), before.size());
    memory.WriteBlock(*address, bytes.data(), bytes.size());
    Core::InvalidateInstructionCacheRange(process.Get(), *address, bytes.size());
    if (ctx.timeline) {
        ctx.timeline->Push(EventKind::Watch, 0, "mcp",
                           fmt::format("write {} {} -> {}", Hex(*address), ToHex(before),
                                       ToHex(bytes)));
    }
    return ToolResult::Structured(
        json{{"address", Hex(*address)}, {"before", ToHex(before)}, {"after", ToHex(bytes)}});
}

// ---- search -------------------------------------------------------------------------------

/// How much one search/scan call may do: a client-sized region list ("all") must not pin a worker
/// thread (and the memory it reads) for minutes.
struct ScanBudget {
    static constexpr u64 kMaxBytes = u64{2} << 30; // 2 GiB
    static constexpr auto kMaxTime = std::chrono::seconds{8};
    std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + kMaxTime;
    u64 used = 0;
    bool exhausted = false; ///< A limit (bytes, time, server stopping) cut the scan short.

    bool Spent() {
        if (used >= kMaxBytes || ServerStopping() || std::chrono::steady_clock::now() >= deadline) {
            exhausted = true;
        }
        return exhausted;
    }
};

/// Calls fn(address, data, size) over every region in chunks (with `overlap` bytes carried over
/// so matches across chunk boundaries are found). Reads bypass GPU read traps (no side effects).
/// Stops when `budget` is spent (checked between chunks).
template <typename Fn>
void ForEachChunk(Core::Memory::Memory& memory, const std::vector<Region>& regions,
                  size_t overlap, ScanBudget& budget, Fn&& fn) {
    constexpr size_t kChunk = 1024 * 1024;
    std::vector<u8> buffer(kChunk + overlap);
    for (const auto& region : regions) {
        for (u64 offset = 0; offset < region.size; offset += kChunk) {
            if (budget.Spent()) {
                return;
            }
            const u64 addr = region.base + offset;
            const size_t size =
                static_cast<size_t>(std::min<u64>(kChunk + overlap, region.size - offset));
            if (!memory.ReadBlockUnsafe(addr, buffer.data(), size)) {
                continue;
            }
            budget.used += size;
            if (!fn(addr, buffer.data(), size)) {
                return;
            }
        }
    }
}

/// search_memory: scans the chosen regions for a hex pattern with "??" wildcards, an ascii string
/// or a typed value, at addresses that are multiples of `align`. Stops at max_results matches.
/// Blocks the HTTP worker thread for the whole scan, which can take a while for large regions.
ToolResult SearchMemory(ToolContext& ctx, const json& args) {
    const AppProcessRef process{*ctx.system};
    if (!process) {
        return ToolResult::Error(kNoApplicationProcess);
    }
    constexpr size_t kMaxPattern = 256;
    // Pattern bytes with a wildcard mask.
    std::vector<u8> pattern;
    std::vector<bool> mask;
    if (args.contains("hex")) {
        std::string text = args["hex"].get<std::string>();
        std::erase_if(text, [](char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; });
        if (text.empty() || text.size() % 2 != 0 || text.size() > kMaxPattern * 2) {
            return ToolResult::Error(
                "hex pattern: 1..256 bytes, each two hex digits or ?? (e.g. \"DE AD ?? EF\")");
        }
        bool has_fixed_byte = false;
        for (size_t i = 0; i < text.size(); i += 2) {
            if (text[i] == '?' && text[i + 1] == '?') {
                pattern.push_back(0);
                mask.push_back(false);
                continue;
            }
            const auto hi = HexNibble(text[i]);
            const auto lo = HexNibble(text[i + 1]);
            if (!hi || !lo) {
                return ToolResult::Error("bad hex byte: " + text.substr(i, 2));
            }
            pattern.push_back(static_cast<u8>(*hi << 4 | *lo));
            mask.push_back(true);
            has_fixed_byte = true;
        }
        if (!has_fixed_byte) {
            return ToolResult::Error("hex pattern needs at least one non-?? byte");
        }
    } else if (args.contains("ascii")) {
        const std::string text = args["ascii"].get<std::string>();
        if (text.empty() || text.size() > kMaxPattern) {
            return ToolResult::Error("ascii pattern: 1..256 bytes");
        }
        for (const char c : text) {
            pattern.push_back(static_cast<u8>(c));
            mask.push_back(true);
        }
    } else if (args.contains("value") && args.contains("type")) {
        const ValueType type = ParseType(args["type"].get<std::string>());
        const u64 bits = ToBits(args["value"], type);
        pattern.resize(TypeSize(type));
        std::memcpy(pattern.data(), &bits, pattern.size());
        mask.assign(pattern.size(), true);
    } else {
        return ToolResult::Error("pass hex, ascii, or value + type");
    }
    if (pattern.empty()) {
        return ToolResult::Error("empty pattern");
    }
    const size_t max_results = std::clamp<size_t>(args.value("max_results", size_t{50}), 1, 10000);
    const size_t align = std::max<size_t>(args.value("align", size_t{1}), 1);
    const auto regions = SearchRegions(*process.Get(), args.value("region", "data"));
    json matches = json::array();
    u64 scanned = 0;
    ScanBudget budget;
    const size_t overlap = pattern.size() - 1;
    ForEachChunk(process.Memory(), regions, overlap, budget,
                 [&](u64 base, const u8* data, size_t size) {
                     scanned += size > overlap ? size - overlap : size;
                     for (size_t i = 0; i + pattern.size() <= size; ++i) {
                         if ((base + i) % align != 0) {
                             continue;
                         }
                         bool hit = true;
                         for (size_t j = 0; j < pattern.size() && hit; ++j) {
                             hit = !mask[j] || data[i + j] == pattern[j];
                         }
                         if (hit && (matches.empty() || matches.back() != Hex(base + i))) {
                             matches.push_back(Hex(base + i));
                             if (matches.size() >= max_results) {
                                 return false;
                             }
                         }
                     }
                     return true;
                 });
    // Read before `matches` is moved into the result.
    const bool capped = matches.size() >= max_results;
    json out{{"matches", std::move(matches)},
             {"regions", regions.size()},
             {"bytes_scanned", scanned},
             {"capped", capped}};
    if (budget.exhausted) {
        out["truncated"] = true;
        out["truncated_note"] = "search stopped at its time/size limit (8 s, 2 GiB); narrow "
                                "'region' to cover the rest";
    }
    return ToolResult::Structured(std::move(out));
}

// ---- watches ------------------------------------------------------------------------------

/// Samples watched addresses once per presented frame on its own thread and records changes in
/// the timeline. Runs only while watches exist and a game is running. Watches added with
/// at_invalidate are sampled instead on the guest thread inside NVN InvalidateMappedRange calls
/// that cover them (Core::Trace::g_invalidate_hook, installed only while such watches exist):
/// GPU results the guest reads there and clears before the next present are invisible per frame.
class WatchSampler {
public:
    static constexpr size_t kMaxWatches = 256;

    /// One watched address, its last sampled bits (unset until the first sample) and change
    /// statistics.
    struct Watch {
        u32 id;
        u64 address;
        ValueType type;
        std::string label;
        bool at_invalidate = false;
        std::optional<u64> last;
        u64 changes = 0;
        u64 last_change_frame = 0;
        u64 samples = 0; ///< Successful reads (unmapped addresses are skipped, not counted)
    };

    static WatchSampler& Get() {
        static WatchSampler instance;
        return instance;
    }

    /// Adds a watch and returns its id. Starts the sampling thread if it is not running, joining
    /// a previous thread that stopped by itself. `system` and `timeline` must outlive the thread.
    /// Returns 0 and sets `error` when the server is shutting down or the watch limit is reached
    /// (checked under the lock, so concurrent callers can't overshoot).
    u32 Add(Core::System& system, Timeline* timeline, u64 address, ValueType type,
            std::string label, bool at_invalidate, std::string& error) {
        std::scoped_lock lock{mutex};
        // Checked under the lock Shutdown takes: a watch added after shutdown began would start
        // a sampler that nothing joins.
        if (ServerStopping()) {
            error = "the MCP server is stopping";
            return 0;
        }
        if (watches.size() >= kMaxWatches) {
            error = fmt::format("at most {} watches", kMaxWatches);
            return 0;
        }
        const u32 id = next_id++;
        watches.push_back({id, address, type, std::move(label), at_invalidate});
        hook_system = &system;
        hook_timeline = timeline;
        UpdateHook();
        if (!thread.joinable() || stopped) {
            if (thread.joinable()) {
                thread.join();
            }
            stopped = false;
            thread = std::jthread([this, &system, timeline](std::stop_token stop) {
                Run(stop, system, timeline);
            });
        }
        return id;
    }

    /// Removes a watch; returns whether one had that id. The thread exits on its next frame once
    /// no watches remain.
    bool Remove(u32 id) {
        std::scoped_lock lock{mutex};
        const bool removed = std::erase_if(watches, [id](const Watch& w) { return w.id == id; }) != 0;
        UpdateHook();
        return removed;
    }

    void Clear() {
        std::scoped_lock lock{mutex};
        watches.clear();
        UpdateHook();
    }

    /// Invalidate hook body: samples the at_invalidate watches inside [address, address + size).
    void SampleAtInvalidate(u64 address, u64 size) {
        std::scoped_lock lock{mutex};
        if (hook_system == nullptr) {
            return;
        }
        const auto covered = [&](const Watch& w) {
            return w.at_invalidate && w.address >= address &&
                   w.address + TypeSize(w.type) <= address + size;
        };
        if (std::none_of(watches.begin(), watches.end(), covered)) {
            return;
        }
        const AppProcessRef process{*hook_system};
        if (!process) {
            return;
        }
        const u64 frame = PresentedFrames();
        for (auto& w : watches) {
            if (covered(w)) {
                Sample(w, process.Memory(), frame, hook_timeline);
            }
        }
    }

    /// Copy of the current watches, taken under the lock.
    std::vector<Watch> List() {
        std::scoped_lock lock{mutex};
        return watches;
    }

    /// Server shutdown: drops the watches and ends the sampling thread.
    void Shutdown() {
        std::jthread finished;
        {
            std::scoped_lock lock{mutex};
            watches.clear();
            UpdateHook();
            finished = std::move(thread);
            stopped = false;
        }
        if (finished.joinable()) {
            finished.request_stop();
            finished.join();
        }
    }

    ~WatchSampler() {
        if (thread.joinable()) {
            thread.request_stop();
            thread.join();
        }
    }

private:
    /// Sampling thread body: polls every 2 ms for a new presented frame, then reads each watch
    /// under the lock and posts changes. Unreadable addresses are skipped. Clears the watches and
    /// exits (setting `stopped`) when none are left or the system powers off.
    void Run(std::stop_token stop, Core::System& system, Timeline* timeline) {
        u64 last_frame = PresentedFrames();
        while (!stop.stop_requested()) {
            std::this_thread::sleep_for(std::chrono::milliseconds{2});
            const u64 frame = PresentedFrames();
            if (frame == last_frame) {
                continue;
            }
            last_frame = frame;
            std::scoped_lock lock{mutex};
            if (watches.empty() || !system.IsPoweredOn() || system.IsShuttingDown()) {
                watches.clear();
                UpdateHook();
                stopped = true;
                return;
            }
            // Booting: the process doesn't exist yet. Keep the watches, sample next frame.
            const AppProcessRef process{system};
            if (!process) {
                continue;
            }
            for (auto& w : watches) {
                if (!w.at_invalidate) {
                    Sample(w, process.Memory(), frame, timeline);
                }
            }
        }
    }

    /// Reads one watch and posts a timeline event when the value differs from the last sample
    /// (the first sample only records the value). Call with the lock held.
    static void Sample(Watch& w, Core::Memory::Memory& memory, u64 frame, Timeline* timeline) {
        const size_t size = TypeSize(w.type);
        u8 buf[8]{};
        if (!memory.IsValidVirtualAddressRange(w.address, size) ||
            !memory.ReadBlockUnsafe(w.address, buf, size)) {
            return;
        }
        ++w.samples;
        const u64 bits = LoadBits(buf, w.type);
        if (w.last && *w.last != bits) {
            ++w.changes;
            w.last_change_frame = frame;
            if (timeline) {
                timeline->Push(EventKind::Watch, 0, w.label.empty() ? Hex(w.address) : w.label,
                               fmt::format("{} {}{}: {} -> {}", Hex(w.address),
                                           kTypes[static_cast<size_t>(w.type)].first,
                                           w.at_invalidate ? " @invalidate" : "",
                                           ValueJson(*w.last, w.type).dump(),
                                           ValueJson(bits, w.type).dump()));
            }
        }
        w.last = bits;
    }

    /// Installs the invalidate hook while an at_invalidate watch exists. Call with the lock held.
    void UpdateHook() {
        const bool wanted = std::any_of(watches.begin(), watches.end(),
                                        [](const Watch& w) { return w.at_invalidate; });
        Core::Trace::g_invalidate_hook.store(wanted ? &OnInvalidate : nullptr,
                                             std::memory_order_release);
    }

    static void OnInvalidate(u64 address, u64 size) {
        Get().SampleAtInvalidate(address, size);
    }

    std::mutex mutex;
    std::vector<Watch> watches;
    /// What the invalidate hook samples with (set by Add; the system outlives the server).
    Core::System* hook_system = nullptr;
    Timeline* hook_timeline = nullptr;
    u32 next_id = 1;
    bool stopped = true;
    std::jthread thread;
};

/// watch_add: adds a watch on a mapped address (at most 256) and returns its id.
ToolResult WatchAdd(ToolContext& ctx, const json& args) {
    const auto address = ParseAddress(args.at("address"));
    if (!address) {
        return ToolResult::Error("bad address");
    }
    const ValueType type = ParseType(args.value("type", "u32"));
    if (RangeWraps(*address, TypeSize(type))) {
        return ToolResult::Error("address + size wraps around the address space");
    }
    {
        const AppProcessRef process{*ctx.system};
        if (!process) {
            return ToolResult::Error(kNoApplicationProcess);
        }
        if (!process.Memory().IsValidVirtualAddressRange(*address, TypeSize(type))) {
            return ToolResult::Error("address not mapped");
        }
    }
    const bool at_invalidate = args.value("at", "present") == "invalidate";
    std::string error;
    const u32 id = WatchSampler::Get().Add(*ctx.system, ctx.timeline, *address, type,
                                           args.value("label", ""), at_invalidate, error);
    if (id == 0) {
        return ToolResult::Error(error);
    }
    return ToolResult::Structured(
        json{{"id", id},
             {"at", at_invalidate ? "invalidate" : "present"},
             {"note", at_invalidate
                          ? "Sampled inside each NVN InvalidateMappedRange call that covers the "
                            "address (VTX-HLE only), i.e. what the guest reads there; changes "
                            "appear as 'watch' events marked @invalidate. Never sampled if the "
                            "game does not invalidate that range. Watches clear when the game "
                            "stops."
                          : "Sampled once per presented frame; changes appear as 'watch' events "
                            "in the timeline (timeline kinds:[\"watch\"], or act/wait_for). "
                            "Changes that revert within one frame are missed (GPU results the "
                            "guest clears after reading: try at:\"invalidate\"). Watches clear "
                            "when the game stops."}});
}

/// watch_list: each watch with its last value (null before the first sample) and change count.
ToolResult WatchList(ToolContext&, const json&) {
    json list = json::array();
    for (const auto& w : WatchSampler::Get().List()) {
        json entry{{"id", w.id},
                   {"address", Hex(w.address)},
                   {"type", kTypes[static_cast<size_t>(w.type)].first},
                   {"label", w.label},
                   {"at", w.at_invalidate ? "invalidate" : "present"},
                   {"samples", w.samples},
                   {"changes", w.changes},
                   {"last_change_frame", w.last_change_frame}};
        entry["value"] = w.last ? ValueJson(*w.last, w.type) : json{};
        list.push_back(std::move(entry));
    }
    return ToolResult::Structured(json{{"watches", std::move(list)}});
}

/// watch_remove: removes all watches with all:true, otherwise the one with `id` (error if none).
ToolResult WatchRemove(ToolContext&, const json& args) {
    if (args.value("all", false)) {
        WatchSampler::Get().Clear();
        return ToolResult::Text("all watches removed");
    }
    if (!args.contains("id")) {
        return ToolResult::Error("pass id or all:true");
    }
    return WatchSampler::Get().Remove(args["id"].get<u32>())
               ? ToolResult::Text("removed")
               : ToolResult::Error("no watch with that id");
}

json RegionSchema() {
    return json{{"type", "string"},
                {"enum", {"data", "heap", "code", "all"}},
                {"description", "data = writable regions (default), heap, code (r-x), all readable."}};
}

} // namespace

void StopMemoryWork() {
    WatchSampler::Get().Shutdown();
}

/// Registers the memory read/write, search and watch tools.
void RegisterMemoryTools(ToolRegistry& registry) {
    registry.Add(Tool{
        .name = "read_memory",
        .title = "Read guest memory",
        .description = "Read the application's memory at a virtual address: as hex (length bytes, "
                       "default 64), ascii, or count values of a type (u8..u64, s8..s64, f32, "
                       "f64). Max 64 KiB. Fails if the range isn't fully mapped (see memory_map).",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"address", AddressSchema("Virtual address")},
                           {"length", {{"type", "integer"}, {"minimum", 1}, {"maximum", 65536}}},
                           {"format", {{"type", "string"},
                                       {"enum", [] {
                                            json e = TypeEnum();
                                            e.push_back("hex");
                                            e.push_back("ascii");
                                            return e;
                                        }()}}},
                           {"count", {{"type", "integer"}, {"minimum", 1}, {"maximum", 8192}}}}},
                         {"required", {"address"}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true, .idempotent = true},
        .requires_game = true,
        .handler = ReadMemory,
    });
    registry.Add(Tool{
        .name = "write_memory",
        .title = "Write guest memory",
        .description = "Write bytes (hex) or one typed value to the application's memory. Code "
                       "writes invalidate the JIT cache for the range. Logged to the timeline with "
                       "the previous bytes.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"address", AddressSchema("Virtual address")},
                           {"hex", {{"type", "string"}, {"maxLength", 8192}}},
                           {"type", {{"type", "string"}, {"enum", TypeEnum()}}},
                           {"value", {{"type", "number"}}}}},
                         {"required", {"address"}},
                         {"additionalProperties", false}},
        .annotations = {.destructive = true},
        .requires_game = true,
        .requires_write = true,
        .requires_control = true,
        .handler = WriteMemory,
    });
    registry.Add(Tool{
        .name = "search_memory",
        .title = "Search guest memory",
        .description = "Find a byte pattern (hex with ?? wildcards, e.g. \"DE AD ?? EF\"), an "
                       "ascii string, or a typed value in the application's memory. Returns up to "
                       "max_results addresses (default 50). Stops at 8 s / 2 GiB per call "
                       "('truncated').",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"hex", {{"type", "string"}, {"maxLength", 768}}},
                           {"ascii", {{"type", "string"}, {"maxLength", 256}}},
                           {"type", {{"type", "string"}, {"enum", TypeEnum()}}},
                           {"value", {{"type", "number"}}},
                           {"region", RegionSchema()},
                           {"align", {{"type", "integer"}, {"minimum", 1}, {"maximum", 4096}}},
                           {"max_results", {{"type", "integer"}, {"minimum", 1}, {"maximum", 10000}}}}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true},
        .requires_game = true,
        .handler = SearchMemory,
    });
    registry.Add(Tool{
        .name = "watch_add",
        .title = "Watch an address",
        .description = "Watch a value; it is sampled every presented frame (or, with at "
                       "\"invalidate\", inside each NVN InvalidateMappedRange covering it) and "
                       "each change becomes a 'watch' timeline event (old -> new, with the "
                       "frame). Use with act or wait_for {kinds:[\"watch\"]} to see what an input "
                       "changed.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"address", AddressSchema("Virtual address")},
                           {"type", {{"type", "string"}, {"enum", TypeEnum()}}},
                           {"label", {{"type", "string"}, {"maxLength", 100}}},
                           {"at",
                            {{"type", "string"},
                             {"enum", {"present", "invalidate"}},
                             {"description",
                              "When to sample: once per presented frame (default), or when the "
                              "guest invalidates a mapped range covering the address (GPU results "
                              "it reads and clears within a frame; VTX-HLE only)."}}}}},
                         {"required", {"address"}},
                         {"additionalProperties", false}},
        .requires_game = true,
        .requires_control = true,
        .handler = WatchAdd,
    });
    registry.Add(Tool{
        .name = "watch_list",
        .title = "List watches",
        .description = "Current watches with their last value and how often they changed.",
        .annotations = {.read_only = true, .idempotent = true},
        .handler = WatchList,
    });
    registry.Add(Tool{
        .name = "watch_remove",
        .title = "Remove watches",
        .description = "Remove one watch by id, or all with all:true.",
        .input_schema = {{"type", "object"},
                         {"properties",
                          {{"id", {{"type", "integer"}, {"minimum", 1}}}, {"all", {{"type", "boolean"}}}}},
                         {"additionalProperties", false}},
        .requires_control = true,
        .handler = WatchRemove,
    });
}

} // namespace Mcp

// SPDX-FileCopyrightText: Copyright 2026 Vortex727 (FUKKO)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <map>
#include <mutex>
#include <tuple>

#include <fmt/format.h>

#include "video_core/nvn_draw_probe.h"

namespace VideoCommon::NvnDrawProbe {

namespace Detail {
std::atomic<bool> g_active{false};
}

namespace {

using Key = std::tuple<std::array<u64, StageCount>, u32, bool, bool, bool>;

struct State {
    std::mutex mutex;
    // Census: `current` is filled by the replay, `last` is the one of the last present.
    u32 census_presents_left{};
    bool census_partial{}; ///< `current` started mid-frame: dropped at the next present
    std::map<Key, Row> current;
    u64 current_draws{};
    u64 current_skipped{};
    std::map<u64, DispatchRow> current_dispatch_rows;
    u64 current_dispatches{};
    u64 current_skipped_dispatches{};
    Census last;
    u64 presents{};
    // Skip set.
    std::vector<u64> skip;
    SkipMode skip_mode{SkipMode::Skip};
    // Dump.
    DumpState dump;
    std::string dump_directory;
};

State& S() {
    static State state;
    return state;
}

void UpdateActiveLocked(State& s) {
    const bool active = s.census_presents_left != 0 || !s.skip.empty() ||
                        s.skip_mode == SkipMode::Only || (s.dump.armed && !s.dump.done);
    Detail::g_active.store(active, std::memory_order_relaxed);
}

bool Matches(const std::vector<u64>& set, const DrawInfo& info) {
    for (const u64 hash : info.hashes) {
        if (hash != 0 && std::find(set.begin(), set.end(), hash) != set.end()) {
            return true;
        }
    }
    return false;
}

void ClearCurrentLocked(State& s) {
    s.current.clear();
    s.current_draws = 0;
    s.current_skipped = 0;
    s.current_dispatch_rows.clear();
    s.current_dispatches = 0;
    s.current_skipped_dispatches = 0;
}

} // namespace

bool OnDraw(const DrawInfo& info) {
    State& s = S();
    std::scoped_lock lock{s.mutex};
    bool skip = false;
    if (s.skip_mode == SkipMode::Only) {
        skip = !Matches(s.skip, info);
    } else if (!s.skip.empty()) {
        skip = Matches(s.skip, info);
    }
    if (s.census_presents_left != 0) {
        const Key key{info.hashes, info.mode, info.indexed, info.indirect, info.mesh};
        auto [it, inserted] = s.current.try_emplace(key);
        Row& row = it->second;
        if (inserted) {
            row.first = info;
        }
        ++row.draws;
        row.elements += info.count;
        row.skipped += skip ? 1 : 0;
        ++s.current_draws;
        s.current_skipped += skip ? 1 : 0;
    }
    return skip;
}

bool OnDispatch(const DispatchInfo& info) {
    State& s = S();
    std::scoped_lock lock{s.mutex};
    // Only mode selects draws; a dispatch is skipped in either mode when its hash is listed.
    const bool skip =
        info.hash != 0 && std::find(s.skip.begin(), s.skip.end(), info.hash) != s.skip.end();
    if (s.census_presents_left != 0) {
        DispatchRow& row = s.current_dispatch_rows[info.hash];
        row.hash = info.hash;
        ++row.count;
        if (info.indirect) {
            ++row.indirect;
        } else {
            row.groups += u64{info.groups[0]} * info.groups[1] * info.groups[2];
        }
        row.skipped += skip ? 1 : 0;
        ++s.current_dispatches;
        s.current_skipped_dispatches += skip ? 1 : 0;
    }
    return skip;
}

void OnPresent() {
    State& s = S();
    std::scoped_lock lock{s.mutex};
    ++s.presents;
    if (s.census_presents_left == 0) {
        return;
    }
    if (s.census_partial) {
        s.census_partial = false;
        ClearCurrentLocked(s);
        return;
    }
    Census census;
    census.present = s.presents;
    census.draws = s.current_draws;
    census.skipped = s.current_skipped;
    census.rows.reserve(s.current.size());
    for (auto& [key, row] : s.current) {
        census.rows.push_back(std::move(row));
    }
    std::sort(census.rows.begin(), census.rows.end(),
              [](const Row& a, const Row& b) { return a.draws > b.draws; });
    census.dispatches = s.current_dispatches;
    census.skipped_dispatches = s.current_skipped_dispatches;
    census.dispatch_rows.reserve(s.current_dispatch_rows.size());
    for (const auto& [hash, row] : s.current_dispatch_rows) {
        census.dispatch_rows.push_back(row);
    }
    std::stable_sort(census.dispatch_rows.begin(), census.dispatch_rows.end(),
                     [](const DispatchRow& a, const DispatchRow& b) { return a.count > b.count; });
    s.last = std::move(census);
    ClearCurrentLocked(s);
    --s.census_presents_left;
    UpdateActiveLocked(s);
}

std::optional<std::string> DumpRequested(const DrawInfo& info) {
    State& s = S();
    std::scoped_lock lock{s.mutex};
    if (!s.dump.armed || s.dump.done) {
        return std::nullopt;
    }
    if (std::find(info.hashes.begin(), info.hashes.end(), s.dump.hash) == info.hashes.end()) {
        return std::nullopt;
    }
    // Claimed: one draw only.
    s.dump.done = true;
    UpdateActiveLocked(s);
    return fmt::format("{}/nvn-draw-dump-{:016x}-p{}.txt", s.dump_directory, s.dump.hash,
                       s.presents);
}

void DumpDone(std::string path, std::string error) {
    State& s = S();
    std::scoped_lock lock{s.mutex};
    s.dump.path = std::move(path);
    s.dump.error = std::move(error);
}

bool ArmCensus(u32 presents) {
    State& s = S();
    std::scoped_lock lock{s.mutex};
    const bool running = s.census_presents_left != 0;
    if (!running) {
        ClearCurrentLocked(s);
        s.census_partial = true;
    }
    s.census_presents_left = std::max(s.census_presents_left, presents);
    UpdateActiveLocked(s);
    return running;
}

Census LastCensus() {
    State& s = S();
    std::scoped_lock lock{s.mutex};
    return s.last;
}

void Reset() {
    State& s = S();
    std::scoped_lock lock{s.mutex};
    s.census_presents_left = 0;
    s.census_partial = false;
    ClearCurrentLocked(s);
    s.last = {};
    s.skip.clear();
    s.skip_mode = SkipMode::Skip;
    s.dump = {};
    s.dump_directory.clear();
    UpdateActiveLocked(s);
}

void SetSkip(std::vector<u64> hashes, SkipMode mode) {
    State& s = S();
    std::scoped_lock lock{s.mutex};
    std::sort(hashes.begin(), hashes.end());
    hashes.erase(std::unique(hashes.begin(), hashes.end()), hashes.end());
    s.skip = std::move(hashes);
    s.skip_mode = mode;
    UpdateActiveLocked(s);
}

void GetSkip(std::vector<u64>& hashes, SkipMode& mode) {
    State& s = S();
    std::scoped_lock lock{s.mutex};
    hashes = s.skip;
    mode = s.skip_mode;
}

void ArmDump(u64 hash, std::string directory) {
    State& s = S();
    std::scoped_lock lock{s.mutex};
    s.dump = DumpState{.armed = true, .done = false, .hash = hash};
    s.dump_directory = std::move(directory);
    UpdateActiveLocked(s);
}

DumpState GetDump() {
    State& s = S();
    std::scoped_lock lock{s.mutex};
    return s.dump;
}

} // namespace VideoCommon::NvnDrawProbe

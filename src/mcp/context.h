// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <string>

namespace Core {
class System;
}

namespace Mcp {

class ControlLease;
class Host;
class Timeline;

/// Everything a tool handler may use. Pointers can be null in unit tests.
struct ToolContext {
    Core::System* system = nullptr;
    Host* host = nullptr;
    Timeline* timeline = nullptr;
    bool writes_allowed = false;
    ControlLease* control = nullptr;
    std::string session_id; ///< MCP session making the call.
    /// The server's live Allow Writes flag (null in unit tests: `writes_allowed` is used).
    const std::atomic<bool>* live_writes = nullptr;

    /// Allow Writes right now, for work that outlasts the call (background routes) and handlers
    /// that wait for a lock: `writes_allowed` is only a copy taken when the call arrived.
    [[nodiscard]] bool WritesAllowed() const {
        return live_writes != nullptr ? live_writes->load(std::memory_order_relaxed) : writes_allowed;
    }

    /// True when a system is set, powered on and not shutting down.
    [[nodiscard]] bool GameRunning() const;
};

} // namespace Mcp

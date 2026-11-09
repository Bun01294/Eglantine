// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <set>

#include <catch2/catch_test_macros.hpp>

#include "common/settings.h"
#include "core/debugger/mapped_range_hook.h"
#include "mcp/control.h"
#include "mcp/protocol.h"
#include "mcp/timeline.h"
#include "mcp/tool_registry.h"
#include "mcp/tools/tool_helpers.h"

using namespace Mcp;

namespace {

ToolRegistry MakeRegistry() {
    ToolRegistry registry;
    registry.Add(Tool{
        .name = "echo",
        .description = "Echo text",
        .input_schema = {{"type", "object"},
                         {"properties", {{"text", {{"type", "string"}, {"maxLength", 10}}}}},
                         {"required", {"text"}},
                         {"additionalProperties", false}},
        .annotations = {.read_only = true},
        .handler = [](ToolContext&, const json& a) { return ToolResult::Text(a["text"].get<std::string>()); },
    });
    registry.Add(Tool{
        .name = "needs_game",
        .description = "Needs a game",
        .requires_game = true,
        .handler = [](ToolContext&, const json&) { return ToolResult::Text("ran"); },
    });
    registry.Add(Tool{
        .name = "needs_write",
        .description = "Needs writes",
        .requires_write = true,
        .handler = [](ToolContext&, const json&) { return ToolResult::Text("ran"); },
    });
    registry.Add(Tool{
        .name = "throws",
        .description = "Throws",
        .handler = [](ToolContext&, const json&) -> ToolResult {
            throw std::runtime_error("boom");
        },
    });
    return registry;
}

HttpRequest Post(const json& body, const std::string& session = "") {
    HttpRequest r;
    r.method = "POST";
    r.path = "/mcp";
    r.body = body.dump();
    r.headers["accept"] = "application/json, text/event-stream";
    if (!session.empty()) {
        r.headers["mcp-session-id"] = session;
    }
    return r;
}

std::string Initialize(Protocol& p, const std::string& version = "2025-06-18") {
    const auto res = p.Handle(Post(json{{"jsonrpc", "2.0"},
                                        {"id", 1},
                                        {"method", "initialize"},
                                        {"params", {{"protocolVersion", version}}}}));
    REQUIRE(res.status == 200);
    for (const auto& [k, v] : res.headers) {
        if (k == "Mcp-Session-Id") {
            return v;
        }
    }
    FAIL("no session id");
    return {};
}

json Call(Protocol& p, const std::string& session, const std::string& tool, json args) {
    const auto res = p.Handle(Post(json{{"jsonrpc", "2.0"},
                                        {"id", 7},
                                        {"method", "tools/call"},
                                        {"params", {{"name", tool}, {"arguments", args}}}},
                                   session));
    REQUIRE(res.status == 200);
    return json::parse(res.body);
}

} // namespace

TEST_CASE("MCP initialize negotiates version and creates a session", "[mcp]") {
    const auto registry = MakeRegistry();
    Protocol p{registry, {}, {}};
    const auto res = p.Handle(Post(json{{"jsonrpc", "2.0"},
                                        {"id", 1},
                                        {"method", "initialize"},
                                        {"params", {{"protocolVersion", "2025-06-18"}}}}));
    REQUIRE(res.status == 200);
    const auto body = json::parse(res.body);
    REQUIRE(body["result"]["protocolVersion"] == "2025-06-18");
    REQUIRE(body["result"]["capabilities"].contains("tools"));
    REQUIRE(p.SessionCount() == 1);

    const auto other = json::parse(
        p.Handle(Post(json{{"jsonrpc", "2.0"},
                           {"id", 2},
                           {"method", "initialize"},
                           {"params", {{"protocolVersion", "1999-01-01"}}}}))
            .body);
    REQUIRE(other["result"]["protocolVersion"] == kSupportedProtocolVersions[0]);
}

TEST_CASE("MCP session rules", "[mcp]") {
    const auto registry = MakeRegistry();
    Protocol p{registry, {}, {}};
    const json ping{{"jsonrpc", "2.0"}, {"id", 3}, {"method", "ping"}};
    REQUIRE(p.Handle(Post(ping)).status == 400);           // missing session
    REQUIRE(p.Handle(Post(ping, "deadbeef")).status == 404); // unknown session
    const auto session = Initialize(p);
    REQUIRE(p.Handle(Post(ping, session)).status == 200);

    // Notifications are accepted without a body.
    const auto note = p.Handle(Post(json{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}, session));
    REQUIRE(note.status == 202);
    REQUIRE(note.body.empty());

    HttpRequest del;
    del.method = "DELETE";
    del.path = "/mcp";
    del.headers["mcp-session-id"] = session;
    REQUIRE(p.Handle(del).status == 204);
    REQUIRE(p.Handle(Post(ping, session)).status == 404);
}

TEST_CASE("MCP HTTP rules", "[mcp]") {
    const auto registry = MakeRegistry();
    ProtocolOptions options;
    options.bearer_token = "secret";
    Protocol p{registry, options, {}};

    HttpRequest req = Post(json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"}});
    REQUIRE(p.Handle(req).status == 401);
    req.headers["authorization"] = "Bearer secret";
    REQUIRE(p.Handle(req).status == 200);

    req.headers["origin"] = "http://evil.example";
    REQUIRE(p.Handle(req).status == 403);
    req.headers["origin"] = "http://localhost:3000";
    REQUIRE(p.Handle(req).status == 200);
    REQUIRE_FALSE(Protocol::IsAllowedOrigin("http://localhost.evil.example"));

    // DNS rebinding: same-origin requests carry no Origin, but Host names the attacker.
    req.headers.erase("origin");
    req.headers["host"] = "rebind.evil.example:6544";
    REQUIRE(p.Handle(req).status == 403);
    req.headers["host"] = "127.0.0.1:6544";
    REQUIRE(p.Handle(req).status == 200);
    REQUIRE(Protocol::IsAllowedHost("localhost"));
    REQUIRE(Protocol::IsAllowedHost("[::1]:6544"));
    REQUIRE_FALSE(Protocol::IsAllowedHost("localhost.evil.example"));
    REQUIRE_FALSE(Protocol::IsAllowedHost("127.0.0.1.evil.example:6544"));

    REQUIRE(Protocol::TokenMatches("Bearer secret", "Bearer secret"));
    REQUIRE_FALSE(Protocol::TokenMatches("Bearer secreT", "Bearer secret"));
    REQUIRE_FALSE(Protocol::TokenMatches("Bearer secre", "Bearer secret"));
    REQUIRE_FALSE(Protocol::TokenMatches("Bearer secret2", "Bearer secret"));
    REQUIRE_FALSE(Protocol::TokenMatches("", "Bearer secret"));

    HttpRequest get = req;
    get.method = "GET";
    REQUIRE(p.Handle(get).status == 405);

    HttpRequest batch = req;
    batch.body = "[]";
    REQUIRE(p.Handle(batch).status == 400);

    HttpRequest bad = req;
    bad.body = "{not json";
    REQUIRE(json::parse(p.Handle(bad).body)["error"]["code"] == -32700);

    // Fetch Metadata: a plain cross-site GET carries no Origin but says where it comes from.
    REQUIRE(Protocol::IsAllowedFetchSite(""));
    REQUIRE(Protocol::IsAllowedFetchSite("same-origin"));
    REQUIRE(Protocol::IsAllowedFetchSite("none"));
    REQUIRE_FALSE(Protocol::IsAllowedFetchSite("cross-site"));
    REQUIRE_FALSE(Protocol::IsAllowedFetchSite("same-site"));
    HttpRequest cross = req;
    cross.headers["sec-fetch-site"] = "cross-site";
    REQUIRE(p.Handle(cross).status == 403);
    cross.headers["sec-fetch-site"] = "same-origin";
    REQUIRE(p.Handle(cross).status == 200);

    HttpRequest deep = req;
    deep.body = std::string(100000, '[') + std::string(100000, ']');
    REQUIRE(p.Handle(deep).status == 400);

    HttpRequest wrong_path = req;
    wrong_path.path = "/other";
    REQUIRE(p.Handle(wrong_path).status == 404);
}

TEST_CASE("MCP tools/list and tools/call", "[mcp]") {
    const auto registry = MakeRegistry();
    Protocol p{registry, {}, [] { return ToolContext{}; }};
    const auto session = Initialize(p);

    const auto list = json::parse(
        p.Handle(Post(json{{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}}, session)).body);
    REQUIRE(list["result"]["tools"].size() == 4);
    REQUIRE(list["result"]["tools"][0].contains("inputSchema"));

    auto r = Call(p, session, "echo", {{"text", "hi"}});
    REQUIRE(r["result"]["content"][0]["text"] == "hi");

    REQUIRE(Call(p, session, "echo", json::object())["error"]["code"] == -32602);
    REQUIRE(Call(p, session, "echo", {{"text", "hi"}, {"x", 1}})["error"]["code"] == -32602);
    REQUIRE(Call(p, session, "echo", {{"text", "much too long"}})["error"]["code"] == -32602);
    REQUIRE(Call(p, session, "nope", json::object())["error"]["code"] == -32602);

    REQUIRE(Call(p, session, "needs_game", json::object())["result"]["isError"] == true);
    REQUIRE(Call(p, session, "needs_write", json::object())["result"]["isError"] == true);
    REQUIRE(Call(p, session, "throws", json::object())["error"]["code"] == -32603);
    // The server keeps working after a handler throws.
    REQUIRE(Call(p, session, "echo", {{"text", "ok"}})["result"]["content"][0]["text"] == "ok");

    const auto unknown = json::parse(
        p.Handle(Post(json{{"jsonrpc", "2.0"}, {"id", 9}, {"method", "bogus/method"}}, session)).body);
    REQUIRE(unknown["error"]["code"] == -32601);
}

TEST_CASE("MCP argument validation", "[mcp]") {
    const json schema{{"type", "object"},
                      {"properties",
                       {{"n", {{"type", "integer"}, {"minimum", 0}, {"maximum", 5}}},
                        {"mode", {{"type", "string"}, {"enum", {"a", "b"}}}},
                        {"list", {{"type", "array"}, {"items", {{"type", "integer"}}}, {"maxItems", 2}}}}}};
    REQUIRE_FALSE(ValidateArguments(schema, json{{"n", 3}, {"mode", "a"}, {"list", {1, 2}}}));
    REQUIRE(ValidateArguments(schema, json{{"n", 9}}));
    REQUIRE(ValidateArguments(schema, json{{"n", "3"}}));
    REQUIRE(ValidateArguments(schema, json{{"mode", "c"}}));
    REQUIRE(ValidateArguments(schema, json{{"list", {1, 2, 3}}}));
    REQUIRE(ValidateArguments(schema, json{{"list", {1, "x"}}}));
    REQUIRE(IsValidToolName("read_memory"));
    REQUIRE_FALSE(IsValidToolName("ReadMemory"));
    REQUIRE_FALSE(IsValidToolName("1abc"));
}

TEST_CASE("MCP events with invalid UTF-8 always serialize", "[mcp]") {
    Timeline t{8};
    // A lone 0xFF, Shift-JIS text, a truncated sequence, an overlong form and a surrogate.
    t.Push(EventKind::Log, 2, "Debug\xff", "bad \xff byte");
    t.Push(EventKind::Log, 2, "Service", "\x83\x65\x83\x58\x83\x67");
    t.Push(EventKind::Log, 2, "Service", "cut \xE3\x81");
    t.Push(EventKind::Log, 2, "Service", "overlong \xC0\xAF surrogate \xED\xA0\x80 ok \xE3\x81\x82");
    t.Push(EventKind::Log, 2, "Service", std::string(100000, 'x'));
    const auto r = t.Read({});
    REQUIRE(r.events.size() == 5);
    for (const auto& e : r.events) {
        REQUIRE_NOTHROW(EventJson(e).dump());
        REQUIRE(e.message.size() <= kMaxMessageBytes);
    }
    REQUIRE(r.events[0].message == "bad \xEF\xBF\xBD byte");
    REQUIRE(r.events[3].message.find("\xE3\x81\x82") != std::string::npos);
    REQUIRE(SanitizeText("a\xF0\x9F\x98\x80z", 100) == "a\xF0\x9F\x98\x80z"); // valid 4-byte
    REQUIRE(SanitizeText("a\xF0\x9F\x98\x80z", 3) == "a");                       // not cut inside
    // Tool results with raw guest bytes serialize too (replaced, not thrown).
    REQUIRE_NOTHROW(ToolResult::Structured(json{{"s", std::string("\xff\xfe")}}));
}

TEST_CASE("MCP Allow Writes is never saved", "[mcp]") {
    REQUIRE_FALSE(Settings::values.mcp_allow_writes.Save());
    REQUIRE_FALSE(Settings::values.mcp_allow_writes.GetValue());
    REQUIRE_FALSE(Settings::values.enable_mcp_server.GetValue());
}

TEST_CASE("MCP timeline cursor, filters and wraparound", "[mcp]") {
    Timeline t{4};
    for (int i = 0; i < 6; ++i) {
        t.Push(i % 2 ? EventKind::Mark : EventKind::Log, 2, i % 2 ? "agent" : "Service.FS",
               "event " + std::to_string(i));
    }
    REQUIRE(t.LastSeq() == 6);

    auto r = t.Read({});
    REQUIRE(r.dropped == 2); // seq 1-2 were overwritten
    REQUIRE(r.events.size() == 4);
    REQUIRE(r.events.front().seq == 3);
    REQUIRE(r.next_seq == 6);
    REQUIRE(t.Read({.since_seq = 6}).events.empty());

    Timeline::Query logs;
    logs.kinds = {EventKind::Log};
    REQUIRE(t.Read(logs).events.size() == 2);

    Timeline::Query limited;
    limited.limit = 1;
    r = t.Read(limited);
    REQUIRE(r.events.size() == 1);
    REQUIRE(r.truncated);
    limited.since_seq = r.next_seq;
    REQUIRE(t.Read(limited).events.front().seq == 4);

    Timeline::Query prefix;
    prefix.source_prefix = "Service";
    prefix.contains = "4";
    REQUIRE(t.Read(prefix).events.size() == 1);

    Timeline::Query wait;
    wait.since_seq = t.LastSeq();
    REQUIRE_FALSE(t.WaitFor(wait, std::chrono::milliseconds{1}));
    t.Push(EventKind::Crash, 0, "core", "fatal");
    wait.kinds = {EventKind::Crash};
    REQUIRE(t.WaitFor(wait, std::chrono::milliseconds{1})->message == "fatal");
}

TEST_CASE("MCP control lease", "[mcp]") {
    ControlLease lease{std::chrono::seconds{60}};
    ControlLease::Holder h;
    REQUIRE(lease.Touch("a", h));            // free: granted implicitly
    REQUIRE(h.session_id == "a");
    REQUIRE_FALSE(lease.Touch("b", h));      // held by a
    REQUIRE(h.session_id == "a");
    REQUIRE_FALSE(lease.Acquire("b", "bot", std::nullopt, h));
    REQUIRE_FALSE(lease.Release("b"));       // only the holder releases
    REQUIRE(lease.Release("a"));
    REQUIRE(lease.Acquire("b", "bot", std::chrono::seconds{0}, h)); // expires immediately
    REQUIRE(lease.Touch("a", h));            // expired, so a gets it
    lease.Reset();
    REQUIRE_FALSE(lease.Current().has_value());
}

TEST_CASE("MCP protocol enforces the control lease", "[mcp]") {
    ToolRegistry registry;
    registry.Add(Tool{
        .name = "drive",
        .description = "Needs control",
        .requires_control = true,
        .handler = [](ToolContext&, const json&) { return ToolResult::Text("driving"); },
    });
    ControlLease lease;
    Protocol p{registry, {}, [&lease] {
                   ToolContext c;
                   c.control = &lease;
                   return c;
               }};
    const auto a = Initialize(p);
    const auto b = Initialize(p);
    REQUIRE(Call(p, a, "drive", json::object())["result"]["content"][0]["text"] == "driving");
    REQUIRE(Call(p, b, "drive", json::object())["result"]["isError"] == true);
    REQUIRE(lease.Release(a));
    REQUIRE(Call(p, b, "drive", json::object())["result"]["content"][0]["text"] == "driving");
}

TEST_CASE("MCP refused calls do not take the control lease", "[mcp]") {
    ToolRegistry registry;
    registry.Add(Tool{
        .name = "poke",
        .description = "Needs writes and control",
        .requires_write = true,
        .requires_control = true,
        .handler = [](ToolContext&, const json&) { return ToolResult::Text("poked"); },
    });
    ControlLease lease;
    Protocol p{registry, {}, [&lease] {
                   ToolContext c;
                   c.control = &lease;
                   c.writes_allowed = false;
                   return c;
               }};
    const auto a = Initialize(p);
    REQUIRE(Call(p, a, "poke", json::object())["result"]["isError"] == true);
    REQUIRE_FALSE(lease.Current().has_value());
}

namespace Mcp {
void RegisterAllTools(ToolRegistry& registry);
}

TEST_CASE("MCP every built-in tool has a valid definition", "[mcp]") {
    ToolRegistry registry;
    RegisterAllTools(registry); // asserts on duplicate or malformed names
    REQUIRE(registry.Size() >= 35); // the subset of FUKKO's tools this build carries
    registry.ForEach([](const Tool& tool) {
        INFO(tool.name);
        REQUIRE(IsValidToolName(tool.name));
        REQUIRE(!tool.description.empty());
        REQUIRE(tool.handler);
        REQUIRE(tool.input_schema.value("type", "") == "object");
        // Writes need the lease too, so one agent can't change state under another.
        if (tool.requires_write) {
            REQUIRE(tool.requires_control);
        }
        // Read-only until Allow Writes: a tool that changes anything needs writes, except these
        // that only manage the lease, mark the timeline or switch watches.
        static const std::set<std::string> kNoWriteGate{"acquire_control", "release_control",
                                                        "mark", "watch_add", "watch_remove"};
        if (!tool.annotations.read_only && !tool.requires_write) {
            REQUIRE(kNoWriteGate.contains(tool.name));
        }
        if (tool.annotations.destructive) {
            REQUIRE(tool.requires_write);
        }
        // An empty object must either validate or fail only for missing required fields.
        const auto error = ValidateArguments(tool.input_schema, json::object());
        if (error) {
            REQUIRE(tool.input_schema.contains("required"));
        }
        // Definitions serialize for tools/list.
        const json def = tool.DefinitionJson();
        REQUIRE(def["name"] == tool.name);
        REQUIRE(def.contains("inputSchema"));
    });
}

TEST_CASE("MCP frames fall back to the last presented screenshot", "[mcp]") {
    ToolContext ctx; // no system: the capture layer is unavailable, as under VTX-HLE
    RememberScreenshot({0x89, 'P', 'N', 'G'}, 1234);
    const auto last = LastScreenshot();
    REQUIRE(last);
    REQUIRE(last->frame == 1234);
    std::string source;
    std::string error;
    const auto png = FallbackFrame(ctx, 640, source, error);
    REQUIRE(png);
    REQUIRE(png->size() == 4);
    REQUIRE(source.find("last presented screenshot (guest frame 1234") != std::string::npos);
    // Without a host a screenshot fails cleanly and leaves the remembered frame alone.
    REQUIRE_FALSE(CaptureScreenshot(ctx, 640, std::chrono::milliseconds{1}, error));
    REQUIRE(LastScreenshot()->frame == 1234);
}

TEST_CASE("MCP watches can sample at invalidate; mapped-range lines are queryable", "[mcp]") {
    ToolRegistry registry;
    RegisterAllTools(registry);
    const Tool* watch_add = registry.Find("watch_add");
    REQUIRE(watch_add);
    REQUIRE_FALSE(ValidateArguments(watch_add->input_schema,
                                    json{{"address", "0x1000"}, {"type", "f32"}, {"at", "invalidate"}}));
    REQUIRE_FALSE(ValidateArguments(watch_add->input_schema, json{{"address", 4096}, {"at", "present"}}));
    REQUIRE(ValidateArguments(watch_add->input_schema, json{{"address", 4096}, {"at", "later"}}));
    // The invalidate hook is only installed while an at:"invalidate" watch exists.
    REQUIRE(Core::Trace::g_invalidate_hook.load() == nullptr);

    // EDEN_NX2_DIAG_INVALIDATE lines reach the timeline as Core log events through the log sink.
    Timeline t{16};
    t.Push(EventKind::Log, 2, "Core", "[nvn] mapped-range flush f10 addr=0x1000 size=0xc0 words: 0");
    t.Push(EventKind::Log, 2, "Core",
           "[nvn] mapped-range invalidate f10 addr=0x1000 size=0xc0 last_seq=5 completed=5 words: "
           "3e1db22d");
    t.Push(EventKind::Log, 2, "Render.Vulkan", "[nvn] mapped-range invalidate elsewhere");
    const auto q = ParseQuery(json{{"kinds", {"log"}},
                                   {"source_prefix", "Core"},
                                   {"contains", "mapped-range invalidate"}});
    const auto r = t.Read(q);
    REQUIRE(r.events.size() == 1);
    REQUIRE(r.events[0].message.find("3e1db22d") != std::string::npos);
}

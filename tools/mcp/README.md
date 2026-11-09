# Built-in MCP server

Eglantine can expose a [Model Context Protocol](https://modelcontextprotocol.io) server so AI
agents can inspect and drive the running emulator.

MCP server from FUKKO by Vortex727 (https://github.com/Vortex727/FUKKO), originally written by
quixomatic. This build carries a subset of its tools (see [Not in this build](#not-in-this-build)).

## Enable

The server is **off by default**. Turn it on in one of these ways:

- **Tools > MCP Server > Enabled** (remembered as the `enable_mcp_server` setting), or
- start the emulator with `--mcp` (this run only; `--mcp-port <port>` implies `--mcp`).

It listens on `http://127.0.0.1:6544/mcp` (Streamable HTTP, loopback only, so only programs on
this PC can reach it). The **Tools > MCP Server** menu also has *Allow Writes* (lets agents change
emulator state), *Open Dashboard* and actions that copy the URL or the `claude mcp add` command to
the clipboard. A status-bar label shows whether it is running and what agents are doing.

Settings: `enable_mcp_server` (default off), `mcp_port` (6544), `mcp_allow_writes` (off; never
saved, so writes are granted per session), `mcp_token` (optional bearer token). Command line:
`--mcp`, `--mcp-port <port>`, `--mcp-allow-writes`.

Builds include the server unless configured with `-DENABLE_MCP_SERVER=OFF`.

It is separate from the NX2 debug server (`EDEN_NX2_DEBUG_SERVER`, port 6545,
`tools/nx2/debug_mcp.py`); both can run at the same time on their own ports.

## Connect

Claude Code:

```sh
claude mcp add --transport http eglantine http://127.0.0.1:6544/mcp
```

Or a project `.mcp.json`:

```json
{ "mcpServers": { "eglantine": { "type": "http", "url": "http://127.0.0.1:6544/mcp" } } }
```

Stdio-only clients can use a bridge such as `npx mcp-remote http://127.0.0.1:6544/mcp`.
Test with `npx @modelcontextprotocol/inspector` (transport: Streamable HTTP).

## Tools

W = needs *Allow Writes*; L = needs the control lease (granted automatically when no other
agent holds it, see `acquire_control`).

| Tool | What it does |
|---|---|
| `status` | Running/paused, program id, title, build id, guest frame, perf, version |
| `timeline` | Cursor-based read of everything recorded (log lines, state, marks, watches...) |
| `wait_for` | Block until a matching timeline event happens |
| `mark` | Insert a labelled marker into the timeline |
| `act` | Input (W, L) + wait (event / guest frames / ms) + capture (screenshot, timeline, controller, perf) in one call |
| `pause`, `resume` (W, L) | Pause or resume emulation |
| `frame_advance` (W, L), `capture_frames` | Run N frames then pause, with an image of the last advanced frame (`image_source` says where it came from); a screenshot burst every N frames (refused while paused) |
| `screenshot` | PNG of the presented frame (while paused: the capture layer or the last presented screenshot, labelled) |
| `press_buttons`, `set_stick`, `input_sequence`, `release_all` (W, L) | Virtual gamepad input (wall-clock) |
| `tas_status`, `tas_read_script` | TAS engine state; read a script or the last recording |
| `tas_write_script`, `tas_play`, `tas_stop`, `tas_record_start`, `tas_record_stop`, `tas_set_options` (W, L) | Frame-locked TAS scripts (write as steps or raw TAS-nx, play with screenshots at frames, record a human) |
| `memory_map` | Mapped regions of the application process |
| `read_memory`, `search_memory`, `write_memory` (W, L) | Guest memory (typed or hex; pattern search with `??`) |
| `watch_add`, `watch_list`, `watch_remove` | Values sampled every frame; changes become `watch` timeline events |
| `summarize_since`, `timeline_export` (W, L), `compare_runs` | Clustered digest; JSONL export under `<log dir>/mcp`; diff two runs |
| `set_determinism` (W, L), `env_switches` | Fixed RNG/RTC/language/region; active `EDEN_*` variables (`EDEN_*` only) |
| `control_status`, `acquire_control`, `release_control` | Share one emulator between several agents |

`act` accepts `screen_state` / `probes` captures and a `screen` wait for compatibility with
FUKKO clients; in this build they report "not available in this build".

MCP resources: `eglantine://tools/guide`, `eglantine://status`, `eglantine://timeline/summary`.
Prompt: `compare_change`. While running, the server writes `<user dir>/mcp-server.json` (url,
port, pid; never the token). Log events redact key-like hex.

### Images while paused

Renderers fill screenshot requests only when a frame is presented, and the VTX-HLE replay never
renders the applet capture layer. So `frame_advance` captures its image as the last advanced
frame is presented, before it pauses again (`image_source: "presented frame ..."`). If that fails
it falls back to the capture layer (emulated GPU path) and then to the last presented screenshot
any tool took, with its guest frame and age. `screenshot` uses the same fallback while paused;
`capture_frames` refuses to run while paused.

### Not in this build

`shader_words`, `ipc_trace`, `ipc_summary`, `svc_trace`, `list_services`,
`unimplemented_commands`, `scan_start`/`scan_next`/`scan_results`, `probe_*`, `route_*`,
`screen_label`/`screen_labels`/`screen_state`/`screenshot_raw`, `baseline_*`, `settings_*`,
`debug_knobs`, `perf_report`, `perf_sample`, `tap`, `swipe`, `set_docked`, `list_games`,
`boot_game`, `stop`, `quit_emulator`, `game_info`, `firmware_status`, `process_info`,
`list_threads`, `backtrace`, `set_log_filter`, `get_controller_state`. Crash and unimplemented
command events are not posted to the timeline either (FUKKO's core trace hooks are not ported).

## Other HTTP endpoints (same port, same Host/Origin/token checks)

| Endpoint | Use |
|---|---|
| `GET /` | Read-only dashboard: live screen, status, control holder, live event log |
| `GET /events/wait?kinds=watch,mark&timeout_s=600` | Blocks until a matching event, then returns it: run it as a background `curl` to be woken by it |
| `GET /stream/timeline?min_level=info` | The timeline as server-sent events |
| `GET /mcp` + `Accept: text/event-stream` + session | MCP server-to-client stream: `notifications/message` for warning+ logs |
| `GET /status.json`, `GET /screen.png?w=640` | Data behind the dashboard |

With `mcp_token` set, pass `Authorization: Bearer <token>` (or `?token=` in a browser).

## eden-cli

`eden-cli --mcp [--mcp-port N] [--mcp-allow-writes] -g <game>` serves the same tools for
unattended runs (the game is the one on the command line). eden-cli starts the server only with
`--mcp`.

## Code

`src/mcp/`: transport-independent protocol (`protocol.*`), tool registry and argument
validation (`tool_registry.*`), event timeline (`timeline.*`), server/HTTP binding and extra
endpoints (`server.*`, `dashboard.h`), tools (`tools/`, shared pieces in `tool_helpers.h`).
Frontends implement `Mcp::Host`: `src/yuzu/mcp_host_qt.*` (Qt), `src/yuzu_cmd/mcp_host_cli.*`
(eden-cli). The timeline is fed by the logging sink (`Common::Log::SetExternalSink`).
Unit tests: `src/tests/mcp/` (`tests "[mcp]"`). Minimal command-line client:
`tools/mcp/scripts/mcp.py <tool> [json-args]` (set `EGLANTINE_MCP_TOKEN` when a token is
configured, and `EGLANTINE_MCP_SID_FILE` per agent so each one gets its own session and control
lease).

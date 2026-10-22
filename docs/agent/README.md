# Sonora application control

Sonora provides a shared message-thread control API, an **Agent Control** panel and an MCP bridge for external agents.
MCP needs no Sonora model API key: enter your prompt in the MCP client. Optionally configure a native LLM to chat
inside Sonora. See [NATIVE_LLM.md](NATIVE_LLM.md) for provider/key setup and [CONTROL_API.md](CONTROL_API.md) for operations.

Build with the usual CMake/Ninja commands. In the rebuilt app choose **View > Agent Control** or **Cmd+Shift+P**.
The default panel shows the MCP access grant, request activity, readable action results, full receipt details and guarded
undo. There is no prompt box or Run button until a native LLM is enabled with a model, endpoint and saved API key.

The Serum recipe requires the scanned **Serum 2 instrument VST3, version 2.0.24**, a 4/4 project and a usable plugin
installation. It adds a new MIDI track at bar 1, keeps the current tempo, creates D#m/B/F#/C# chords over 16 beats,
and prepares an original seven-voice supersaw configuration using the plugin's initialized Default Shapes wavetable.
No vendor preset or wavetable data is copied into the repository. This is the real hosted plugin.

Both external MCP agents and the optional native LLM reason over the same typed tools. MCP exposes general MIDI note add/update/delete/replace in Arrangement, Session and takes, plus exact hosted VST3
parameter batches with undo. Native devices and rack controls use those same tools, with actual unit/range/choice metadata.
Agents can discover and insert implemented native devices/scanned VST3 plugins on existing tracks, or remove devices with
captured-live undo. Parameter commits rebuild the affected processor and return a new device ID. Unexposed/nested controls,
media/content authoring and broader track operations remain incremental work; inspect the tool descriptions.

## Connect an MCP client

1. In the intended open project, enable **Allow MCP access to this project** in Agent Control. This grants same-user local agents
   access to inspect/edit that project. It is disabled again on project switch or app exit.
2. Install the bridge dependencies with `npm ci --prefix packages/mcp-server` from the repository root. Node.js 22 or later
   is required for this development package. SDK and schema-library versions are pinned in its lockfile.
3. Configure the MCP client to launch Node with an absolute path to `packages/mcp-server/server.mjs`. For this checkout,
   an example configuration is:

```json
{
  "mcpServers": {
    "sonora": {
      "command": "/opt/homebrew/bin/node",
      "args": ["/path/to/sonora/packages/mcp-server/server.mjs"]
    }
  }
}
```

4. Ask the agent to list Sonora instances, attach to the explicit instance/document you selected, inspect the project,
   and execute the requested workflow. The bridge does not follow the foreground window or silently switch documents.

The official MCP SDK v2 handles modern 2026-07-28 discovery/metadata and legacy initialize-based clients. Tests cover
both protocol eras. This package is a local stdio bridge; it opens no HTTP or LAN listener. The current grant is scoped
to the project and local OS user, with owner-only credentials; per-client pairing/scopes and a bundled Node runtime
are future release work. Do not share registry credentials or commit them to projects.

The app shows MCP operation outcomes in Agent Control. Request activity means an authenticated request was received;
stdio MCP does not provide a persistent connected-client session. The bridge exposes this guide's API companion as
the read-only resource `sonora://docs/control-v1`.

### Codex on this Mac

Codex uses TOML rather than the generic client JSON above. The shared user configuration is
`~/.codex/config.toml`; keep other server entries intact. This checkout is registered as:

```toml
[mcp_servers.sonora]
command = "/opt/homebrew/bin/node"
args = ["/path/to/sonora/packages/mcp-server/server.mjs"]
tool_timeout_sec = 150
```

The timeout accommodates the bridge's bounded native-operation wait. No credential belongs in this configuration.
For a fresh installation, after installing dependencies, the equivalent registration command is:

```bash
codex mcp add sonora -- /opt/homebrew/bin/node /path/to/sonora/packages/mcp-server/server.mjs
codex mcp get sonora
```

See the [official Codex MCP configuration documentation](https://developers.openai.com/codex/mcp/).
The client starts the bridge on demand over stdio; no daemon, HTTP endpoint, port forwarding or firewall change is needed.
For a manual stdio launch, run the absolute Node command above; closing stdin or pressing Ctrl+C stops that bridge.
Removing the client registration is `codex mcp remove sonora`. Disabling the checkbox in Sonora revokes project access
independently of the client registration. Do not remove other MCP servers or quit an app with unsaved work to reconnect.

After changing configuration, use a new Codex session that loads it. If the current desktop session does not show
`sonora`, reload its MCP configuration or restart the client when convenient; editing the file does not retrofit the
tool catalog of an already running conversation. In Sonora, choose the intended project, open **View > Agent Control**, and
explicitly enable **Allow MCP access to this project**. Then ask Codex to list instances, attach to the chosen instance/document, and
inspect it without edits. An empty instance list means no enabled project was discovered; it is not an app-control pass.

## Verification

```bash
./build/tests/sonora_tests 'control:'
./build/tests/sonora_tests 'agent:'
./build/tools/control_probe
./build/tools/control_probe --vital                    # second-vendor generic VST3 check
./build/tools/control_probe --native-snapshot
python3 scripts/test_native_agent.py --probe ./build/tools/control_probe
npm test --prefix packages/mcp-server
```

`control_probe` creates its own temporary Sonora-authored project. It tests the real Serum instance, MIDI, state
save/reload, offline WAV output and undo/redo without opening an audio device or touching a user's running project.
Its printed output paths contain the disposable set and audio audition. Plugin execution should always have an
outer watchdog; the in-process host cannot safely recover a stuck plugin constructor.

For another build directory, set `SONORA_CONTROL_PROBE` to its absolute `tools/control_probe` path when running the MCP
tests. A native assistant screenshot complements `ui_snapshot`, which only renders the Skia tree.

## MIDI and generic VST3 workflow

Ask the agent to inspect the project and clip, then use `sonora_edits_apply` with `notes.add`, `notes.update`,
`notes.delete` or `notes.replace`. `sonora_clip_get` returns revision-scoped note indices and supports paging;
`scene`/`laneId` select Session/take clips. Each batch is one undo step.

For a plugin, inspect `sonora_parameters_get`, choose exact writable ID/name pairs and call `sonora_parameters_set`
with normalized values. It captures the live before-state, prepares/reload-verifies an isolated edited state and
commits a processor replacement. Use the returned new device ID. Undo restores the captured state and prior ID.
Authored automation is rejected. Arbitrary GUI-only controls/preset dialogs are not parameter tools.

The offline probe and MCP integration test save/reload their own edited disposable projects and verify note data and
real Serum parameter values, including undo/redo. The optional `--vital` probe separately verifies generic
Vital 1.5.5 Volume control, save/reload and undo/redo (requires the scanned plugin). Save/export commands are not exposed to external clients yet.
Restart/reload the stdio bridge after updating it and open the rebuilt app to load the new native service.

## Native controls and device insertion

Ask the agent to inspect the project, list `sonora_devices_catalog`, and insert an exact catalogue entry using
`sonora_devices_insert` on a selected existing track. For example, add Utility, inspect `sonora_parameters_get`, then
send `{parameterId: "Gain", name: "Gain", value: 0.5}` via `sonora_parameters_set`. Use the returned new device ID.
Native normalized writes are also supported and use the descriptor's linear range. Read ranges and choices first.
VST3 writes continue to require normalized values.

`sonora_devices_remove` removes an exact top-level device and retains its live state for undo. A second instrument
is rejected rather than silently replacing the first. Default order is MIDI effects → instrument → audio effects.
All mutations retain document/revision/idempotency guards. The native LLM uses the same operations. See
[CONTROL_API.md](CONTROL_API.md) for automation, units, content requirements and remaining coverage gaps.

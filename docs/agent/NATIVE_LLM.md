# Agent Control: MCP or a native LLM

Open **View > Agent Control** (Cmd+Shift+P). The default panel is for external agents: enable **Allow MCP access to this
project**, then enter the prompt in Codex or another MCP client. Sonora displays authenticated request activity,
readable action results, **Result details…** and guarded **Undo agent edit**. No Sonora LLM key is required for MCP.
An enabled grant is not a persistent client connection. Project switches and app exit revoke it.

To chat inside Sonora, choose **Configure…**, select a provider, review its model/base URL/protocol, enter its API key and
choose **Save & Enable**. Only that enabled configuration with a saved key reveals the prompt and **Run**. Enter runs;
Shift+Enter adds a line; marked IME text is committed without submitting. **Disable native LLM** hides the composer and
keeps the credential. **Remove saved key** deletes the selected provider/endpoint credential and hides its composer.
The native LLM does not require MCP access to be enabled. Both routes execute the same guarded app operations.

## Provider presets

Defaults checked against official documentation on 2026-10-16. They are editable starting points, not a promise that
every account has access to a model. Custom endpoints must implement tool calling for the chosen protocol.

| Provider | Base URL | Default model | Protocol |
| --- | --- | --- | --- |
| OpenAI | `https://api.openai.com/v1` | `gpt-6.1-sol` | Responses |
| Anthropic | `https://api.anthropic.com/v1` | `claude-sonnet-5-5` | Anthropic Messages |
| Gemini | `https://generativelanguage.googleapis.com/v1beta/openai` | `gemini-3.8-flash` | Chat Completions |
| DeepSeek | `https://api.deepseek.com` | `deepseek-flash` | Chat Completions |
| GLM / Z.ai | `https://api.z.ai/api/paas/v4` | `glm-5.3` | Chat Completions |
| OpenRouter | `https://openrouter.ai/api/v1` | `~openai/gpt-sol-latest` | Chat Completions |
| Custom / OpenAI compatible | Enter your endpoint | Enter its model ID | Chat Completions, editable |

Use the base URL, without `/responses`, `/chat/completions` or `/messages`; Sonora appends the selected protocol's path.
HTTPS is required except for HTTP on loopback. URLs containing credentials, queries or fragments are rejected. Redirects
are refused, so authentication cannot follow a redirect to another service. Editing the endpoint or protocol requires
a credential for that exact configuration; another host's saved key is never silently reused.

OpenAI uses Bearer authentication and Responses with `store:false`, including opaque encrypted continuation items.
Compatible providers use Bearer authentication and Chat Completions; complete assistant messages retain tool IDs,
reasoning continuation and Gemini thought signatures. Gemini's schema adapter maps unsupported JSON Schema keywords
to its supported subset; Sonora retains authoritative argument validation. Anthropic uses its Messages endpoint with
`x-api-key` and `anthropic-version: 2023-06-01`, preserves signed continuation blocks, and groups tool results.
Private reasoning is not displayed or saved as a transcript.

References: [OpenAI tools](https://developers.openai.com/api/docs/guides/function-calling),
[OpenAI reasoning](https://developers.openai.com/api/docs/guides/reasoning),
[Anthropic models](https://platform.claude.com/docs/en/models/overview),
[Anthropic tools](https://platform.claude.com/docs/en/agents-and-tools/tool-use/define-tools),
[Anthropic authentication](https://platform.claude.com/docs/en/manage-claude/authentication),
[Gemini compatibility](https://ai.google.dev/gemini-api/docs/openai),
[Gemini schema](https://ai.google.dev/api/generate-content),
[DeepSeek API](https://api-docs.deepseek.com/),
[Z.ai API](https://docs.z.ai/guides/overview/quick-start),
[OpenRouter](https://openrouter.ai/docs/quickstart).

## Supported work and recovery

Example: `Please make me a supersaw lead chord progression with Serum 2, in D# minor, with 4 bars`.
The qualified tool requires the installed/scanned Serum 2 instrument VST3 2.0.24 and 4/4. It creates a new 16-beat clip
with D#m/B/F#/C# chords and an original seven-voice supersaw patch. The model can also inspect clips, edit MIDI notes,
set tempo/track gain, duplicate clips, inspect/filter native/rack/VST3 parameters, set exact parameter batches and undo.
It can discover `sonora_devices_catalog`, insert an implemented native device or scanned VST3 on an existing track,
and remove a top-level device with captured-live undo. Native writes accept processor `value` or linear `normalized`;
VST3 writes accept normalized only. Read descriptors first. Insertion never silently replaces an existing instrument.
See [CONTROL_API.md](CONTROL_API.md) for units, IDs, automation restrictions and remaining coverage gaps.

The eleven native tools use the same request schema and service as MCP. The native adapter supplies its bound instance/document
and fresh per-call idempotency keys; the model must supply the inspected revision for writes. A stale revision/document,
undo conflict or uncertain outcome stops the run. No automatic HTTP or write retries occur. The interface displays
actual receipts separately from model prose. Parameter replacement returns a new device ID; subsequent calls must use it.

Native context is bounded: project overview returns up to 32 tracks and four clips per container; focused `trackId`
inspection pages up to 64 clips per container using `clipOffset`. `trackOffset` pages tracks. The service's existing
512-summary-per-container ceiling still applies. Parameter reads add case-insensitive `filter`, `offset` and `limit`
(default 64, maximum 128). Note reads retain the shared paged API. Tool output is capped at 128 KiB; oversized committed
receipts retain their revision, undo token and entity IDs with `detailsOmitted`, so an applied write cannot look like a
safe retry. Large reads return an error asking for a narrower page.

Each request permits 12 model rounds, 32 tool calls, eight calls per model turn and 1 MiB of outgoing conversation JSON.
Model output is limited to 4096 tokens, HTTP responses to 4 MiB, parsed JSON to depth 64 and individual arguments to
64 KiB. HTTP has a 65-second deadline; a three-minute run budget is checked between requests. This is a bounded
nonstreaming integration. There is no usage/cost estimator yet; provider charges follow your account's terms.

**Stop** cancels HTTP and future calls. An app edit already executing may finish and is reported; it cannot be interrupted
inside a plugin constructor. Already applied edits remain unsaved. Switching projects cancels the run, and every tool
also checks the original document before executing. **Undo agent edit** refuses to erase intervening manual changes.
Use normal application history deliberately if that guard refuses. Closing the app cancels outstanding network work.
The native runtime cannot run arbitrary shell commands or write files. Save, playback, export and audio upload are not
available model tools; a model reply is not proof of saved state or audible quality.

## Credential and data handling

Keys are generic-password entries in macOS Keychain under service `org.sonora.daw.native-agent`, scoped by provider,
protocol and normalized base URL. The secure field is never repopulated with a saved secret. Read access may trigger
the normal macOS Keychain authorization. Only provider/model/endpoint/protocol/enabled metadata is written to
`~/Library/Application Support/Sonora/Agent.json` (owner-only permissions). Keys are not in that file, project XML,
tool arguments or model conversations. HTTP provider error bodies are not displayed; matching credentials are redacted
from model/error text. The transcript is bounded and session-only; prompts and model replies are not persisted.

Running a native prompt sends that prompt and subsequently queried project data to the configured provider, including
names, MIDI and parameter values needed by its tools. The configuration sheet discloses this before enabling it.
Sonora sends no audio file or plugin state blob through these model tools. `store:false` is a request option, not a
claim about every provider's retention policies. MCP clients have their own model/data policies.

## Verification and limits

```bash
./build/tests/sonora_tests 'agent:'
./build/tools/control_probe --keychain
./build/tools/control_probe --native-snapshot
python3 scripts/test_native_agent.py --probe ./build/tools/control_probe
SONORA_CONTROL_PROBE="$PWD/build/tools/control_probe" npm test --prefix packages/mcp-server
```

Use an outer watchdog for probes that instantiate real VST3 plugins. The Keychain probe creates and removes a unique
dummy credential, never using a provider account. The HTTP fixture binds loopback, supplies simulated model turns and
uses the actual native networking/tool loop. Responses, Chat and Anthropic all create a real Serum progression, verify
MIDI/parameters/audio, save/reload and undo/redo in disposable projects. Other cases cover cancellation, document change,
401, redirect refusal, invalid JSON and oversized responses. Protocol unit tests cover hidden continuation, tool outputs,
malformed calls, schema shaping and guarded edits. Native captures cover MCP-only, configured composer and settings.

These checks qualify local framing, transport and app execution. Live calls against the six provider services need a real
provider key and were not made. Account/model compatibility, streamed output, cost UI, helper process isolation, broader
VoiceOver/external-display qualification and complete DAW control remain follow-ups. This implementation does not claim
that the model can execute every possible request or recover a hung in-process third-party plugin.

# ADR 0008 — Agent Control and optional native text input

Status: accepted, amended for MCP-first control with an optional native LLM, 2026-10-16.

ADR 0004 continues to govern DAW rendering. Agent Control is a bounded native AppKit region beside the canvas,
hosted through a JUCE `NSViewComponent`. It uses `NSTextView` for both the editable composer and selectable results,
and native buttons for configuration, execution, guarded undo and the local automation grant. The default panel has no
composer: external agents receive prompts in their MCP client. Prompt/Run appear only for an enabled native provider
with a valid endpoint/model and stored Keychain credential. Disabling that configuration or removing the key hides them.

This supplies OS text selection, text undo, Unicode, IME and accessibility semantics without implementing another text
engine inside Skia. Return submits only when no IME marked text is present; Shift+Return inserts a line. Entering the
composer releases held computer MIDI/mapping keys. The sibling layout avoids drawing native controls over the Metal
surface. Skia/raster canvas behavior remains independent of the assistant.

The first dock has a bounded width and can be shown through View > Agent Control or Cmd+Shift+P. A draggable divider,
conversation persistence, structured action cards and broader VoiceOver/input qualification remain AGT-5/UI-16 work.
Native-view screenshots must supplement `ui_snapshot`, which renders the Skia widget tree only.

Native provider keys were later added alongside MCP. AGT-6a uses a small C++ worker plus Apple's
NSURLSession/Keychain instead of shipping a new TypeScript runtime. It calls the same validated message-thread service
directly; it does not require the external MCP grant. The broader helper-isolation proposal remains future work.
Provider protocol shaping is separate from transport and app execution, with injectable deterministic fixtures.
Metadata may persist locally; credentials never enter the model context, project XML or settings JSON. Conversations
and action transcripts are not persisted. See `../agent/NATIVE_LLM.md` for bounds and verification limits.

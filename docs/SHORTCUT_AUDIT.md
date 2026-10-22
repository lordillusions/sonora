# Keyboard audit — 2026-10-15

Compared the existing handlers in MainLayout, RootWidget, SessionView, ArrangementView, PianoRoll,
ClipEnvelopeEditor, AutomationEditor, TopBar and the parameter widgets with the installed macOS reference
(12.4.6), its inspected menus/tooltips and the official [shortcut reference](https://www.ableton.com/en/manual/live-keyboard-shortcuts/).
This is an implementation audit, not a claim that every reference command exists. UI_GUIDE contains the supported-key table.

## Current routing

- Document commands: New/Open/Save/Save As, Settings, Close, audio export and normal macOS Quit. Open/Settings/Close
  are application commands; editor/global commands run through the canvas. Native menu shortcut labels use the
  right-aligned shortcut field. Sonora's Assistant remains an application-specific extension.
- History and editing: Cmd+Z / Shift+Cmd+Z; contextual Delete, duplication and rename. The old Mac Cmd+Y redo alias
  was removed. Note duplication now has its own action, with new note identities; it cannot duplicate the whole clip.
  Note inversion and activation are editor-local. Parameter Delete resets the focused value.
- Views and navigation: the implemented main views, browser, Clip/Devices and Session mixer sections now have their
  documented toggle/focus keys. Shift+Tab changes the detail editor instead of changing Session/Arrangement. Global
  zoom and full-screen entry points exist. Option-number keys are normalized before dispatch because macOS may emit
  symbols. Text-entry widgets retain their own editing handlers.
- Recording and playback: ordinary F9 targets Arrangement; the modified Session-record key is separate. Space toggles
  playback; Shift+Space continues. Session arrows select slots, Enter launches, and the stop modifier acts on the track.
  Clip creation reaches the same menu operation from either main view. Computer MIDI typing retains note releases and
  exposes single-letter actions when Shift is held.
- Editor grids, time zoom, loop selection, take-lane visibility/audition, draw mode and mapping retain their existing
  handlers. Launch quantization keys now change the actual undoable/saved set value. Note command arrows adjust
  velocity rather than transposing. Unsupported modified note arrows no longer silently move notes.
- Focused widgets and ancestors get the event once. Another editor cannot consume a declined key. Track grouping is
  blocked from note/device/browser focus. The old comp-cycling key no longer reorders take lanes.
- UI-30: Cmd+D on selected Audio/MIDI/group headers duplicates complete tracks (nested groups included); note/clip/take
  duplication retains its own focus. Arrangement Return seeks to the enabled loop start, then Space starts there;
  selected take-range Return still comps. K in the note editor toggles scale highlighting. Right-side icon buttons
  replace the text view selectors; the keyboard view/focus shortcuts are unchanged.
  Option+Shift+T inserts a saved, undoable empty take lane on the selected Audio/MIDI track.

## Not applied / incomplete

| Area | Remaining work |
|---|---|
| Selection and clipboard | Shared Cut/Copy/Paste; device and Return/Main duplication; track/device deletion and device reordering; all-slots and multi-clip selections; general inversion; time editing beyond region duplication and block move/copy. Cmd+D now copies an Arrangement region (including gaps) and piano-roll ruler/marquee ranges with their full time span; individual-note selection still uses the note span. |
| Notes | Group/ungroup, splitting/joining/chopping, note tools and filters, further scale shortcuts/modes/persistence beyond F/G/K and the local fold/highlight buttons, complete modified-arrow navigation, velocity deviation/chance commands and numeric value entry. Shift+Cmd+U now opens Quantize Settings; full tool parameter/pattern parity remains UI-35b. |
| Audio and automation | Warp/transient keyboard selection/insertion, quantization, numeric parameter entry, envelope-wide commands and complete marker shortcuts. Existing mouse editing remains available. |
| Tracks and comping | Full selected-track folding/navigation in Arrangement, comp-take cycling/replacement and track movement beyond the UI-34 pointer workflow. Take-lane reorder buttons remain available. |
| Session | Scene insertion/capture/movement and follow-action keyboard workflows; add/remove stop buttons; group-scene clip aggregation/launch. |
| Playback and zoom | Separate insert-marker playback, move marker to playhead, follow scrolling, selection zoom/history and complete loop-brace keyboard editing. |
| Focus/accessibility | Tab-to-control and neighbor traversal, wrap policy, all clip-panel/mixer targets and full accessibility semantics. Text-field select-all/paste behavior is not unified. |
| Unimplemented views/backends | Tuning, grooves, video, Max, second window, global plug-in-window policy, MIDI export has no complete command workflow. |

Cmd+Opt+L remains a Sonora compatibility toggle for the combined detail area. Sonora's take-lane reorder buttons and
Assistant shortcut are extensions. Unsupported commands are disabled or safely consumed in their editor instead of
being redirected to unrelated edits. Modifier guards are improved but exhaustive physical-key delivery is not proven.

## Verification

RootWidget interaction tests exercise the added view/focus/global keys, Session navigation, note editing and parameter
focus. Context-command tests check selection/identity and disabled recording state; track-color history and ALS reload
are checked. Real Skia snapshots cover both group views. Native title-bar/pop-up behavior, physical Option/F-key
handling and focus after native sheets still need a manual check in the running app.

UI-31 adds H/W fit/restore with Arrangement focus (computer MIDI keyboard off), waveform-factor numeric
entry (Enter commits, Escape cancels, Cmd-click resets) and x/dB selection. Region Cmd+D is routed before whole-clip
and track duplication, including Edit > Duplicate. This does not implement Shift+Cmd+D / Duplicate Time across all
tracks, a shared clipboard, or automatic piano-roll loop expansion.

UI-32: after selecting an Arrangement time region across tracks, plain drag moves every clip intersection and gap
as one block; Option-drag copies it, Cmd bypasses snapping and Escape cancels. Invalid track/automation mappings reject
the complete drop. Each move/copy is one history entry. A title drag with no time selection still moves one clip.


UI-34 / UI-35: Shift-click in Arrangement extends from the first clip through both outer endpoints and intervening tracks.
Track-title drags reorder/group with Escape cancellation. F folds occupied notes, G folds to scale, K highlights scale
(with note-editor focus and the existing computer-keyboard routing rules). Shift+Cmd+U opens the Quantize panel;
Cmd+Enter applies the open Transform/Generate tool from detail focus or the Edit menu. Return retains its existing
Arrangement start-marker behavior outside that detail command. View also exposes Fold to Notes and Expression Editor.

UI-38: Cmd+J consolidates Arrangement ranges including gaps (single/multiple tracks);
Option+Shift+Cmd+F freezes/unfreezes the selected regular track and Cmd+B bounces it to a new muted Audio track.
Option-drag on the lower clip edge stretches audio through warp markers; Option body drag continues to copy.
Fade diamonds use vertical drags; enlarged left/right clip-edge targets expose resize cursors.


UI-40: Cmd+E splits Arrangement clips at the retained insertion cursor, or at both edges of a time
selection across tracks. The Edit menu and clip context Split use the same command. This follows the
[documented Arrangement Split command](https://www.ableton.com/en/live-manual/12/arrangement-view/#splitting-clips).
Solo S/menu actions are exclusive; Session and Arrangement buttons accept Cmd-click for additive solo.
The configurable Exclusive Solo preference remains unimplemented.

UI-41: Option+wheel over Arrangement lanes and Option `+` / `−` change track heights, including visible selected tracks.
The waveform footer level separately adjusts drawing amplitude. Control-bar meter and audio-clip BPM support
double-click text entry with Enter/Escape; BPM wheel and drag use 0.1 steps. These input paths are covered through
RootWidget dispatch; physical native Option-key/trackpad acceptance remains UI-16.

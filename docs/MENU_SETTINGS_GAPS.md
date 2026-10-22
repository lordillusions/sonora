# Menu and Settings gaps — 2026-10-15

Reference: Live **12.4.6 (2026-09-10)** on macOS. Every top-level menu, submenu, and Settings category was inspected through the native accessibility interface; Display & Input, Audio, Tempo & MIDI, and Record, Warp & Launch were also inspected visually. This report lists **remaining gaps only**. Implemented changes are recorded in UI_GUIDE.

Sonora shows unavailable commands disabled. A disabled command can also mean that the current selection is ineligible; the table below distinguishes features that are actually missing. Existing DSP or a similar clip-panel control does not imply that the corresponding menu workflow is complete.

## Menus: not applied

| Menu | Remaining functionality | Scope / reason |
|---|---|---|
| Sonora application menu | Live-style About content and complete Help/account integration. | Settings and normal macOS application commands exist. Vendor account/authorization is not applicable to Sonora. |
| File | Install Pack; Manage Files; Save Set as Template; Save Set as Default Set; video export. (Export MIDI Clip done 2026-10-17: Standard MIDI File, start marker to end.) | Pack/content management, template media/state ownership and MIDI export need explicit implementations. Audio export and opening installed content do not provide these workflows. |
| Edit: selection and clipboard | General Cut/Copy/Paste across tracks, clips, notes, devices and automation; invert selection outside the note editor; Select Loop; multi-clip operations beyond region move/copy; Clip Markers submenu actions. | Editors currently have different selection/clipboard contracts. The general menu clipboard remains disabled. Clip-panel marker editing works, but the menu commands need a defined insertion position and selection target. |
| Edit: grouping and metadata | Device grouping/ungrouping into racks; scene/device renaming; Edit Info Text; Compare A/B for devices. | Track grouping is implemented; unsupported note/device grouping no longer falls through to track grouping. Track/clip rename is implemented; the generic scene/device workflows are not. |
| Edit: time and rendering | Cut/Copy/Paste/Duplicate/Delete Time; Paste Bounced Audio; Fit to Time Range. | Freeze/Unfreeze, Bounce in Place/New Track and gap-preserving Consolidate now have rendering/history/save workflows (UI-38). Remaining commands need time insertion/clipboard semantics. Normal Cmd+D now duplicates selected Arrangement/piano-roll regions (UI-31); Duplicate Time still requires inserting time across all tracks. |
| Edit: launch and quantization | Remove/insert stop buttons; Extract Groove; input Record Quantization; Simplify Envelope. | Slot stop-presence is not modeled, groove authoring/engine behavior is incomplete, and recording quantization must be applied at capture boundaries. Quantize selected MIDI notes to the current grid works. |
| Create: project structure | Insert Return Track; Insert Scene; Capture and Insert Scene; Consolidate Time to New Scene. | Return insertion must update every send/routing/automation identity. Scene insertion/moves must preserve slot contents and unknown scene metadata. Empty take-lane insertion now works through the Create/context menus and Option+Shift+T. |
| Create: conversion and markers | Selection-based Create Fade; Add Time Signature Change. | Manual clip fades and curve diamonds work. Melody/Harmony/Drums conversion and slicing now create MIDI tracks with playable native instruments; detection uses independent approximate algorithms. Remaining commands require range semantics or a meter-map authoring/playback contract. Locator creation works; locator rename/delete/jump editing is still incomplete. |
| Playback | Play From Insert Marker; Move Insert Marker to Playhead; Follow; Start Transport With Record preference; Start Transport With Tap Tempo preference; Link Start Stop Sync; arm recording without starting; metronome only while recording. | A separate persistent insertion marker, follow-scroll policy and recording-armed state are needed. Main Record now targets Arrangement even while Session is displayed; Session Record remains separate. Continue Playback currently resumes the stored transport position. |
| View: windows and panels | Second Window; Info; Learn View; Pack Overview; File Manager; Undo History; Expand Clip View; automatic/horizontal/vertical clip-panel arrangements. | Requires new views/window ownership, an exposed history model or per-note-expression editing. The existing content editor and MIDI-controller envelope editor can be selected from View. |
| View: browser and media | Groove Pool; Tuning; Full-Height Browser; Show Similar Files; Show Filter View; global show/hide plug-in windows; Video Window; Max window. | Browser tags/similarity, microtuning, media/Max hosts and editor-window policies are incomplete or absent. Individual plug-in editors can already open. |
| View: mixer and Arrangement | Separate Track Options visibility; crossfader; per-track Performance Impact; Return Tracks visibility; separate Arrangement mixer section visibility; Overview visibility; scene tempo/signature controls. | Session Mixer/Sends/In-Out controls are implemented. Arrangement still has its own fixed mixer presentation. Parsed scene tempo is not a complete scene editor or display toggle. |
| View: folding and zoom | global Show/Hide Chains; Zoom to Time Selection and Zoom Back. | Fold to Scale works for all 35 scales of the set/clip scale (read and saved, UI-30b), retaining out-of-scale notes. Group fold/unfold is implemented, including revealing the selected group's ancestors. |
| Navigate | Groove Pool/Learn/Tuning/Mixer/Clip Panels targets; Previous/Next and neighboring-control traversal; browser history Back/Forward; Tab-to-focus; wrapped focus navigation. | Supported main views can receive focus. Comprehensive keyboard navigation requires a focus order and accessible subcontrols throughout the painted UI. |
| Options: synchronization and audio | Accessibility speech options; external MIDI sync; switchable Delay Compensation; Reduced Latency When Monitoring; time/fps ruler formats; Audio Engine On/Off. | PDC and solo-in-place already have engine behavior, but their reference preference/switching contracts are not implemented. Incoming clock synchronization and compensated monitoring need engine work. |
| Options: editing and MIDI | Snap Automation to Grid option independent of the editor grid; Step Input; Chase MIDI Notes; MIDI Envelope Auto-Reset; retune on loading tuning systems; Solo/Cue mode switches; Solo in Place option; Lock Envelopes; Max developer mode. | MIDI Note Editor Preview and Highlight Scales now have working switches. Scale controls currently cover local Major/Minor editor state; imported/set/clip scale persistence and the full mode list remain incomplete. Launch-quantization size, triplets and enable/disable controls are implemented. |
| Help | Learn View; Pack Overview; demo-set workflow; support/update service; integrated user forum/account/licensing. | The local Sonora guide is bundled and opens from Help. Vendor content/accounts are not reproduced; there is no updater service yet. |

Track context menus now follow the reference ordering and expose supported Group/Ungroup, Audio/MIDI Track insertion, track Rename and undoable track colors. Clip menus include Duplicate/Delete/Rename, activation and colors; note menus include Duplicate/Delete/Quantize/Select All and grid settings. Browser item and parameter reset/MIDI-map menus are present. Other context menus are not claimed to reproduce every reference command. Whole-track deletion (including returns) is implemented in UI-37. Multi-clip clipboard/editing, device/rack grouping and advanced browser/device context operations remain incomplete. Audio-file placement and new-track creation now form one undo step (UI-7b).

## Settings: not applied

The redesigned Settings sidebar currently contains Display & Input, Theme & Colors, Audio, Tempo & MIDI, File & Folder, Library, Plug-Ins, and Record, Warp & Launch. Unsupported Link and licensing/updater pages are not presented as functioning pages.

| Reference page | Remaining controls / behavior |
|---|---|
| Display & Input | Localization; focus-outline preference; scrollbar policies; Follow Behavior; Hide Labels; Tab/wrapped navigation preferences; moving clips with arrows preference; pen/tablet mode; permanent scrub-area preference; pitch-locked drawing; restoring suppressed warnings. Zoom and the existing computer keyboard are available. |
| Theme & Colors | System-follow appearance; tone and high-contrast selectors; adjustable grid intensity, brightness, color intensity/hue; automatic/default/reduced track colors; random-vs-track clip color policy. Built-in Light/Dark and `.ask` import are available; they do not claim pixel-identical fonts or every theme customization. |
| Audio | Reference-styled independent input-device selector and channel matrices: configuration currently delegates to JUCE's native device dialog. Default sample-rate/pitch conversion quality, Driver Error Compensation, Test Tone, tone volume/frequency and CPU simulator are missing. Output/sample-rate/buffer choices and actual latency readouts are available. Full cue and independent multi-output mixing remain engine work. |
| Link | Link peers, shared tempo/start-stop, Link Audio, latency and incoming-audio synchronization. No Link backend is implemented. |
| Tempo & MIDI | Tempo Follower; external hardware resync; six control-surface/script slots and preset dump; controller takeover modes; input Sync and per-port MPE settings; general MIDI output Track/Remote roles and simultaneous independent clock-output routing. Input Track/Remote roles and one selected clock output work. |
| File & Folder | Analysis-file policy and sound analysis; external sample-editor integration; configurable temporary recording folder; Max application selection; decoding-cache location/size/free-space controls and cleanup. Current actual folders/cache can be revealed. |
| Library | Collect Files on Export policy; downloadable-pack/cloud/Push/Splice services, visibility preferences and Splice download destination. User Library/Packs browsing locations are configurable and saved; that does not install Packs or authenticate online services. |
| Plug-Ins | Source enable/disable policies; VST3 custom folders; VST2 hosting; AU v2/v3 source selectors; auto-open/auto-hide/multiple-window policies. VST3 system-folder and AU scanning/retry work. Existing AU insertion/state gaps remain UI-7b; VST2 hosting remains outside the current format support. |
| Record, Warp & Launch | Selectable recording file type/bit depth; exclusive arm/solo; clip update rate; Session-automation recording policy; Start Transport With Record preference; short-sample loop/warp policy; automatic long-sample warp; default warp mode; default edge fades; default launch mode/per-clip quantization; Select on Launch/next scene; recording on scene launch; Start Transport With Tap Tempo preference. Count-in and current recording tools are exposed; global launch quantization changes the current set and is undoable/saved, rather than pretending to be an application default. |
| Licenses & Updates | No automatic updater or usage-data service. Vendor authorization is not applicable to this open-source application. |

## Context and keyboard follow-ups: not applied

- Track/clip/device/automation Cut/Copy/Paste, whole-track Delete, complete multi-clip/scene editing,
  device/rack grouping and metadata, browser file-management/tagging/preset menus, track default presets, Info Text,
  and advanced automation context actions remain incomplete. Disabled menu entries are deliberate.
- Whole-track Duplicate now works for Audio/MIDI/groups (including nested groups), through history and saving; selected
  roots must share a parent. Return/Main and device duplication remain unsupported. Track menus also support inline
  colors, assigning track colors to clips, selecting Arrangement content and inserting/showing take lanes. Freeze/Bounce
  now render audio (UI-38). Return insertion remains disabled.
- A Session group slot still stops its descendants; it does not yet aggregate/launch the contained clips in that scene.
  Deep nested-group framing still needs matched-scale reference measurements. Native title-bar/menu rendering and
  physical Option/F-key delivery need a rebuilt-app acceptance pass.
- Complete shortcut and focus limitations are listed in **[SHORTCUT_AUDIT.md](SHORTCUT_AUDIT.md)**. In particular,
  general clipboard/time-range workflows, comp-take cycling, note grouping and neighboring-control Tab traversal are
  not implemented. Unsupported comp cycling no longer reorders take lanes; use their reorder buttons.

## Verification boundary

The comparison targets the installed macOS version above; menu availability in Live changes with focus and selection. Sonora's remaining broad fidelity work is UI-13b; these menus/settings are not a claim of complete Live feature parity. Tests and renderer snapshots verify the rebuilt implementation; native interaction with every new menu/dialog and physical audio/MIDI devices still needs a disposable-project check.


## UI-34 / UI-35 remaining parity — 2026-10-16

Track reordering/group drops, Shift-click clip spans, Fold to Notes, Quantize Settings, Apply Transformation/Generator
and Expression Editor entry points are now implemented. Remaining MIDI-tool parity is tracked in UI-35b: full style/
pattern/parameter sets, automatic reversible previews, custom generator/time-warp curves, MPE transformations, Max MIDI
tools, full scale persistence/modes, full Pitch & Time utilities and Launch bank/program controls. Current native tools
are independent simplified algorithms. MPE is a working editor with self-save/history/MIDI-byte coverage; reference-app acceptance, matched expression layout, curve interpolation, channel/zone/range configuration, hardware and
native-instrument behavior still need qualification. No full feature or pixel parity is claimed.

## UI-38 media workflows — 2026-10-16

Freeze/Unfreeze, track/group Bounce in Place, Bounce to New Track, Cmd+J Consolidate including gaps,
Melody/Harmony/Drums to MIDI and slicing are implemented. Freeze retains original devices/clips in the model and
uses a Sonora-specific persisted render cache. Other DAWs ignore that cache and retain the original track.
Remaining qualification: hosted-plugin audible comparison, steady-state Session-loop effects, source-specific transcription quality and complete asynchronous
control/MCP job exposure. No identical transcription or pixel-parity claim.

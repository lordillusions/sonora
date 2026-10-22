# UI guide — Live 12 look & feel

Native device faces and the new compact Vector/Impulse/Echo instruments/effect are documented in
[`NATIVE_DEVICE_FACES.md`](NATIVE_DEVICE_FACES.md). This includes controls, sample drops, graph gestures,
Sonora-only state identities and the remaining visual/DSP parity boundaries.

## Principles
- The DAW is drawn with Skia on a single canvas. Agent Control is a bounded AppKit panel/input exception (ADR 0008).
- Live 12 look: **rounded panels on the `Desktop` colour**, 8 px outer margin, 6 px gaps, a dark 2 px frame around the
  focused main view; flat control-bar boxes without frames; framed light fields/dropdowns inside panels; small type.
- Colours **only** from `ui::Theme` tokens (names = Live `.ask` keys). The built-in defaults reproduce Live's
  "Default Light Cool High" (measured from screenshots); any installed `.ask` can be loaded. The only hard-coded colours
  are meter green/yellow/red.
- Selection: orange = active/on (`ChosenDefault`), blue = solo, red = arm/record, light blue = selected list rows.
- Track colour (palette index) tints title bars and clips; text on top uses `paint::contrastText`.
- Shared drawing helpers for Live-style controls live in `ui/Controls.h` (`ctl::dropdown`, `valueBox`, `toggle`, `barBox`,
  `caption`, `framed`, `triangle`).

## Layout (logical px at 100 %, `Theme::Metrics`)
| Area | Size | Widget |
|---|---|---|
| Control bar | 36 high (items 18 px, gap to panels included) | `TopBar` |
| Window margin / panel gap / corner radius | 8 / 6 / 4 | `MainLayout` |
| Browser | 400 wide (sidebar 160 + list) | `BrowserPanel` |
| Session: track column 95; Main 104; slot 18; header 24 | returns + Main anchored at the right edge | `SessionView` |
| Session mixer | stop row 19, I/O 115, Sends 55 (+15.5/extra send), Volume 120, colour strip 7, 2 px separators | `SessionView` |
| Arrangement: ruler 48 (overview 14), lane 87 (return 20, expanded group 87, folded group 44, collapsed 18), footer 18 | header 306 = title 103 + I/O 79 + mixer 106 + meters 14 (225 without I/O) **on the right** | `ArrangementView` |
| Device / clip view | 200 high by default; saved sizes retained | `DetailView` |
| Status bar | 32 high (rounded boxes) | `StatusBar` |

## Token usage cheat-sheet
| Token | Used for |
|---|---|
| `Desktop` | window background: panels sit on it, the control bar and status bar draw directly on it |
| `SurfaceArea` | browser, lanes, clip slots, detail background |
| `SurfaceBackground` | control bar, mixer strips, headers, ruler |
| `SurfaceHighlight` | selected strip/lane, device title bars |
| `DetailViewBackground` | device bodies |
| `ControlBackground` / `ControlForeground` | display boxes and their text |
| `ChosenDefault` | on-state, activator, playhead when stopped |
| `RangeDefault` | knob value arc |
| `RetroDisplay*` | EQ curve display |
| `LoopColor` | loop brace |
| `GridLineBase` | bar lines in lanes |

## Typography
Default: the macOS system font (San Francisco, "SF Pro Text") via CoreText, falling back to Helvetica Neue — it is the
closest installed match to Live's own "Ableton Sans" face, which we do not ship. For a closer look on a machine that has Live installed, you may point Sonora at a font
file at runtime: `SONORA_UI_FONT=/path/font.ttf SONORA_UI_FONT_BOLD=/path/bold.ttf` (local use only,
nothing is copied into the repo).
Default text sizes are 10 logical px (normal) and 9 px (small), with weight 600 for emphasized text. This reduces
the cap height and header width against a matched-scale reference capture; it remains a system-font approximation.
`ui_snapshot --font-report` prints the resolved family, weight, cap height, x-height and sample text advance.

## Themes
Options > Load Live Theme (.ask) imports any Live 12 theme, e.g.
`/Applications/Ableton Live 12 Suite.app/Contents/App-Resources/Themes/Default Dark Neutral Medium.ask`.
`ui_snapshot --theme <file.ask>` does the same for snapshots.

## Shortcuts (implemented)
| Key | Action |
|---|---|
| Tab | toggle Session / Arrangement |
| Shift+Tab / F12 | switch Clip / Device View |
| Opt+0..5 | focus control bar, Session, Arrangement, Clip, Devices, Browser |
| Cmd+Opt+3 / Cmd+Opt+4 | hide/show Clip / Device View |
| Cmd+Opt+M / Cmd+Opt+S | hide/show Session mixer / sends |
| Cmd++ / Cmd+- | global display zoom |
| Cmd+6 / 7 / 8 / 9 / 0 | launch quantization: sixteenth / eighth / quarter / bar / off |
| M / S / C | computer MIDI keyboard / selected-track solo / arm |
| Space | play / stop |
| Home | position to start |
| Up / Down | select previous / next track |
| Cmd+E (Arrangement) | split at the insertion cursor, or both edges of a time selection |
| Cmd+N | new project |
| Cmd+O | open set |
| Cmd+S / Shift+Cmd+S | save / save as |
| Cmd+M / Cmd+K | MIDI mapping / Session clip computer-key mapping |
| A (Arrangement) | show/hide automation lanes |
| Cmd+Opt+U (Arrangement) | show/hide selected track take lanes |
| T (take selected) | toggle audition of that lane |
| F9 / Record button | record armed tracks to Arrangement |
| Shift+Cmd+F9 | Session recording |
| Cmd+F | focus browser search; Escape returns keyboard focus to the views |
| Cmd+Alt+B | toggle browser |
| Cmd+Alt+L | toggle device view |
| Cmd+wheel / trackpad pinch (Arrangement, note editor) | horizontal zoom around the pointer (scroll up = in) |
| Shift+wheel (or Alt+wheel) | scroll in time; plain wheel scrolls tracks / pitches |
| + / - | zoom in / out time (around the play position) |
| Cmd+1 / Cmd+2 | Narrow / Widen Grid |
| Cmd+3 / Cmd+4 / Cmd+5 | Triplet Grid / Snap to Grid / Fixed Grid |
| Cmd or Alt while dragging | ignore the grid |
| Shift/Cmd+click a track name | select a range / toggle tracks (Cmd+G groups adjacent tracks, Shift+Cmd+G ungroups) |
| Cmd+L | Loop Selection: loop brace = selected notes (note editor), take range or selected Arrangement clip, loop on |
| Delete (Device View) | remove the device you last clicked (also right-click a device title > Delete, Edit > Delete Device); undo restores it |
| Shift+Cmd+M / Create menu | insert an empty one-bar MIDI clip on the selected MIDI track |
| Double-click knob / fader | reset to default |
| Shift+drag | fine adjust |

## Checking your work
```bash
./build/tools/ui_snapshot /tmp/a.png "<set.als>" --track 2
./build/tools/ui_snapshot /tmp/s.png "<set.als>" --session
```
Open the PNG and compare with Live. Screenshots of Live are kept locally and are not distributed (UI-13).

## Automation and audio clip views

Press A in Arrangement to show an automation lane under each track/return/main. The right-hand header has a device chooser (Mixer, numbered devices, MIDI Ctrl or unmapped targets)
and a separate parameter chooser. Current entries are ticked; a dot marks authored automation. Repeated device names
carry their chain position. Native processor names/ranges are used when available; unresolved targets retain their ID. Select a point and drag time/value; Alt disables
snap. Double-click empty space adds a point, double-click/right-click a point or Delete removes it. Each
completed gesture is one undo step. The audio Clip tab shows a stereo source waveform with the saved warp
markers; Cmd+wheel zooms around the cursor, wheel scrolls. With Warp on, double-click the marker strip to
add/remove a marker (at least two anchors remain), drag horizontally to change its source time, or
Shift-drag vertically to change its beat (Alt disables beat snapping). Neighbouring anchors cannot cross.

Double-click a Session clip body to open the shared Clip editor; its left launch strip controls playback.
Double-click an empty MIDI slot creates a clip when the set has a MIDI template, including Session-only
sets. The sidebar edits start and loop values by dragging up/down (Shift: fine), Loop on/off, and audio
Warp/mode/transpose. Start is shown as an absolute content position; MIDI/warped audio use beats,
unwarped audio seconds. Warp toggles preserve source positions while changing those units.

The MIDI sidebar offers local major/minor/root highlighting (not yet imported/saved as set scale enums).
Click Vel/Prob beside the bottom lane to edit velocity or note probability. Probability is saved; playback
uses a stable per-clip/note/loop decision, so split blocks and note-offs agree and seeking repeats the
same occurrence. This random-seed behavior has not been matched to Live.

Snapshot a Session clip with `ui_snapshot out.png <set.als> --session --clip TRACK --scene SCENE`.

## Recording input controls (ENG-9b)

Cmd+Opt+I shows/hides input controls in Arrangement headers and Session mixer strips. Click source to
cycle external input/resampling/no input, or MIDI All Ins/available ports/no input; click the channel row
to cycle mono/stereo physical channels or MIDI All Channels/1..16. These are cycling fields for now;
full popup choosers and output routing controls remain follow-up work. In/Auto/Off are direct buttons,
undoable and saved. Resampling forces Off. Missing MIDI ports display offline.

In monitors regardless of arm and suppresses clip output. Auto monitors armed tracks, inhibited over
playing clip intervals; replacement recording suppresses old clips. Off records without monitoring.
Input source/channel and arming cannot change during a take; monitor mode can. Audio monitoring/recording
opens required physical input channels after microphone permission; MIDI-only and resampling do not need
microphone access. Hardware capture has not yet been verified. Record again cancels a pending request.

The control-bar Count field cycles None/1/2/4 bars and persists as an application preference. When starting
Record from stopped, count-in holds the timeline position, monitors inputs, clicks even with the metronome
toggle off, and shows negative bars/beats in blue while the record indicator flashes. Starting Record during
playback skips count-in. Stop or Record cancels counting without creating a clip.

In/Out buttons beside the loop toggle enable Arrangement punch-in/out at the loop brace boundaries even
when Loop is off. Existing clips play outside the captured range; held input notes enter at punch-in and
close at punch-out. Transport keeps playing after punch-out. Session recording ignores Arrangement punch
flags. Punch/loop settings and their undo/redo are guarded during a take because its schedule is immutable.
The Loop button now uses a saved/undoable edit. Minimum-window transport layout was reviewed at 900×560.
Full popup metronome options and changing punch bounds during a take remain follow-ups.

`recording_probe source.als new-copy.als` creates a separate interoperability set with a two-second
Sonora-generated 660 Hz channel-2 mono recording on an empty Audio track. It rejects source overwrite and
existing destinations. Add `--punch` for beats 1→2 of our generated input. Reviewed snapshots: `screenshots/recording-input-{session,arrangement}.png`,
compared with Live's mono file display.

## Browser

Cmd+F searches the selected category/current folder. Add Folder stores a custom Place; disclosure arrows or double-click expand folders inline,
and the toolbar Up button changes the browser root to its parent. Selecting a sample auditions it when Preview is on; the footer toggle stops preview.
Drag samples onto Audio-track Arrangement lanes (grid snap; Alt for free placement) or Session slots.
Plug-ins are grouped in collapsible vendor folders with format icons; missing manufacturers appear under
Unknown Vendor. Native Instruments, Audio Effects and MIDI Effects have category folders. Click a folder
to expand it; search matches names, vendors and categories and reveals matching children. VST3 and AU
identifiers remain separate even when their display names match. The scanner cache is used without loading
plugins while browsing. Use Options > Scan Plug-ins (VST3 + Audio Units) to populate AU entries.
**Plugin icons and tags (PLG-10):** VST, VST3 and AU badges use a keyboard for instruments and signal controls for
effects; names no longer carry format suffixes. This display does not add VST2 hosting or AU insertion support.
Above the results, Creator, Device, Format, Type and User filters offer the available tag values. Values within a
filter group are alternatives (VST3 or AU); groups combine (Instrument and VST3). Active filter chips have removable
crosses. The search-row filter icon shows/hides these controls while retaining active filters. Search also finds tags.
The selected item's **Tags:** row sits above the audition footer. Click a tag to filter by it, or its cross to remove
that assignment. **Add...** (or the overflow ellipsis) offers existing tags, including removed defaults; **New Tag...**
opens an inline field (Enter adds, Escape cancels). Metadata seeds type/format/manufacturer/category tags. Custom tags
and removals persist in Sonora's settings, keyed separately for each plugin identifier, native device or file path;
they do not dirty a song. A matching selected item survives filtering; filtered-out selections and drags are cleared.
Drag native devices or scanned VST3 entries onto a track or the device panel; double-click inserts on the
selected track. Instruments/MIDI effects require MIDI tracks; MIDI effects insert at the start of the chain.
Escape cancels. AU insertion, sample→MIDI-track/Simpler, Finder drops and chain reorder are UI-7b.
Double-click a filtered set row opens that exact file. User Library defaults to `~/Music/Ableton/User Library`,
Packs to `/Users/Shared/Ableton/Factory Packs`; custom library/pack locations can be added as Places.

### Native instruments and modulation (DEV-13–18)

**Sonora Prism** is in Instruments / Synthesis. Its Oscillators, Filters, Envelopes, LFOs, Matrix,
Macros and Main tabs expose all 540 controls; the selector/arrows choose the oscillator/filter/envelope/
LFO or matrix slot. Choice fields open complete menus, arrows/wheel/keys move between choices, knobs
support Shift fine adjustment, and double-click resets to the exact native default. Envelope time knobs
include zero and give practical resolution near short times. Three previews show the generated base waves;
they do not display live modulation. Horizontal wheel gestures scroll a narrow device chain while vertical
gestures edit controls. All controls expose MIDI-learn targets. Values save/reload as `SonoraWavetable`,
an independent Sonora extension. Presets save as `.prism` files: **Save Preset…** and **Open Preset…** in the `...`
menu (UI-50); saved presets land in `~/Music/Sonora/Prism/Presets/User` and appear in the browser.
Reviewed panel: `screenshots/native-wavetable.png`.

**Floating editor (UI-27).** The 1080 px dashboard does not fit the Device View, so Sonora Prism is the one native
device that opens in its own window, like a hosted plug-in editor. Inserting it from the browser (onto a track or into a
rack chain) opens the window; loading a set opens none. In the chain it shows a 150 px stub: the title wrench and
**Show Editor** open or raise the window, **Dock** puts the dashboard back into the chain. The window has a **Dock**
button in its header; a docked device has a pop-out button at the right end of its title bar to float it again. Docking
selects the device's track (and rack chain) in the Device View. Closing the window behaves like closing a plug-in
window: the device keeps playing and stays floating. The window survives device edits, undo and redo (it rebinds to the
rebuilt processor) and closes when its device is deleted or another set is opened. Keys it does not use (Space, transport
and view shortcuts) are forwarded to the main window. The choice is per instance and **not saved** in the `.als`: it is
view state, like which plug-in windows are open, so saved sets stay unchanged and every reload starts floating. Other
native devices stay docked. `ui_snapshot` renders docked editors by default; `--floating-wavetables` shows the app's
floating stub and `--wavetable-window` renders the window for the selected track's first Wavetable.
Reviewed: `screenshots/wavetable-docked.png`, `wavetable-floating-stub.png`, `wavetable-window.png`.

**Preset navigation and browser (UI-28i).** The header reads `<<  <  [preset name]  ...  >  >>`. Banks and groups come
from the preset library folders (Prism's own library in `~/Music/Sonora/Prism`, then other installed libraries): the
first folder below `Presets` is the bank (Factory, User, Splice…), the
second the instrument group (Bass, Pad…); deeper folders (Bass/Reese) stay in their group. Once a library preset is
loaded (browser, `...` menu, or a set whose device name matches a library preset), `<` / `>` step through **that bank
only** and **wrap** from its last preset to its first (and back), like the built-in cycle and the table arrows; they never
cross into another bank. `<<` / `>>` jump to the first preset of the previous / next group of the same bank, also
wrapping; they are greyed and inert when no library preset is loaded or the bank has fewer than two groups. With a
built-in patch the arrows still cycle Init / Pulse Stack / Hollow Keys / Slow Motion. Clicking the preset name toggles
the **preset browser** over the dashboard (the keyboard footer stays playable): BANK list with counts (empty bank
folders shown with 0), GROUP list with All first, and NAME / FOLDER rows of the bank with the loaded preset in cyan. The
search field above them filters the bank's presets (every typed word must appear in the name or the folder below the
bank, case-insensitive) and is focused on open, so typing searches straight away; letters do not reach the computer
MIDI keyboard or shortcuts while the browser has focus (Skia-drawn field, same model as the main browser's search, not
the native text view of ADR 0008). A click loads the preset through the normal preset import and keeps the browser open
for auditioning; Up / Down step and load; Return or a double-click closes; Esc clears the search, then closes; Cmd+V
pastes; Cmd+Backspace clears. Loading rebuilds the editor, so the browser keeps its bank/group/search/scroll per
instance (view state, not saved). The `...` button keeps the old menu: built-ins, the library as nested menus, Open
Preset… and Save Preset…. `ui_snapshot --wavetable-browser [--wavetable-preset NAME] [--wavetable-search TEXT]` renders it.
Reviewed: `screenshots/wavetable-preset-browser.png`, `wavetable-preset-search.png`, `wavetable-preset-header.png`.

**Drag-to-modulate, keyboard and OSC panel (UI-28).** Each ENV/LFO tab and macro has a four-arrow handle. Drag it onto a
knob: modulatable knobs are outlined during the drag and the drop creates a matrix route (default depth about a third of
the knob's travel; dropping the same source again keeps the slot). Routes draw as coloured rings outside the blue value arc
(ENV cyan, LFO violet, macro amber; two rings shown, all listed in the menu). Drag a ring vertically to change its depth
(150 px = full travel, Shift fine); right-click a modulated knob to remove one route, all routes, or return to default.
Knobs show their label below and the value while hovered/dragged. The footer is a 34 px keyboard (C1–C7, velocity by press
depth) that plays the device directly, Pitch Bend (springs back) and Mod Wheel strips, and KEYS/ARP view buttons; the ARP
view shows the arpeggiator controls and lights the sounding arp note yellow. OSC panels read, in order: power, mode,
F1/F2 filter routing; table; OCT/SEM/FIN/CRS; 3D frame stack (current frame yellow); frame number, phase, random phase;
WT POS/UNISON/DETUNE/BLEND | PAN; WARP 1 knob + selector, selector + WARP 2 knob | LEVEL. An oscillator that is off is
dimmed except its power switch. Reviewed: `screenshots/wavetable-window.png`, `wavetable-dashboard-docked.png`,
`wavetable-mod-rings.png`, `wavetable-arp-view.png`. While a note sounds the editor animates (UI-28c): modulated knobs carry a
moving marker, filter curves and the yellow wavetable frame follow their modulation, ENV/LFO graphs show a playhead dot,
source handles fill with their current value (with a route-count badge), and a meter sits beside MAIN. The newest voice
drives the display. Reviewed: `screenshots/wavetable-live-playing.png`.

**Ranges, library, VELO/NOTE (UI-28d).** Hover a ring to read "Source→Destination: amount ↑/↓/↔" and the plain range it
sweeps; drag it to set the range (dragging through zero turns Forward into Backward). Right-click a ring for Forward,
Backward, Both Sides or Remove; double-click toggles Both Sides. Dropped sources start Forward (unipolar).
The table menu has a "Tables" submenu and the preset menu a "Presets" submenu, read from the library folders at
runtime; after loading from either, ◀ ▶ step through that library. The VOICE panel's VELO and NOTE
tabs edit the Velocity / Key Track curves (drag the end points vertically, the middle point freely; double-click resets)
and carry those sources' drag handles. Reviewed: `screenshots/wavetable-ring-readout.png`, `wavetable-velo-curve.png`. Envelope graphs are editable (UI-28e): drag the
attack end, hold end, decay/sustain point (both axes) and release end; the hollow handles mid-segment bend the attack,
decay and release curves. Sub, Noise and filters that are off are dimmed like oscillators. Imported presets show their
table names, their warp names (an asterisk marks warps Sonora does not implement yet) and a Unison stepper.
Reviewed: `screenshots/wavetable-envelope-edit.png`, `wavetable-import-big-analog-pad.png`. The LFO strip (UI-28f): FREE /
RETRIG / ENVELOPE beside the graph, BPM|HZ, a RATE knob that steps tempo divisions in BPM mode, RISE, PHASE, HOST and
10x. VOICE has a POLY stepper; the counter reads (sounding oscillator voices / POLY x voices per note). The
FX page is a rack of stacked module panels (power, name, knobs), the selected one outlined; ◀ ▶ scroll it.
Reviewed: `screenshots/wavetable-fx-rack.png`, `wavetable-import-alive.png`. FX panels are coloured per effect type (UI-28g):
▼ in the gutter hides a panel, the body shows the mode in large coloured text, a graph of the module and its knobs, and
MIX sits on the right; list rows have a colour bar, power, eye (hide) and ✕ (delete). The LFO type menu (above FREE)
offers Chaos: Lorenz / Rossler and Random S&H; attractors are drawn with the live position. The Noise menu adds White,
Pink, Brown and Geiger. Reviewed: `screenshots/wavetable-chaos-lfo.png`. Panels are compact single rows (UI-28h) showing each
module's main controls; ••• on a panel opens all of its controls in CONTROLS.

Sonora FM exposes four oscillators and eight algorithms. Page switches show all oscillator ratio, fine,
level, wave and ADSR controls. Its DSP and stored `SonoraFm` parameters are independent of Operator;
imported Operator devices still use the existing unsupported-device fallback. FM antialiasing, filtering
and additional modulation are DEV-13b.

Sonora Sampler uses `MultiSampler` zones with the shared sampling engine. Drop samples onto the zone panel
to add them, or onto a row to replace its sample. Drag/wheel Root, Key Lo/Hi, Vel Lo/Hi and Loop values;
right-click a row to remove it. Loop modes are Off, Forward and Ping-pong. Pages expose the amp/filter
controls. This is the sampling foundation, not the complete Sampler feature set (DEV-14b).

Sonora Drum Rack shows 16 pads; wheel the pads to change the displayed bank. Drop a sample/native device
onto a pad to create its chain. Drop onto a chain row to append to it, or into empty space to create a chain.
Selecting a populated pad/chain exposes its child devices to the right; horizontally scroll the device area
to reach them. Instrument Rack chains expose key, velocity and selector ranges. Drum chains expose receiving
note, sending note, choke group, gain, pan and on/off. Drag/wheel numeric cells; right-click a chain to remove.
The + Chain header adds an empty chain. New racks expose 16 macros on control pages; the chain selector is
also a control. Choose Map: M1…M16 in the chain header, then right-click a child native knob to map its full
range. Editable mapping ranges/unmapping and hosted-plugin insertion are DEV-15b. Structural changes use
one undo step and reload only that rack's processor; they are blocked while recording.

Sonora LFO shapes audio volume and pan. Draw in its graph to switch to Custom and edit 17 linear points.
Sync uses musical divisions (including dotted/triplet rates); Free Rate uses Hz. Trigger selects Transport,
Free, Note Loop or Note One-shot. One-shot returns to unity after one cycle; notes are left available to
downstream devices. Volume controls modulation depth, Stereo offsets the right envelope, Pan Mod controls
pan movement, and Smooth/Dry-Wet control the result. Filter/arbitrary-parameter modulation and MIDI CC output
remain DEV-16b. Sonora Velocity adds a curve or fixed value to note-on velocities in MIDI Effects.

**Sonora Curve LFO** (Modulation) is the full multi-graph LFO. Header row of the graph editor: `1`–`12` select the
graph being edited (a dot marks graphs that currently drive a destination); **Shape** applies the next generated
shape (Shift: previous); **x1x** cycles the selected graph's warp (speed multiplier, Shift: back); **Copy/Paste**
duplicate a graph. In the plot: double-click adds a point or removes the one under the cursor (Cmd-click also
removes); drag points (end points only move vertically); drag the hollow handle on a slanted segment to bend it
(Alt bends every segment); Alt snaps to the grid; Shift-click adds a flat step at the grid size. Knob pages: 1 = timing
(Rate, Free Rate, Swing, Phase, PWM, Smooth, Offset, Depth) and Volume/Pan routing; 2 = filter and Cut/Res/Var
routing, CC Out; 3 = MIDI. Each destination picks a graph (1–12) and an amount; volume 1 = unchanged, 0 = silent,
and cutoff/resonance/var/pan move around the graph's midpoint. Sync/Anchor/Dotted/Triplet buttons set timing: Anchor
locks the phase to the song position (swing needs Sync + Anchor); Dotted/Triplet enable those rate variants. MIDI into the
device: notes 0–59 choose the source graph for Cut/Res/Vol/Pan/Var (12 keys each) unless Vel>PWM, Note>Rate or Note>Cutoff
is on; notes ≥60 drive Note Retrig (restart; Env stops at the end of the cycle), Note Gate (LFO acts only while a note is
held), Vel>PWM, Note>Rate and Note>Cutoff. **CC Out** (1–128 = CC 0–127) sends the cutoff curve, scaled by CC Depth, to
the next device or track. Edits to graphs are direct (no undo steps yet; DEV-16c).

`SonoraFm`, `SonoraLfo`, `SonoraCurveLfo`, `SonoraWavetable` and `SonoraVelocity` are Sonora extensions in `.als` files; their state reloads in
Sonora, but these tags are not recognized by other hosts. Newly authored Sampler/rack XML has save/reload
coverage in Sonora; reference-app acceptance is still unverified. Functional references:
[instruments](https://www.ableton.com/en/manual/live-instrument-reference/),
[racks](https://www.ableton.com/en/manual/instrument-drum-and-effect-racks/),
[LFOTool](https://www.xferrecords.com/products/lfo-tool). No proprietary code, presets or assets are included.

Reviewed snapshots: `screenshots/plugins-by-vendor.png` and `screenshots/native-{fm,sampler,drum-rack,instrument-rack,lfo}.png`.
`ui_snapshot --browser-category 2|3|4|5|6 --browser-query TEXT` selects/searches a category; `--rack-chain N`
selects the chain shown in the device area. Plugin browsing reads the real cache without scanning/instantiating.

## Pixel comparison (UI-13)

Frame both images to the same pixel dimensions (use `ui_snapshot --size --scale --zoom`), then run:

```bash
python3 scripts/compare_ui.py reference.png candidate.png --out /tmp/ui-comparison --tolerance 8 --max-mismatch 0.05
```

Requires Pillow. The tool writes `comparison.png`, `difference.png`, and `metrics.json`; exit 1 means the
mismatching fraction exceeded the threshold. It refuses mismatched frame sizes rather than silently
resizing. Optional `--reference-crop L,T,R,B` and `--candidate-crop L,T,R,B` remove window chrome. Actual
comparisons convert embedded ICC profiles to sRGB. The checked Session grid baseline is
13.440% mismatch at tolerance 8 after tuning (previously 93.416%).
It covers only four track headers and seven scenes; broader/pixel-fidelity work remains UI-13b.

## Palette audit (UI-12)

```bash
python3 scripts/verify_palette.py <live-palette.jpg> --names <live-palette-names.json> --out docs/screenshots/palette-verification.json
```

All 70 swatches match within tolerance 8 after converting the monitor ICC profile to sRGB. Swatches are
median-sampled from 9×9 interiors to limit JPEG noise. This verifies indexed ordering and a capture-tolerance
match; it does not establish exact RGB values. No reference fonts, themes or audio assets are distributed.

Count-in/punch snapshots: `screenshots/count-in-punch.png`, `count-in-punch-small.png`, actual-window
`native-count-in.jpg`, and `reference/live-punch-recording.jpg`. `ui_snapshot --count-in N` frames the preference.

## Continuous Arrangement takes (ENG-9c)

With Loop on, recording continues across wraps until Stop/Record, seek, capacity failure or device stop.
Audio uses one WAV per track; each pass remains in a take lane, including the final partial pass. The
main Arrangement plays the most recently recorded sections and keeps the previous remainder. A single
undo removes the whole recording group; save/undo/redo preserve its source identities and sample refs.
Single-pass Arrangement recording retains a lane too. Recorded audio defaults Warp On/Beats; stretch
quality has not been calibrated against Live. Session quantised capture and ongoing MIDI note/controller overdub playback are available (ENG-9d/e/f).

Take-lane display, audition and manual comp selection are implemented in UI-15; refinements remain UI-15b. Reviewed `screenshots/loop-recording.png`
and `reference/live-native-loop-final.jpg`. Prepared limits: 4096 passes, 65536 tempo-map entries and
65536 MIDI notes; overflow ends the take with a warning. Disk rings remain bounded too.

## Take lanes and comping (UI-15)

Select a track and press Cmd+Opt+U, or View > Take Lanes. Showing lanes exits automation; A shows
automation in their place. Lane names/heights come from the set. T or the header T button auditions
a lane instead of the main comp while playback runs. Only one lane per track auditions; toggle it
off to return to main. Session launcher state survives temporary take audition.

Drag a take clip body to select a timeline range, then Enter to copy it into main as one undo step.
Alt disables snapping. Only source intersections are copied; empty gaps retain existing main clips.
Orange strips link main comp sections to their source TakeIds. Drag a clip title or edge to move/trim/
resize the take itself; double-click it to open the shared note/sample editor. Lane edits remain
independent of copies already in main.

Click a lane name to select the lane. Cmd+R or double-click starts rename; type to replace the initially
selected name, Cmd+A selects all and Cmd+V pastes. Enter commits, Escape cancels. Cmd+D duplicates
the selected lane, Delete removes it, and its arrow buttons reorder it. Cmd+Up/Down is reserved for comp-take cycling (not implemented). With a clip
selected, Delete/Cmd+D instead apply to that clip. Deleting a source leaves its main copies intact.

Visibility is local; imported folding state, resizing/inserting lanes, draw-to-comp, crossfade boundaries,
range-only audition and detailed pixel parity are UI-15b. Snapshot with `--takes TRACK`, or select the
first clip of a stable lane ID using `--clip TRACK --take-lane ID`.

## Session recording (ENG-9d)

Arm the intended track, choose its input and monitoring, then click the circle in an empty Session slot.
During playback, start and finish follow global launch quantisation. Starting while stopped begins after
any selected count-in; click the recording circle again to finish and loop the recorded clip. Shift-click
an existing MIDI clip's launch button to overdub notes while hearing its existing contents. Global Record
in Session targets armed tracks' playing MIDI clips or selected/empty scenes. Audio records into empty slots.
Start another armed track's slot while the first records. Clicking a recording circle again finishes only
that track; its result saves while the other tracks continue. Track Stop affects that track and leaves its
result stopped; global Record/Stop All finish all active captures. Queued/Recording/Stopping/Saving labels
are independent per track. Each finished batch is one Cmd+Z take. MIDI recording still needs an existing
MIDI clip template, and newly recorded empty slots start looping after worker/model publication rather
than immediately at the stop boundary (ENG-9d). Additional hardware inputs require finishing capture
before audio-device reconfiguration.

Playback > Stop All Clips ends Session recording and leaves the resulting clips stopped while the transport
continues. Playback also provides Play, Stop and Toggle Recording. The painted Stop All row passed headless
input tests, but native coordinate delivery in that region remains unresolved (UI-16); use the native menu.

Completed notes and touched controller patterns repeat on later passes during existing-MIDI overdub.
Preview playback continues through worker drain into the final clip without restarting voices. The visible
clip model updates after drain; in-flight visuals and empty-slot handover remain ENG-9d. Independent
per-track start/stop is ENG-9g. Native Sonora recording gestures remain unverified after a ScreenCaptureKit
failure; callback tests and own two-/three-beat saved loops opened in Live provide separate evidence.
Do not edit the recording target or switch scenes until the take finishes.

For phase layout review, repeat `ui_snapshot --recording TRACK:SCENE:PHASE` (zero-based track/scene;
phase 1 queued, 2 recording, 3 stopping, 4 saving). This simulates state in real widgets. Reviewed
`screenshots/session-recording-phases.png` and saved output `session-independent-saved.png`.

## MIDI clip envelopes (UI-5b)

Open a MIDI clip's Clip tab and choose Envelopes beside Notes. The same editor addresses Arrangement,
Session and take clips. Click the MIDI Ctrl chooser; scroll the list or type a controller name/number,
use Up/Down and Enter to select, or Escape to close. It offers pitch bend (-8192..8191), channel pressure
and all 128 CCs (0..127); imported Float values and duplicate-time steps remain intact.

Double-click empty space to add a breakpoint. Click/Shift-click points to select, drag time/value,
Alt disables snapping and Shift gives fine value movement. Points may meet to form jumps but cannot
cross unselected neighbours. Cmd+A selects non-preroll points. Delete, right-click or double-click a point
removes it; Clear removes only the chosen envelope. B or Draw toggles grid-cell drawing: adjacent cells
hold values, boundaries remain paired Float jumps, and original values outside the painted interval are
restored. One completed gesture is one undo. Escape or losing focus cancels an unfinished gesture.
Cmd+wheel zooms around the pointer; wheel pans time. Fresh lanes retain a beat-zero default anchor.

Notes remain dim context behind the envelope and are never changed by these gestures. Target allocation
is part of the edit, after recording guards; undo removes an originally absent descriptor table and redo
keeps its IDs. The panel currently edits linked MIDI Ctrl lanes. Device/mixer clip envelopes and range
copy/paste/ramp tools remain UI-5c; independent envelope loops/curves remain ENG-11b.

Snapshots: `ui_snapshot out.png <set.als> --clip TRACK --scene SCENE --envelopes --controller INDEX`, where
INDEX is semantic (0 bend, 1 pressure, CC n + 2). Omit `--scene` for Arrangement, or use `--take-lane ID`.
Reviewed `screenshots/clip-envelope-{sustain,bend}.png`; exact native step output opens in Live under CC64.
Native double-click delivery now dispatches once through mouseDown; JUCE's follow-up notification is ignored.

## Projects and saving (PRJ-1)

File menu: **New Set** (Cmd+N), **Open Set** (Cmd+O), **Open Recent Set**, **Save Set** (Cmd+S), **Save Set As** (Shift+Cmd+S), **Save a Copy**,
**Collect All and Save**. A normal launch starts a new Untitled set; an explicit `.als` launch/Finder open takes priority.
Recent history keeps up to 30 successfully opened/saved sets, excludes temporary Untitled projects, disambiguates folders,
and offers Remove Unavailable Sets / Clear List. Closing the window (Cmd+W) uses the existing save-changes prompt.
New Set opens an empty set (2 MIDI, 2 audio, 2 returns, 8 scenes, 120 BPM) in a scratch
project folder; the window title reads "Untitled". The first Save opens "Save New Project" (default
`~/Music/Sonora Projects`), creates `<Name> Project/<Name>.als` and copies the project's `Samples/**` (recordings) into it,
keeping references project-relative. Save As on an existing set writes exactly where you choose and also carries
`Samples/**` along. Collect All and Save additionally copies every sample referenced from elsewhere into
`<Project>/Samples/Imported`, rewrites the references as project-relative paths and reports copied/missing counts in
the status bar (Live Pack files stay where they are). New, Open and Quit ask to save unsaved changes first.
Save completes the pending action only after a successful write, including the first-save destination chooser.
Don't Save continues immediately; Cancel, a cancelled chooser or a failed save keeps the document open (PRJ-3).
The native prompt explicitly uses zero-based button indices, independent of the custom AlertWindow convention.
`als_dump --new-set <out.als>` writes the generated empty set; `--new-set-demo` adds authored MIDI clips (interop probes).

## Menus and Settings (UI-25 / PRJ-2)

The menu bar follows File, Edit, Create, Playback, View, Navigate, Options and Help. Unsupported commands are disabled;
the complete remaining-function report is [MENU_SETTINGS_GAPS.md](MENU_SETTINGS_GAPS.md), based on the installed macOS
Live 12.4.6. Context can also disable an otherwise supported command.

Create > Insert Return Track (Option+Cmd+T) appends a return (at most 12, see ALS_FORMAT) and selects it. Edit > Deactivate Track /
Hide Track (also in the track menu) act on the selected tracks; View > Show Hidden Tracks (N) lists hidden ones again with a
"(hidden)" prefix, deactivated headers are grayed. Edit > Strip Silence / clip menu Strip Silence splits an Arrangement audio clip
around its audible passages; Option+Left/Right moves the Arrangement insertion cursor between transients. Create > Capture Audio
(and the Capture button when no MIDI is waiting) keeps the latest passage on the armed audio inputs. Clip menu Duplicate as
Linked / Unlink Clip; linked clips show a chain glyph before their name (TRK-1..5).
Settings > Keyboard Shortcuts (REQ-3a) lists every menu-bar command: click its key, press the new shortcut (Delete clears,
Esc cancels), Reset/Reset All restore defaults, typing filters. The menus show rebound keys; a default you move away stops
working; keys that only work inside one view (piano roll arrows, H/W, ...) are not editable yet (REQ-3b).

Create > Insert Audio Track (Cmd+T) / Insert MIDI Track (Shift+Cmd+T) inserts after the selected track, retaining its parent
group. Selecting a folded group inserts inside it and unfolds it in the same undo step. Create > Import Audio File uses
the browser sample-import path, creating an audio track when needed; that track and the subsequent clip are separate
history entries. Insert Empty MIDI Clip also works in an empty selected Session slot. Rename (Cmd+R) targets the selected
clip or track. Add Locator uses the current transport position. Track insertion, names, locators and global launch
quantization are undoable and saved. Locator IDs remain unique when imported IDs are not ordinal.

Session title clicks select tracks with a blue header: Shift selects a range and Cmd adds/removes tracks. Modifier-clicking
slot bodies can select tracks too; launch-button clicks retain their recording/launch behavior. Header clicks clear stale
clip selection and show devices. Cmd+G / Shift+Cmd+G and the track context menu use this selection. Clip/scene focus is
separate from the header-selection highlight. New documents clear the previous document's track-selection indices.

Settings opens from Sonora > Settings, Options > Settings, or Cmd+comma. Its eight-category sidebar has Display & Input,
Theme & Colors, Audio, Tempo & MIDI, File & Folder, Library, Plug-Ins, and Record, Warp & Launch. Selected categories,
aligned labels/value fields, section rules, clipped content and bounded scrolling follow the reference layout. Escape,
Cmd+comma or the close button closes it. Outside clicks stay modal; panel contents cannot trigger hidden underlying UI.
The main-view focus frame and browser-drag label are suppressed over modal Settings/export panels.

- Display: zoom 80–200%, reset to 100%, existing Computer MIDI Keyboard toggle and octave/velocity readouts.
- Theme: built-in Light/Dark and custom `.ask` import; changing themes retains dragged panel sizes. Built-in colors are
  Sonora approximations. Themes and zoom are saved; vendor fonts/assets are not bundled.
- Audio: current driver/input/output, output selector, actual available sample rates/buffer sizes and latency readouts.
  Input/Output Config opens the native JUCE device/channel dialog; input access requests normal microphone permission.
  It supports input/output channel selection instead of the previous output-only dialog. Device configuration is guarded
  against active recording/export/device edits; the dialog is closed before the engine is destroyed.
- Tempo & MIDI: per-input Track/Remote toggles and one output port's clock Sync. MIDI roles/clock retain their existing storage.
- File & Folder: reveal actual temporary-project, settings and plug-in-cache locations.
- Library: choose and persist User Library/Packs browsing folders; reveal Places and add folders to the Browser.
- Plug-Ins: system-folder locations, VST3/AU rescan, available count and failed-plugin retry, disabled while scanning.
- Record, Warp & Launch: persistent count-in; existing Arrangement overdub and Automation Arm; per-set global launch
  quantization, saved/undoable; Browser File Preview shares the Browser/footer/Options state and is persisted.

Navigate focuses supported views. View includes content/MIDI-controller envelope selection, Session mixer/sends/In-Out,
group fold/reveal and zoom controls. Playback exposes Session Record, metronome, Back to Arrangement and Stop All;
the main Record/F9 targets Arrangement regardless of the displayed view. Help opens this bundled guide. The reference's
remaining preferences, editors and engine behaviors are listed in the gap report rather than represented as working switches.

## Audio clips (UI-17)

Select an audio clip (Arrangement/Session) and open the Clip tab. Left: the **Clip**, **Audio** and **Launch** panel
(`ui/views/ClipControls`, shared with MIDI clips); right: the sample editor. Numeric fields drag vertically (Shift = fine); menus open native popups; double-click Gain or Pitch to reset.
- **Warp** toggles warping (positions switch between seconds and bars.beats.sixteenths without moving), **Mode** picks the warp
  mode. Beats: **Preserve** (Transients or a fixed grid), transient loop mode and envelope; Tones/Texture: grain size / flux.
  **BPM** shows the tempo described by the warp markers; type-drag it or use **/2** and **x2**.
- **Reverse** writes a reversed copy of the sample to `<project>/Samples/Processed/Reverse` and points the clip at it
  (markers and loop are mirrored; Cmd+Z restores the original file).
- Sample editor: Cmd+wheel zooms to individual samples (drawn as a line with dots), wheel scrolls; drag the triangles above
  the waveform to move the clip start/end; with Warp on, double-click adds a warp marker (snaps to detected transients),
  drag moves it (Shift: beat); grey hairlines are the detected transients.
- **Crop Sample** (sample editor right-click) writes the part of the file the clip uses to
  `<project>/Samples/Processed/Crop/<name> [cropped].wav` and re-points the clip (markers shift so the same audio plays; undoable).
- Right-click in the sample editor: Insert/Delete Warp Marker, Set 1.1.1 Here, Reset Warp Markers, Crop Sample, Zoom to Fit.
  Alt+Cmd+wheel stretches the waveform vertically. Live's transient markers are detected, not user-editable, so they only snap
  warp-marker insertion.
- **Complex Pro** shows Formants and Envelope (stored in the set, the engine does not apply them yet).

## MIDI clips and the Launch section

The same panel serves MIDI clips: **Clip** (start/end/loop/position/length, editable **Signature**), **Notes** (Rev, Inv,
Legato, Dupl.Loop, x2, /2 = `ui/MidiClipEdits`, pure and unit-tested; x2 and /2 also scale the loop) and **Launch**.
Launch (audio and MIDI): **Mode** Trigger / Gate (stops when the mouse is released) / Toggle (launching a playing clip stops
it) / **Repeat** (hold its Session launch button to retrigger at the clip's quantisation rate; releasing lets the last pass
continue), **Quantization** (Global or the clip's own), **Legato** (a legato clip continues at the position of the clip it
replaces) and **Velocity** (MIDI launch velocity scales audio gain or the clip's note velocities). With quantisation None, Repeat launches once. Scene launches
also honour individual clip quantisation, so clips in one row may start at different boundaries; empty slots stop at the
global boundary and cancel previously queued launches. Track/transport stop, launching another clip or scene, and Back to
Arrangement cancel held repeats. Starting a recording or auditioning a take also cancels holding. Follow actions are not
implemented. Repeat timing across
Arrangement wraps is tested in Sonora; native Live timing and the clip quantisation enum mapping still need verification.
Reviewed snapshot: `screenshots/session-repeat.png` (Session grid/mixer and the Clip/Notes/Launch panel).

Choose **Options > MIDI Map Mode** (Cmd+M), click a Session clip, then press a controller note or momentary CC button.
The learning press assigns the mapping without launching. Leave mapping mode with Cmd+M or Escape; subsequent presses
launch and note release drives Gate/Repeat. CC buttons use down/up edges (64..127 / 0..63) at full launch velocity.
Mapped down/up messages are consumed. Removing a held mapping or disabling Remote releases it safely.

Choose **Options > Key Map Mode** (Cmd+K), click a Session clip, then press an unmodified letter or digit. Leave with
Cmd+K or Escape. The assigned key launches in Session view at full velocity; release drives Gate/Repeat and OS key repeat
is ignored. Text entry and modifier shortcuts keep priority. Focus loss, switching views, opening Preferences or another
document releases held keys. Right-click or Delete in either map mode removes that kind of assignment; MIDI and computer
keys can coexist on a clip. Assignments save automatically in the set's `.sonora.json` sidecar and follow the clip during
edits/undo. A replacement clip in the same slot is not targeted. New unsaved clips become persistent after saving the set.

Velocity amount zero ignores launch velocity; at 100%, MIDI velocity 1 is silent and 127 retains the original sample gain
or note velocity. Intermediate amounts blend linearly. MIDI controllers, note release velocity and monitored live input are
not scaled. Amount is captured at launch; relaunch to hear a changed amount. This response curve and native/hardware
delivery still need comparison with Live. Reviewed mapping previews:
`screenshots/session-key-map.png` and `screenshots/session-midi-map.png` (`ui_snapshot --keymap` / `--midimap` with
`--clip TRACK --scene SCENE` seeds an illustrative assignment).

## Resizing

Everything Live lets you resize can be dragged (the pointer changes at each edge); sizes persist with the app layout:
browser width and its sidebar/list split, the clip/sample/monitor editor height, the Clip panel width, the piano-roll velocity lane, the
Session mixer height (top edge of the mixer area) and individual track-column widths (right edge of a header), and in
Arrangement the track-header width, per-lane height (bottom edge of a header; double-click collapses). `ui/widgets/Splitter.h`
is the reusable divider; per-view edge zones use `cursorAt`. Minimum/maximum sizes are clamped in `MainLayout`.

## Grid, ruler and zoom (UI-19)

Modelled on Live 12 (measured in Live: adaptive grid, ruler labels and the corner indicator).
- **Adaptive grid** (default): the grid is the smallest size whose lines are at least 10 px apart (Narrowest 3 px, Narrow 6 px, Wide 20 px, Widest 40 px),
  so zooming in goes 1 bar, 1/2, 1/4 ... 1/64. **Fixed Grid** keeps one size (8 Bars ... 1/64), **Triplet Grid** uses 2/3 of
  every sub-bar size, **Snap to Grid** can be switched off, and holding Cmd or Alt while dragging ignores the grid.
  Options menu and right-click (Arrangement, note editor) offer Narrow/Widen/Triplet/Snap/Fixed, the density and sizes.
  The Arrangement and the clip editors (notes, envelopes) keep separate settings (`UiContext::arrangementGrid/clipGrid`), saved
  with the layout. The current size is shown in the lower right corner ("1/8", "1/16T", "4 Bars"). Automation points use the
  Arrangement grid. (Live also stores the clip editor grid per clip in `<Grid>`; Sonora does not read or write that yet.)
- **Ruler** (`ui/GridModel`): bar numbers, then beats (1.2, 1.3, 1.4) and finer positions (1.1.3 ...) appear as the zoom allows
  (the finest level whose label fits), with unlabelled ticks down to 1/64. Lane/note grid lines follow the grid size with
  bar > beat > sub-beat strength; tiles alternate per beat when zoomed in, per bar or several bars when out.
- **Scrub area**: clicking the number strip of the Arrangement ruler plays from that (grid-snapped) position and dragging
  moves it; a click on empty lane space moves the insert marker (play start) without playing. The loop brace below drags as before.
- **Note editor scrolling**: the wheel / two-finger scroll moves continuously over all 128 pitches (pitch 0 can sit at the
  bottom edge, 127 at the top), transposing or nudging keeps the selection in view and dragging notes near an edge scrolls.
  Alt+wheel changes the row height around the pointer.
- **Insert MIDI clip**: double-click an empty MIDI lane (one bar at the grid position left of the click, never over the next
  clip), right-click > Insert MIDI Clip, Shift+Cmd+M or Create > Insert Empty MIDI Clip.

## Arrangement and MIDI workflow refinements (UI-30)

- Drag empty Arrangement lane space to select time across tracks; Shift+drag inside an existing clip also selects time.
  Cmd+L loops that range. Return seeks to the enabled Arrangement loop's beginning (or selection start when loop is off),
  without starting playback; Space starts there. Take-range Return retains its comp action. The ruler brace has a dark
  outline and inward end handles; its edges/body retain resize/move gestures. Creating a MIDI clip from the menu uses an
  active time selection's position/length, otherwise one bar at the play position.
- Opening a different MIDI clip, or explicitly double-clicking it again, fits its content horizontally. Arrangement
  clips use their visible duration and content offset; Session loops use their loop length. A one-bar clip fills the
  available note area; manual zoom stays until another explicit open. The adaptive grid follows the fitted zoom.
- Click/hold the piano keys to audition the selected track; drag between keys to glide. Release outside the keyboard,
  focus loss, document/control-bank changes and disabling Options > MIDI Note Editor Preview release the note.
  Preview works with arm off and monitoring off and does not enter the recording input or Capture MIDI path.
- The control bar has root and Major/Minor scale choosers and a highlight switch, sharing the Clip panel's existing
  local scale state. `K` in the note editor toggles highlighting (computer MIDI typing retains priority). The **Scale**
  button above the keys, also View > Fold to Scale, hides unused non-scale pitches while keeping authored out-of-scale
  notes. These are local view settings: full scale modes, imported/per-clip/set scale persistence and scale-aware device
  behavior remain UI-10d/UI-30b.
- Arrangement/Session use horizontal/vertical-bar icon buttons at the far right, with delayed **Arrangement View** and
  **Session View** tooltips. Tab retains its existing view toggle. Selected Arrangement group titles are cyan; that frame
  continues through member rails. Group lanes have a lighter background, and members keep their own colors.
- A selected track header supports **Cmd+D**, Edit > Duplicate and right-click > Duplicate: complete devices/state,
  mixer, Arrangement/Session clips, take lanes and automation are copied. Groups include nested descendants and rebind
  internal parent/routing/automation identities. Multiple selected roots must share a parent. This is one undo command,
  including undo/redo after saving. The engine graph rebuilds, as for grouping; processor editors close and voices may
  restart. Return/Main duplication and device duplication are not implemented.
- Context menus use compact flat rows, aligned shortcut glyphs and the light field background. Track menus include the
  inline 14×5 color grid, **Assign Track Color to Clips**, **Select Track Content**, **Insert Take Lane** and **Show Take Lanes**,
  alongside existing Audio/MIDI track insertion. Empty take lanes also use Create > Insert Take Lane or Option+Shift+T;
  insertion is undoable and saved. Freeze/Bounce, Return insertion, track Cut/Copy, linked tracks,
  Info Text and default-track presets remain disabled. `ui_snapshot --track-menu` renders the actual menu items and
  JUCE drawing code; it is a preview, not native popup interaction. `--fold-scale` enables folding in editor snapshots.

## Track groups (UI-14b)

Shift+click track names (Arrangement headers or Session titles) to select a range, Cmd+click to toggle, then Cmd+G (Edit menu: Group
Tracks) wraps adjacent tracks of one parent in a new group track; Shift+Cmd+G ungroups the selected group (children move up).
Members that played into Main now play into the group; groups can nest. Saved as a real `<GroupTrack>` (children follow it,
`TrackGroupId`, output "AudioOut/GroupTrack", one `GroupTrackSlot` per scene); a generated probe opens in Live 12.4.6
(`als_dump --new-set-group out.als`). Grouping rebuilds the engine graph and keeps the play position; track-bound UI state follows the
track ids. Not yet: non-adjacent selections, renaming/colouring the group, Session launch of a group, dragging tracks into groups.

Session group headers have a circular three-line fold icon; clicking it hides/shows all descendants, including their mixer
strips, without changing model/control indices or moving the anchored returns/Main. Nested groups retain their own fold
states. Folding shares Arrangement's undoable `TrackUnfolded` state and survives save/reopen. Member headers show a 6 px
parent-color rail above and a 3 px rail on the left. Track labels sit in the lower 18 px of the 24 px header.
When a group header is selected, its member rails use the selection cyan while retaining each member's own header
color and inner border. Nested groups remain supported. **UI-29**.
Automatic track names use the current track order, including group tracks and folded members: inserting a group before
track 2 displays `2 Group`, followed by `3 MIDI`, etc. Session, Arrangement, Detail and status navigation share the label.
Empty ALS `Name/UserName` marks an automatic name; `EffectiveName` remains a cache. Explicit names stay literal.
Leading `#` templates also follow the current number (`# Leads`, `### Layers`); renaming an automatic track initially
shows its `#` template. Save/reload and undo preserve the distinction. Older Sonora files with an explicit `Group`
name retain that custom name; no migration guesses at the user's naming intent.
Empty group slots show the same stop squares as ordinary tracks; clicking a square or the group's bottom stop row stops
all descendant audio/MIDI tracks, including folded members. Clicking the slot body only selects it. Group scene launching
is still open. Session row rules use full `SurfaceArea` contrast; scrolling slots stay clipped below the header. **UI-24**.

## Device deletion, selection rectangle, Loop Selection

- **Delete a device**: click anywhere on a device in the Device View (its title highlights), then Delete/Backspace, or right-click
  its title > Delete, or Edit > Delete Device. One undo step; plugin state is captured when the command runs, so undo brings the
  device back in place with its edited state. Top-level devices only for now (devices inside racks keep their own editing).
- **Selection rectangle** in the note editor: drag on empty space to select the notes it touches (Shift adds to the selection,
  the view auto-scrolls near the edges, a plain click deselects). Delete with nothing selected in the note editor now does
  nothing (it used to fall through and delete the whole clip).
- **Cmd+L** (Edit > Loop Selection): see the shortcuts table.

## Delay and Reverb panels (DEV-8c / DEV-6c)

`DeviceBox` uses dedicated compact `EffectPanel` faces for Delay and Reverb. Delay has separate Left/Right Sync grids,
free-time fields, offsets/link, filter display, feedback/freeze, Repitch/Fade/Jump, Ping Pong, Dry/Wet and modulation readouts.
Reverb has dark Input Filter, Early Reflections, Diffusion Network and Chorus sections, with output and room controls.
Drag the filter displays to edit frequency/bandwidth; the diffusion display edits high frequency/gain (Shift edits low).
Display numbers are draggable; choice fields open a menu; knobs retain drag/fine/reset gestures and MIDI mappings.
The title's **…** switches to the existing full parameter groups, including all LFO, mix-law/Eco and Stereo-on-dry controls.
The graphs are parameter-driven visual schematics; they do not claim the reference device's DSP transfer function.

The detail area spans the window below both browser and main view. It has an 8 px lead-in with a dotted MIDI rail and
8 px gaps with twin audio connector marks between device faces. The device tab has no extra track/title tab strip.
MIDI tracks without an instrument show **Drop Instrument / Here** after leading MIDI effects. Audio tracks, returns and
instrument-equipped MIDI tracks omit that slot. **Drop Audio Effects Here** fills the end, with at least 180 px available
by scrolling even for a long chain. Dropping an instrument inserts it after MIDI effects and before existing audio effects;
dropping another instrument replaces the existing one in a single undo step, preserving the effects.
Instrument detection uses device metadata: **Serum 2** is an instrument; **Serum 2 FX** is an audio effect and does not
fill the instrument slot. Imported unusual chains still load unchanged.

**Reorder audio effects:** drag a device's title to another position in the same track. A cyan insertion line marks an
allowed move; red means the destination is invalid. MIDI effects and the instrument remain ahead of audio effects.
Dragging near either horizontal edge scrolls the chain. Escape or releasing outside the device view cancels; simple
clicks and control gestures do not reorder. Moves use one undo step and retain processor instances, edited parameters,
automation identities and existing mapping rebinding through save/reload. Reordering inside rack chains or between
tracks is not implemented. X/Y assignments survive the temporary null model used during a device edit.

The browser audition footer belongs entirely to the right-hand list, below its rows. It contains a small blue headphone
button and centered **0 items selected / 1 item selected** text. The left category column continues to the bottom.

Plugin wrappers show a dark X/Y pad with two **none** fields. Select a parameter for each axis, then drag the ring to
change those parameters; choosing none releases that axis. Bindings survive UI rebuilds within the document, but are not
yet imported from or saved to `.als`/sidecar. The wrench opens the existing hosted editor when available. Full MPE,
hot-swap and preset/header actions remain UI-22b.

The status bar shows the selected track and a miniature rendered device chain. Click/drag the miniature to navigate;
wheeling over it scrolls the chain. The highlighted frame shows the viewport. The triangle shows/hides the detail view;
click the track label for **Devices / Clip / Monitor** (the Clip and Monitor views retain their tabs). The three-bar icon
toggles the Session mixer; its adjacent arrow offers Mixer, Sends and In/Out. Mixer/sends visibility is saved with layout.
Arrangement mixer visibility has not been added. Stock device chains use a fixed 200 px height; their upper divider does
not resize. Clip, sample and monitor editors restore their saved resizable height. UI-27's explicitly docked Sonora Prism
dashboard remains a resizable expanded editor. Switching to a stock chain restores the compact height.

Reviewed snapshots: `screenshots/device-chain-empty-midi.png`, `device-chain-empty-audio.png`,
`device-chain-scrolled.png`. `ui_snapshot` also accepts `--device-scroll 0..1`, `--no-mixer`, `--no-sends`, and `--browser-select N` (filtered row index).

## Control-bar tools

Left to right after the transport (as in Live): **+** MIDI Arrangement Overdub, **automation arm**, **Re-Enable Automation**,
**Capture MIDI**, **Session Record**, then the loop group: start field, **punch-in**, **loop**, **punch-out**, length field.
Right of the loop group: **Draw Mode** (pencil), **Computer MIDI Keyboard**, **Key** (Session clip key map); the **MIDI** box enters MIDI
map mode (it still flashes on MIDI activity). Below 1000 px the keyboard/draw/key and the first three tools are hidden (their
shortcuts keep working).

- **Overdub (+)**: with it on, a finished Arrangement MIDI take that lies inside one existing MIDI clip is merged into that clip's
  notes (undo = one step); takes that cross clip borders, start in empty space, carry controller envelopes or are audio replace
  what they cover as before. Saved with the app layout.
- **Automation arm** (on by default) + Arrangement record: moving a mixer control (volume, pan, sends, activator, device on/off) or a
  native device parameter writes breakpoints into its lane; the lane between the first touch and the stop is replaced and the touched
  control holds its last value until the stop. Works without an armed track too (record, move controls, record again). One undo step
  "Record Automation". Hosted plugin parameters are not recorded.
- **Re-Enable Automation (arrow)**: lights when an automated control was moved by hand while its lane played (that control keeps the
  hand value instead of snapping back); click it to give every overridden control back to its lane. Hand edits while stopped are not
  overrides: the next start lets the lanes win. The record take re-enables the controls it just wrote.
- **Capture MIDI (dashed square)**: the notes you play on armed MIDI tracks are remembered even when nothing records; the button turns the
  latest passage (everything since the previous capture, ending at a pause of 8 s) into a clip in the first empty Session slot of each armed
  MIDI track (selected MIDI track if none is armed), or into the Arrangement at the play position when no slot is free. Passages played
  while the transport ran keep their bar position, free playing starts at the first note. Greyed out while the buffer is empty.
- **Session Record (circle)**: records into Session slots whatever view is showing (the red button keeps its view-dependent behaviour:
  Arrangement in Arrangement view, Session in Session view); lit while a Session take runs.
- **Loop fields**: drag the start or length up/down; the left third of the box changes bars, the middle beats, the right sixteenths. One
  undo step on release; blocked while recording. The punch icons are the old In / Out buttons.
- **Draw Mode (B)**: in the note editor dragging paints one note per grid cell (fast drags leave no gaps), clicking a note removes it; in an
  automation lane dragging paints a stepped value per grid cell; in the clip envelope editor it paints grid cells as before.
- **Computer MIDI Keyboard**: A W S E D F T G Y H U J K O L P ; ' play two octaves from C at the current octave (default C3 = note 60), Z/X
  move the octave, C/V the velocity (steps of 20). Hold Shift to use single-letter shortcuts while it is on; text fields keep priority; it excludes Key
  map mode. Notes reach the armed tracks like a controller (and feed Capture MIDI); focus loss releases held notes.

## Zoom speed, fades, copy-drag, grouping and browser keys

- **Wheel / pinch sensitivity** (`ui/WheelScale.h`): JUCE reports a trackpad scroll as pixels x 0.5/256 and a mouse notch as ~0.04-0.12, the old
  factors made content move about a quarter as fast as the fingers and zoom crawl (+1-5 % per event). Scrolling is now one-to-one
  (`kWheelPixels` = 512 per unit) and Cmd+wheel zoom is exponential (`wheelZoomFactor`, about +25 % per notch). The Device View keeps its older factor.
  Frame cost is not the limit: the Arrangement of the biggest real-world test set paints in 4-13 ms per frame even in the Debug build.
- **Clip fades** (Arrangement audio clips): hover or select an audio clip and drag the small squares at the top corners of its waveform: left = fade in,
  right = fade out (grid snapped, never overlapping each other; back to ~0 = Live's automatic short edge fade). The fade is drawn as a shaded curved boundary,
  played with the same concave gain curve (in the Re-Pitch/unwarped path and the Beats/Tones/Texture/Complex paths) and saved as `<Fades>`
  (`FadeInLength`/`FadeOutLength` in beats, `IsDefaultFadeIn/Out`; authored if the clip had none). Custom curve shape (skew/slope), imported shape evaluation and crossfades are not implemented yet; the current fixed curve is Sonora-authored.
- **Option+drag copies**: Option+drag a clip body (to another position or another track of the same kind) leaves the original and drops a copy
  (Cmd skips the grid then); Option+drag selected notes in the note editor duplicates them. A plain Option click copies nothing.
- **Grouping**: any selection of tracks can be grouped - Shift+click (range) / Cmd+click (toggle) a track name, or Shift+click in a lane - with Cmd+G, the Edit menu or
  right-click > Group Tracks (track headers in Arrangement and Session). The group takes the place of the first selected track, the selected tracks follow it in
  their old order and unselected tracks in between end up after the group. Right-click > Ungroup Tracks on a group.
- **Browser**: the headphone button in the right content column (blue while on) turns sample preview off and on; Up/Down walk the list (also while the search field has the focus),
  scroll the row into view and audition samples; Left closes the selected folder/group, Right/Return opens it.

## Tempo entry, input menus, device scrolling, Monitor, Export, Stems, Utility

- **Tempo box**: double-click types a value (Enter commits, Esc cancels), wheel nudges, drag still scrubs. `wantsTextInput()` keeps letters from becoming shortcuts while typing.
- **MIDI From / channel / audio source boxes** open popup menus (`UiContext::showChoices` replaces the native menu in tests).
- **Device View**: chains wider than the view scroll sideways (swipe anywhere, also over knobs); a thin scrollbar sits under the devices; a new device scrolls into view.
- **Folded groups** show a summary of their children; clicking the group's ring folds/unfolds (persisted as `TrackUnfolded`).
- **Monitor view** (status-bar track menu > Monitor, or third tab when Clip/Monitor is visible): left LUFS readouts + level bar + Reset, centre Bars / Curve / Scope (Scope: time window button, Trig), right correlometer, goniometer, balance, width. Uses `RetroDisplay*` colours for the displays. The analysis thread runs only while the tab is visible.
- **Export Audio** (File menu, Shift+Cmd+R): modal like Preferences; rows Rendered Track, Render Range, Start/End Bar (typed, only for Custom), Tail, Options (Normalize / Convert to Mono), File Type / bit depth / Dither. While rendering only Cancel/Esc work.
- **Clip panel > Stems** button next to Reverse (turns into Cancel while a separation runs).
- **Utility** is a compact panel (92 px button column, 2x2 knob grid, bass mono column); `DeviceBox::utility_` in `DetailView.cpp`.
  *Superseded 2026-10-17:* `DeviceFace::Kind::Utility`, measured from a capture of the reference (see below).
- `ui_snapshot` gained `--monitor bars|curve|scope`, `--export idle|running|done|failed` and `--bench N` (frame timing); a Release UI build lives in `build-release-ui`.


## Agent Control and optional native LLM

View > Agent Control or Cmd+Shift+P opens the native dock. The default view has **Allow MCP access to this project**,
request activity, readable action results/errors, receipt details and guarded **Undo agent edit**. Enter prompts in your
MCP client. Activity distinguishes access off, waiting, processing and the last authenticated request; it does not claim
a persistent client connection.

**Configure…** optionally enables a native LLM using OpenAI, Anthropic, Gemini, DeepSeek, GLM/Z.ai, OpenRouter or a custom
OpenAI-compatible endpoint. Model, base URL and protocol are editable; API keys stay in macOS Keychain. Only a valid,
enabled configuration with a stored key reveals the prompt and Run button. Native LLM and MCP access are independent.
Disable the native LLM to hide the composer; removing its key also hides it. The setup sheet explains that prompts and
queried project data are sent to the selected provider. No audio is uploaded.

In the configured composer, Enter runs, Shift+Enter inserts a line, and Return while Japanese IME text is marked commits
composition without submitting. Text undo belongs to the composer. Entering it releases held computer MIDI/mapping keys.
Stop cancels future model/tool work; edits already committed remain available for guarded undo. Switching projects ends
the request. Results are session-only and changes remain unsaved.

The Serum workflow requires the scanned Serum 2 instrument VST3 version 2.0.24 and a 4/4 project. It creates a new
track with D#m/B/F#/C# chords, keeps project tempo and leaves the edits unsaved. Undo refuses to remove intervening
manual changes. Missing plugins and unsupported operations report errors instead of silently substituting an instrument.

Enable MCP access in the intended project. Switching projects or quitting revokes it. Setup and current limitations are
in `docs/agent/{README,NATIVE_LLM,CONTROL_API}.md`; broader controls and per-client pairing remain roadmap work.
Reviewed native screenshots: `screenshots/agent-control-{mcp,native-llm,settings}.png`.

## Arrangement context and channel deletion (UI-37)

Right-clicking the empty Arrangement grid or a time selection shows Zoom to/Back from Time Selection (Z/X),
Insert Empty MIDI Clip (Shift+Cmd+M on MIDI lanes), Activate/Deactivate Loop (Cmd+L), and the rendering commands.
Freeze/Bounce remain disabled. Adaptive Grid and Fixed Grid are inline choice tables with checkmarks, followed by
Narrow/Widen and Triplet Grid; available limits disable Narrow/Widen. Five adaptive densities retain the old saved
Narrow/Medium/Wide values. Grid picks, including time-selection menus, save through the existing layout callback.
Actual clip right-clicks outside a time selection retain the clip menu.

Header right-click **Delete** and Delete/Backspace remove selected MIDI/audio/group/return channels as one undo
command. Groups include their descendants; Main is protected. Note/clip editors and text fields retain their local
Delete behavior. Recording/pending-recording guards apply to both menu creation and action execution. Removing a
return deletes its sends, parameter/clip/take envelopes and SendsPre entry, retaining surviving send identities,
MIDI bindings and unknown XML. Deleted-track input routes become No Input, audio outputs fall back to Main,
MIDI outputs become None, and affected sidechains switch off. Undo/redo and save after deletion/undo are supported.
Selections, widths, lane heights and runtime MIDI targets follow channel IDs, including returns and Main.
`ui_snapshot --arrangement-menu` and `--track-menu --track N` render the real menu drawing code for review.

## Context menus and keyboard audit (UI-26)

Track headers in both views share the same ordered context actions. Track/clip colors are undoable and saved.
Session slot context clicks select and open a menu without launching, stopping or recording. Clip menus offer
Duplicate, Delete, Rename and activation; note menus offer duplication, deletion, quantization and selection plus
editor grid settings. Browser item menus provide Load/Open, disclosure and Show in Finder where applicable.
Device titles expose Delete alongside disabled unsupported editing commands. Knobs/faders have Return to Default
and MIDI-map entry points; Delete resets the focused parameter and arrows adjust it. Right drags/releases do not
change a knob, fader, transport or toggle. Existing special-purpose knob menus retain priority.

Browser folder arrows now expand in place with indented children loaded asynchronously. Right opens without
re-closing; Left selects the parent or closes an open folder. A right-arrow press on a plug-in/device does not load it;
Return loads it. Up/Down auditions samples when preview is enabled. Group columns have a lighter slot background;
member headers retain parent-color framing. Arrangement member headers are indented, group rings use their own
three-line glyph, and expanded groups use a normal lane height. MIDI arming has a note glyph; audio arming a circle.

The macOS title bar retains traffic lights and the centered document name while sharing the canvas Desktop color
and hiding the native separator. The document title omits the application suffix. Native verification requires the
rebuilt app, not an already running older binary.

Keyboard routing gives the focused editor and its ancestors priority and never falls through to another editor.
Note Cmd+D duplicates the selected note span; Cmd+Up/Down adjusts velocity, plain arrows retain pitch/time editing,
Cmd+Shift+A inverts selection and 0 toggles selected notes. Cmd+G in note/device focus cannot group tracks.
Session arrows move between slots, Enter launches and Cmd+Enter stops the selected track. Escape returns to its
header; U folds the selected group. Shift+Space continues playback. macOS Option symbol characters are normalized
for shortcut matching. **[SHORTCUT_AUDIT.md](SHORTCUT_AUDIT.md)** records remaining gaps and verification boundaries.
Nested track groups remain supported, as in the reference app and its documented group hierarchy.

## Region editing and Arrangement display controls (UI-31)

- Drag a clip's waveform/note body or empty Arrangement lane to select time; drag its title strip to move it.
  Edge trim/resize, fade squares and Option-copy retain priority. Cmd+D duplicates the full selection, including gaps,
  immediately after it on the selected tracks. Only the destination interval is replaced; surviving tails retain source
  offsets, unwarped audio offsets use seconds, and authored track automation retains boundary jumps. Static pre-roll
  manual values stay static. Repeated Cmd+D advances the selection; each duplicate is one undo step and saves normally.
- In the piano roll, drag the ruler to select all pitches in a time range, or drag empty note space for a pitch/time
  rectangle. Cmd+D uses that range's full width, preserving its leading/trailing silence. Clicking individual notes
  retains note-span duplication. Copied note IDs are fresh. Loop bounds and clip envelopes are not extended/copied.
- In the velocity/probability lane, drag horizontally to draw through multiple stems (including simultaneous chords).
  Fast and reverse drags interpolate all crossed notes; notes outside the stroke stay untouched. A vertical stem drag
  retains batch editing of selected notes. One history entry is committed on release.
- The Arrangement footer has Waveform Vertical Zoom, its factor, H and W. The waveform toggle/factor affect drawing
  only. Drag or scroll the factor, double-click to type (Enter/Escape), Cmd-click to reset, or right-click for x/dB display
  and reset. Adjusting the factor enables waveform zoom immediately. Option+wheel over the Arrangement lanes resizes
  track heights around the pointer; a time selection or selected headers resize all selected visible lanes together.
  Option `+` / `−` resizes the selected track(s). H fits visible track heights (minimum 18 px), W fits
  the song horizontally; pressing again restores prior geometry. H/W keyboard shortcuts apply with Arrangement focus
  and the computer MIDI keyboard off. Persistent display state and resize-aware fitting remain UI-31b.
- Session and Arrangement icons sit side by side at the far top right (vertical bars then horizontal bars), with hover
  tooltips. MIDI tracks with no instrument show MIDI activity dots, MIDI To/No Output, activator/solo/MIDI arm and input
  routing. They hide audio volume/pan/sends/readouts/meters. Instrument racks and hosted instruments restore audio
  controls. MIDI activity is an atomic event counter, not a fabricated audio peak. External MIDI-output routing is
  still a separate backend capability.

## Moving a selected region (UI-32)

Select time across one or several Arrangement tracks, then drag inside that selection (a clip title, its body or a
gap) to move the entire block. Every clip intersection keeps its relative time/channel position, including silence.
A preview shows the destination while the song stays unchanged until mouse release. One undo restores the whole move.
Dragging the selection past the last source track into blank canvas keeps all touched source tracks selected.
Shift-drag starts a fresh range; fades and lane-height handles retain their own gestures. Dragging without an existing
region retains single-clip/title and time-selection/body behavior.

The destination replaces only the covered range, including gaps; source/destination overlaps are staged before any
insertion so a block cannot overwrite itself. Partial clips leave their outside portions in place, with source
seconds/beats preserved. Authored track automation follows the region, and clip envelopes stay in content time.
Full clips keep their UIDs during moves; splits/copies get fresh identities and retain immutable XML for save/history.

Option-drag copies the whole region, Cmd bypasses the grid, and the start clamps to beat zero. Clicking without moving,
returning to the original position, Escape or an incompatible drop makes no edit. Vertical drops require matching track
kinds and corresponding existing envelope targets (mixer, MIDI controller or same device/parameter); an unavailable
mapping rejects the complete drop. Returns/Main and take lanes are not region destinations. Finish recording first.
`ui_snapshot --region-drag start:end:firstTrack:lastTrack:beatDelta:trackOffset` renders an uncommitted preview for QA.


## Range selection, track dragging and the MIDI editor (UI-34 / UI-35)

Click an Arrangement clip, then Shift-click another clip to select the full span between their outer edges, including
both clips, gaps and every channel between them. Further Shift clicks extend from the first clip. Drag inside that
selection to move the block (UI-32); Shift-drag on a clip without an anchor still starts a time selection.

Drag a track title in Session or Arrangement. A thin insertion marker shows a reorder; the middle of a group title
shows a framed destination for moving into that group. Group tracks carry their full nested subtree; selected roots
can move together. Drop near a group edge to put the block beside it rather than inside. Member-title boundaries remain
visible when their parent is selected. Escape, outside/invalid drops, a click without dragging, focus loss and recording
cancel the gesture. One undo restores order, parents, default group routing and the destination's folded state. Custom
output routes survive. Selection, widths and lane heights follow stable track IDs through the app's existing remapping.
Returns/Main are not draggable source tracks. Use the wheel to reach off-screen destinations.

Audio meters now decay to exact silence and repaint every visible decay step. The peak readout still holds its peak
until clicked, independently of the live meter bars.

The piano toolbar has **Fold** (F), **Scale** (G) and **Highlight Scale** (K). Fold shows occupied pitches; on an empty
clip it retains the keyboard so notes can be created. Scale shows the current Major/Minor scale plus existing outside
notes. Fold and Scale are mutually exclusive. Highlight is independent and tints scale rows/keys with the clip color.
The toolbar has its own row above the ruler; keyboard audition and the resizable velocity/probability lane remain.
Scale state is still local editor state, not a complete import/persistence implementation.

Notes, Envelopes and MPE are centered in the Clip header. Envelopes retains the existing linked MIDI-controller editor.
The compact sidebar has Clip, Launch, Pitch & Time, Transform and Generate. Duplicate sits above Loop. MIDI marker Set
buttons use the Arrangement transport position in clip-content time; source-seconds/audio Set buttons are not exposed.
Launch is initially folded. The sections and property fields scroll when the detail area is short; drag the sidebar
divider to change its width. Pitch & Time includes Reverse, Invert, Fit to Scale, Legato, x2, /2 and Humanize.

Transform provides original Arpeggiate, Chop, Connect, Ornament, Quantize, Recombine, Span, Strum, Time Warp, Humanize,
Fit to Scale, Reverse, Invert and Legato implementations. Generate provides Rhythm, Seed, Shape and Stacks. These are
Sonora's simplified native algorithms, not complete replicas of all reference tools/parameters. Choose a tool, drag
its numeric fields vertically (Shift for fine adjustment), then Apply; Reset restores that tool's parameter defaults.
Cmd+Enter applies from the detail editor or Edit menu. Shift+Cmd+U opens Quantize Settings. Only relevant fields are
shown; Rate is in beats, Gate is a proportion of note length, Parts/Steps changes subdivision/rotation/voices where
applicable, Density controls generated occupancy, and pitch bounds are MIDI numbers. Quantize uses start-time amount.
Recombine currently rotates pitches; Span applies a rate/gate duration; Shape is a sinusoidal contour; Stacks uses an
original four-step chord-root pattern. Auto Apply/live previews, custom curves and full pattern/style sets remain UI-35b.

Transform targets selected notes, a ruler time selection or the clip loop. Generate targets the ruler selection or loop,
replacing notes starting in that interval while retaining outside notes. Output stays selected for successive tools.
When scale highlighting is enabled, generated pitches fit the chosen Major/Minor scale within the pitch bounds when
that range contains a scale pitch. Apply creates one undo entry and edits save normally. Algorithms have explicit output
limits so tiny rates/long ranges cannot create unbounded note counts.

MPE edits one note at a time: select a note in the piano roll, open MPE, choose Pitch, Slide, Pressure or Release Velocity,
then click/drag the graph. The note chooser can choose another note. Right-click/double-click a point to remove it;
Escape/focus loss cancels an active gesture. A gesture previews without mutating the model, then commits once on release.
Changes to that note data during the gesture cancel its commit. Curves use note-relative beats; pitch uses ±48 semitones,
slide/pressure 0–127 and release velocity 0–127. Saved/undone/redo curves are covered by self-roundtrip tests. Existing
unknown expression metadata is preserved for untouched source lanes.

Playback emits bounded MIDI on member channels 2–16 for expressive notes, ordinary notes on channel 1. Receivers need
MPE enabled and a matching ±48-semitone member bend range. Fifteen overlapping expressive notes per clip are supported;
extra notes play without expression and add a build-log message. Allocation is currently per clip, so simultaneous
independent clips on one instrument can contend. Note capture remains a fixed-size value type; expression vectors are
prepared outside the callback. Stop/seek/loop releases include all channels. Existing imported curve controls are retained
but playback/editor interpolation is linear. Native instrument/MPE hardware response and reference-app acceptance remain UI-35b; this is not a claim of complete cross-DAW MPE compatibility.

References consulted: [Editing MIDI](https://www.ableton.com/en/live-manual/12/editing-midi/),
[MIDI Tools](https://www.ableton.com/en/live-manual/12/midi-tools/) and
[Editing MPE](https://www.ableton.com/en/live-manual/12/editing-mpe/). Reference assets/code/fonts were not copied.

## Audio clip media editing (UI-38)

Arrangement waveforms query decoded samples at close zoom and draw anti-aliased continuous envelopes in both
the Arrangement and sample editor. Selected/hovered clips show fade-length squares, yellow curve diamonds and
lower-corner grips. Drag diamonds vertically to bend fades; the same bounded curve drives playback and saves to
FadeInCurveSkew/FadeOutCurveSkew. Edge targets are nine logical pixels (smaller for narrow clips). Option-drag
the lower edge to stretch/shrink from the nearer edge with warp markers and retained pitch; Option body drag copies.

Cmd+J / Edit > Consolidate renders an Arrangement selection into one clip per track, including gaps. MIDI keeps
looped notes, trimmed boundaries, controller/parameter clip envelopes and note-expression identities. Audio renders
clip gain/fades/warp/pitch before track effects; track effects and mixer remain live. Consolidated audio uses Complex
warp at the set's current tempo. Audio analysis/consolidation ranges are currently bounded to ten minutes per job.

Track/grid/clip context menus and Edit expose Freeze/Unfreeze (Option+Shift+Cmd+F), Bounce Track in Place and
Bounce to New Track (Cmd+B). Freeze retains original clips/devices and plays pre-mixer renders; mixer/send controls
remain live. Unfreeze restores original processing. Bounce replaces the source by Audio; Undo restores it. A new-track
bounce starts muted to avoid doubling. Rendering includes native/hosted devices, Session slots and nested groups for
bounce; generated WAVs live under Samples/Processed, travel with project Save As/Collect and remain available for Undo.
Arrangement tails render until the target track is silent for half a second (maximum 30 seconds); Session-loop
steady-state effects still need qualification. The cache uses a versioned SonoraFreeze extension; a reference DAW can ignore it and play original data.

Create and audio-clip context menus offer Melody, Harmony, Drums and Slice to New MIDI Track. Analysis runs locally,
produces editable MIDI, retains the source and commits one undo. Melody/Harmony use Sonora FM; Drums/Slices build
playable sample racks from the source. Detection is approximate (periodicity/spectral/onset analysis), particularly on
mixed/polyphonic/noisy recordings; there is no trained-model download or claim of reference-algorithm equivalence.


### Simpler and additional native devices (UI-39 / DEV-20 / DEV-21)

Simpler now places Sample/Controls tabs in the device title and Classic/1-Shot/Slice vertically beside the waveform.
The black sample display includes timestamps, draggable start/end markers, gain/start/loop/length/crossfade readouts,
loop, voices and retrigger. Trim/readout edits commit once through the existing undoable structural-device path.
The compact bottom strip exposes filter/type/slope/frequency/resonance, LFO waveform/Hz-or-sync/beat rate, ADSR and volume.
Controls exposes transpose/detune, velocity/pan, filter envelope and LFO destinations. Slice uses decoded source onsets;
Warp is visibly disabled. Sampler retains Zones and gains a Modulation page for the additional shared processor controls.

Redux, Tuner, Gate and older Auto Pan have dedicated compact faces. Tuner displays the processor's measured frequency;
right-click its display to choose note spelling. Pitch, Chord, Scale, Random and Velocity provide the existing paged native
parameter controls. All are available through the browser and persist via the normal native state-capture/save path.

Arrangement clip bodies and enabled piano-roll notes now retain the clip-title palette colour without white/velocity tints.
Velocity remains visible/editable in its lane. New Arrangement/Session/captured MIDI clips inherit the destination channel's
colour, while existing independent clip colours are retained. Disabled, selected and mapping states keep their distinct overlays.


### Split, bounce position and Solo (UI-40)

Click a clip's waveform/MIDI body to place the insertion cursor. Cmd+E, Edit > Split Clip or the clip
context menu's Split divides clips there. A time selection splits every intersecting unfrozen Arrangement
track at both boundaries. Audio sample/MIDI loop offsets and envelopes remain in their source clocks;
fragments survive Undo/Redo and save/reload. Session and take-lane splitting are not included.

Arrangement bounce/freeze starts at the earliest enabled clip on the selected track or group subtree,
retains intervening gaps, and leaves the rendered clip at that same Arrangement beat. Session bounces
keep their own slot-relative start at zero.

Solo buttons are blue when enabled. Plain click selects only that channel; click again when it is the
only solo turns it off. Cmd-click toggles one channel without clearing the others. Plain click on a
member of a multi-solo leaves that channel alone in solo. S and Edit > Solo Track use the same exclusive
rule. This governs explicit Solo flags; existing group/return routing continues to apply.

### Meter, vertical zoom and source tempo (UI-41)

The control-bar time signature is editable: drag or scroll its left/right half to change the numerator/denominator,
or double-click and type a value. A full pair such as `7/8` also works; Enter commits and Escape cancels. Numerators
range from 1 to 99, denominators from 1 to 16 in powers of two. Edits update the set, metronome/count-in and bar grid,
save normally and support undo. This is the global meter; timeline meter-change markers/automation remain ENG-11b.

Option+wheel over Arrangement lanes changes track height. Active time selections or selected headers resize their
visible tracks together; Option `+` / `−` works with Arrangement focus. The footer waveform factor adjusts drawing
amplitude independently of lane height and enables waveform zoom as soon as it is changed. Neither changes audio gain.

On the first enable of an audio clip's Warp, source tempo is estimated from decoded rhythmic attacks in the background.
Authored warp grids and previously detected/manual tempos are retained. Audio with no reliable pulse uses set tempo;
half/double-tempo ambiguity can require correction. Double-click BPM to type, Enter to apply, Escape to cancel; wheel
events change it by exactly 0.1 BPM, and vertical dragging also uses tenths (Shift slows the drag). Detected/manual
source tempo survives Warp Off/On and save/reload. A clip/document edit during analysis cancels the pending result.

## Utility matched to the reference (2026-10-17)

`DeviceFace::Kind::Utility` reproduces the reference's Live 12 Utility, measured from a screen recording of the reference.
Face units are 125.4 x 164 and scale
uniformly with the face height (geometry and text: 9.9 regular, 10.7 bold headers). Input column: Ø L / Ø R, channel
chooser, Width knob (label click switches to Mid/Side = `MidSideBalanceOn`), Mono, Bass Mono, frequency box and the
headphone audition switch (`BassMonoAudition`); divider with its arrow; Output column: Gain, Balance, Mute, DC.
`Knob::Style::Ring` is the reference knob: dark 2.6-unit ring and needle, cyan value arc from a triangle marker that
turns cyan when the value leaves it. Knob laws: 0 dB / 100 % / C / mid-side centre point up; Gain +-35 dB linear in dB
(last 5 % fades to -inf, approximation), Width 0-100 % left half and 100-400 % right half. All device title bands now use
`SurfaceHighlight` (lighter than the body) like the reference. Not reproduced: the reference font (no fonts are
copied), the title bar's hot-swap / save / menu icons.

## Swiss Army Meter matched to the original (2026-10-17)

`DeviceFace::Kind::Meter` reproduces the third-party patch's own dark face from the same recording
(`docs/screenshots/native-swiss-army-meter{,-stopped}.png`, original left). Its colours are the original's (sampled),
not theme tokens, and DeviceBox gives it a #333 body in every theme. Text sizes are fitted to the measured widths of
"Integrated", "-17.4" and "-15.20". The original's logo is not reproduced.

## Live 12 device faces (UI-42, 2026-10-19)

EQ Eight, EQ Three, Compressor and Limiter are matched to a Live 12 capture
with the `ReferenceFace` kit
([NATIVE_DEVICE_FACES.md](NATIVE_DEVICE_FACES.md) "Face kit"). Shared look now on every device:

- Title band: activator, name, then the hot-swap / save / menu circles at the right (12.3 units across). Title widgets
  (Simpler tabs, rack page button, Wavetable float buttons) sit left of them.
- `Knob::Style::Device` is the device knob everywhere in device bodies except Simpler (Compact), Utility (Ring) and the
  Delay/Reverb panels matched to the older capture: 36-px ring (3 thick) in a 63.5-px cell, dark unfilled ring, cyan arc from the
  start or from the marker (bipolar: triangle under the label), needle from the centre, label above and value below.
- Faces with several sections (EQ Eight, Compressor) draw body panels separated by title-coloured gutters; a dark display uses
  `RetroDisplayBackground` with rounded corners, cyan (`RangeDefault`) values and amber (`ChosenDefault`) handles/readouts.
- Number boxes are light with a cyan `RangeEditField` fill up to the value; on toggles are amber; controls Sonora does not
  implement keep their frame with grey text and a tooltip naming the task.

## Collab sessions (COL-5)

- **File** menu: Share Session on Local Network, Share Session through Relay, Join Session..., Collaborate...,
  Leave/End Session. The **people button** at the right of the control bar (left of the CPU/MIDI readouts) shows "Share"
  before a session and everyone's avatar during one (a green dot when connected, amber while connecting); it opens the
  Collaborate panel.
- **Collaborate panel** (`ui/views/CollabPanel`, modal like Export): before a session, your name, the two Share buttons with
  the relay address, Paste Invite Link and the sessions found on the network ("Ask to Join"); during one, the invite link
  with Copy, everyone with colour, role (host clicks to switch Editor/Listener), the four safety words and Remove, join
  requests (Allow/Deny), Shared transport (guests), recent notices and Leave/End Session. Text is typed in native prompts.
- **Presence** (`ui/CollabOverlay`): each participant has a colour (`collabColor`, host = blue). Arrangement: their pointer
  as a 2 px line with a name tag on the lane under it, their selected clip outlined, their time selection framed. Session
  view: a bar on top of the column they point at and the name tag at the column bottom (stacked). Piano roll: their pitch
  row tinted and their position, when they have the same clip open. Cursors fade after 8 s without movement and disappear
  after 30 s.
- Guests' window title reads "<set> (shared session)"; Save, Save As and Collect All write a copy with the downloaded samples.
- Check with `ui_snapshot out.png --collab share|host|guest|presence [--session]`.

# Clip editing rules (UI-71)

Every rule Sonora implements for moving, resizing and creating Arrangement clips and for the clip (MIDI note) editor, with
its source. Sources are the Live 12 reference manual ([Arrangement View](https://www.ableton.com/en/live-manual/12/arrangement-view/),
[Clip View](https://www.ableton.com/en/live-manual/12/clip-view/), [Editing MIDI](https://www.ableton.com/en/live-manual/12/editing-midi/))
and a library of 65 real sets made with the reference app (read only), which show how the reference
app stores what its editors produce. Where neither says anything, the rule is marked **unverified**.

Code: `core/ClipGeometry.{h,cpp}` (pure clip geometry), `core/Edit.cpp` (`placeArrangementClip`, `withStart/withEnd`,
splits), `ui/ClipEditing.h` (`dragClip`), `ui/GridModel.h` (`snapSize`, `snapKeepingOffset`, ruler), `ui/views/ArrangementView.cpp`,
`ui/views/PianoRoll.cpp`, `ui/views/ClipControls.cpp`. Tests: `tests/test_clip_geometry.cpp`, `tests/test_clip_editing_ui.cpp`.

## How the reference stores clip markers

Measured on 44,441 Arrangement MIDI clips of the real sets (script: count clips by `LoopOn` and compare the fields):

| Loop | `LoopStart` | `LoopEnd` | `StartRelative` | Arrangement length (`CurrentEnd - CurrentStart`) |
|---|---|---|---|---|
| off (35,317 clips) | start marker | end marker | always 0 | always `LoopEnd - LoopStart` |
| on (9,124 clips) | loop brace start | loop brace end | start marker - `LoopStart`, always in `[0, loop length)` | free: the loop repeats (141 clips are longer than their loop) |

`OutMarker` holds the end marker while the loop is on, `HiddenLoopStart/End` the brace while it is off (Sonora reads neither yet).
Content units are beats for MIDI and warped audio clips and seconds for unwarped audio clips (docs/ALS_FORMAT.md "Clip timing").

The only files that broke the unlooped invariant were saved by Sonora (394 clips with a stale
`LoopEnd` and 350 with `StartRelative` != 0): its trims and splits moved `StartRelative` and never the end marker. Now
`normaliseUnloopedMarkers` keeps unlooped Arrangement clips in the reference's form after every edge drag, split, region
edit and marker edit; playback does not change. Every clip editor view and the Clip panel compute an unlooped clip's end from
its Arrangement length (`clipMarkers`), so older files still show the right end.

## Grid and snapping

- Adaptive or fixed grid, Narrow/Widen, Triplet, Snap to Grid (Cmd+1..5) — Arrangement View "Editing Grid" (6.10). Unchanged (UI-19).
- **Cmd bypasses snapping; with Snap to Grid off, Cmd snaps temporarily.** Manual 6.10: "Hold Alt (Win) / Cmd (Mac) ... to
  bypass snapping. If the grid is off, the modifier temporarily enables it." Was: the modifier only bypassed, and Option did too.
  Option is no longer a bypass in the Arrangement and the note editor (on macOS it copies; Editing MIDI "Moving Notes").
  (`ui::snapSize` is `snap != modifier`.) The automation editor still treats Option as a bypass (not reviewed here).

## Arrangement clips

- **Move**: only the clip's title bar drags it (6.7 "only the clip bar is draggable"); the drag is relative to the grab point;
  the clip's start lands on the nearest grid line of the current grid. Unchanged, but see the next rule.
- **Magnetic objects**: "Clips snap to the editing grid, as well as various objects in the Arrangement" (6.7) — the other
  clips' edges on the destination lane and the locators. `snapClipStart` / `snapWithTargets`: the nearest of the grid line and
  the targets; for a move both edges of the clip may dock on a target. Was: grid only. Time-signature changes are not targets
  (Sonora has no signature changes).
- **Shift while moving** snaps the clip's end instead of its start (UI-60). Not a reference rule; kept.
- **Option-drag copies** (unchanged); **vertical drag** moves to another track of the same kind (unchanged).
- **A clip dropped on others overwrites them**: the covered part of a crossing clip is removed (its content stays where it was
  on the timeline), clips entirely under it are removed, a clip the dropped one lands inside is split. The reference never has
  overlapping clips on one lane (0 overlaps among the 12,877 main-lane clips of the sets it saved). `model::placeArrangementClip`, used for moves, edge drags, Option-copies,
  marker edits that change an unlooped clip's extent, and Cmd+D. Was: clips simply overlapped. **The exact reference behaviour for
  partial covers is inferred** (it matches what its consolidate/record paths leave in the sets), not read from the manual.
- **Right edge** (6.7 "Dragging a clip's left or right edge changes the clip's length"): snaps to grid/targets; never shorter
  than one grid step (was 1/16 beat). Loop off: the end marker follows, so more (or less) of the clip's content plays.
  Loop on: the loop repeats, the brace does not change. Unlooped audio cannot pass the end of its sample (unwarped: in seconds;
  warped: through the warp markers). `model::resizedEnd`. Was: only `CurrentEnd` changed, audio could run past the sample.
- **Left edge**: trims or extends while the content stays where it is on the timeline. Loop off: the start marker
  (`LoopStart`) moves; extending left reveals content before the old start (MIDI may go before 1.1.1, audio stops at the
  sample's first frame). Loop on: the start marker moves through the loop and wraps around it, so extending left shows earlier
  repetitions. Minimum one grid step. `model::trimmedStart`. **Wrap-around is unverified** against the reference app (the
  stored invariant "start always inside the brace" is verified).
- Edges are grabbed in the title bar only; below it the body selects time (UI-45, kept).
- **Insert MIDI clip** (Editing MIDI "Creating a MIDI Clip"): Shift+Cmd+M or Create > Insert Empty MIDI Clip spans the time
  selection; without one it is one bar at the bar of the play position. The Arrangement's own Shift+Cmd+M ignored the selection
  (the menu did not); fixed. It refuses to land on existing clips. Double-click in an empty MIDI lane: one bar at the grid
  position left of the click, shortened before the next clip (unchanged; the manual names the gesture, not the length:
  **the one-bar length is unverified**).
- **Cmd+D** places the copy right after the clip and now overwrites what it covers there (was: overlapped).
- New clips (`makeEmptyMidiClip`) start their content at 0, so their editor reads 1, 1.2 ... like the reference's inserted clips
  (most clips in the sets have `LoopStart` 0 wherever they sit in the Arrangement).

## Clip editor (piano roll)

- **Numbering**: the ruler shows the clip's own time, bars.beats from content beat 0 = "1" — the same values the Clip panel's
  Start/End/Position show and the note `Time` uses. A clip at bar 5 whose content starts at 0 reads 1, 1.2, 1.3, 1.4, 2 ...; a
  split fragment that plays content 92..94 reads 24 .. 24.3 (as the reference's Start field does for it). Positions before
  1.1.1 read 0.4, 0, -1 ... (`rulerMarks(..., allowNegative)`; the label convention before bar 1 is **unverified**). The ruler,
  grid and tiles follow the **clip's own signature** (Clip panel > Signature; Clip View manual: display only), not the set's.
- **Opening a clip** shows the view the reference saved with it (`<ScrollerTimePreserver LeftTime/RightTime>` in content
  beats); otherwise the clip's region: from the start marker (or the loop start when earlier) to the loop end, or start to end
  marker when the loop is off (`openingEditorRange`). In the real sets 1,604 of 2,453 MIDI clips store exactly that region,
  141 store 0..0 (never opened) and the rest a view the user zoomed to. Was: a looped Arrangement clip was fitted to its
  Arrangement length (a 16-bar clip with a one-bar loop showed 16 bars), and no saved view was used.
- **Markers** (Clip View "Start and End Markers", "Loop Brace"): a strip under the ruler's numbers shows the start flag (points
  right), the end flag (points left, hidden while the loop is on, as the reference hides it) and the loop brace (grey while the
  loop is off). Dragging snaps to the clip grid (Cmd: free): start, end, the brace's edges and its body (moves the loop, keeping
  its length and its offset from the grid). An unlooped Arrangement clip's length follows Start/End (its left edge stays), and a
  looped clip's Arrangement length never changes (`model::withMarkers`). Was: markers were not draggable in the note editor.
  The note area outside the clip's region is shaded and notes there are drawn faded.
- **Clip panel**: Start/End/Position/Length use the same rules; **Position moves the whole loop** (Length stays; was: it changed
  the length). The Set buttons too.
- **Notes**: moving keeps a grooved note's offset from the grid unless it is dragged onto a grid line ("Notes will also snap to
  an offset, which is based on the original placement of the note relative to the grid", Editing MIDI "Grid Snapping";
  `snapKeepingOffset`). Was: the grabbed note's start jumped to the grid. Resizing snaps the end to the grid, minimum one grid
  step; a drawn or double-clicked note is one grid step long; Option-drag copies (unchanged).

## Not done / open (UI-71b)

- Writing `ScrollerTimePreserver` back (Sonora keeps the file's value; a view zoomed in Sonora is not saved).
- The per-clip editor grid (`<Grid>`: fixed/adaptive, `GridIntervalPixel`, `Ntoles`, `SnapToGrid`) and the vertical
  note-editor view (`NoteEditorFold*Zoom/Scroll`) are not read; Sonora keeps one clip-editor grid for all clips.
- `OutMarker` (end marker while looping) and `HiddenLoopStart/End` are not read; switching the loop off/on in Sonora does not
  restore the hidden brace or end marker like the reference.
- Turning the loop off on an Arrangement clip longer than its content does not shorten it.
- Audio clip edges: the sample editor (SampleView) has its own marker drags (unchanged); only the Arrangement edges follow the
  rules above.
- Not compared side by side with the reference app (no computer use in this session): the wrap-around of looped left-edge
  drags, overwrite details, the label before bar 1, the default inserted clip length.

# ProFace kit (PLG-23)

The face kit of Sonora's own creative devices: Stutter, Sweep and Phase Align (`src/ui/views/ProFace.h/.cpp`). It
replaced their first faces (0.1.32), which reused the device chain's small knobs and grey panels and were hard to read. These devices are Sonora's own, with no reference device, so AGENTS rule 7
does not apply.

## Look

Graphite panels on near black (`pro::kBackground`, `kPanel`, `kWell`), one accent per device (Stutter pink, Sweep amber,
Phase Align cyan), big knobs with the value inside, the label under it and a caption that says what the value means
("4.9x slower", "overlap x1.9", "rises"), segmented choices, pads, a help line at the bottom that explains the control
under the pointer (or, idle, what the device is doing), and a wordmark ("stutter." with "sonora" and the version).

## Two layouts

A face is drawn on a design grid and scaled to its widget (`ProFace::scale`). **Full** (its own window, the VST3 editor;
1100-1160 x 600-700) shows everything. **Compact** (the device chain; 700-760 x 170) keeps the controls you play live at
a readable size. The face picks the layout from its height (`kCompactBelow` = 330 px). In the chain the face keeps its
aspect at the chain's height (DetailView `proFace_`), and the title bar's window icon opens the full face in its own
window (`ui::ProFaceWindowView`, `AppController::openProFaceWindow`, `src/app/AppProFaceWindow.cpp`), which follows its
device by key through graph rebuilds and closes when the device goes away (like the floating Hush).

## Controls and wiring

`addKnob` / `addToggle` / `addSegment(s)` / `addButton` / `addPad` / `addArea` on the design grid. Knobs: drag up/down
(Shift fine), double-click or Alt-click resets, the wheel turns; ms and Hz are logarithmic; `toNorm/fromNorm` for custom
laws (Stutter's Speed); `driven` + `shown` draw a value a macro sets. Edits write the device's parameters directly; the
app saves them from the running device (`captureDeviceState`), the VST3s report them to the host on a timer.
`ui::wireProFace` connects a face in the app: MIDI/key mapping (`mappingTarget`), Sweep's sample drop and chooser
(`loadSweepSample`: one undoable edit), Phase Align's sidechain source. The VST3 windows are `plugin::ProFaceEditor`
(the full layout letterboxed to any window size, fixed aspect, files dropped on the window).

## Verification

`sonora_tests "stutter plugin"`, `"sweep plugin"`, `"phase align plugin"` drive the VST3 windows with real clicks and
drags at design-grid points (`tests/ProFaceTestKit.h`); the chain tests render the compact faces and the window views.
PNGs land in the test temp folder (`sonora_*_plugin.png`, `*_chain.png`): look at them after any change.

#pragma once
// PLG-25a: "Apply <generator> to Clip" in the app: the clip menu entry (TrackMenu) and every generator face's To Clip
// button (ProFace::onToClip, set by wireProFace) call applyGeneratorToClip. Generic: any devices::MidiGenerator on the
// track works, nothing here names a device.
//
// It renders the running device's settings on a clone (MidiGenerator::renderNotes), writes the result as one undoable edit
// (core/GeneratorClip) and then bypasses the device so the written part is not generated a second time on top of it. The
// bypass is the device's On switch (like clicking it), not part of the undo step; the status line says so.

#include "core/Edit.h"

#include <optional>
#include <string>
#include <vector>

namespace sonora::devices { class MidiGenerator; }

namespace sonora::ui
{
struct UiContext;

// The pattern generator running as top-level device `device` of track `track`, or null.
devices::MidiGenerator* generatorAt(const UiContext&, int track, int device);
// The top-level devices of `track` that are pattern generators (indices into the track's devices).
std::vector<int> generatorDevices(const UiContext&, int track);

// With `clip` (a MIDI clip on `track`): its notes inside its loop become the generator's output over the clip (the
// clip's own notes are the input when the generator listens to MIDI). Without: a new clip of the device's
// clipLengthBeats() at the loop start (loop on) or at the playhead's bar. One undo step through ctx.applyEdit, then the
// device is switched off. `ctx.statusText` says what happened. False when nothing was written.
bool applyGeneratorToClip(UiContext& ctx, int track, int device, std::optional<model::ClipLocation> clip);

// The face's To Clip button: the selected clip when it is a MIDI clip on the device's track, else a new clip.
bool generatorToClipFromFace(UiContext& ctx, int track, int device);
} // namespace sonora::ui

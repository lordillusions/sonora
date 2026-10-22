#pragma once
// Pure edits on MIDI clips used by the Clip panel's Notes section (Rev, Inv, Legato, Dupl.Loop, x2, /2). They return a
// modified copy; the caller commits it through model::modifyClip so everything is undoable and saved. Notes are
// addressed in the clip's content time; only notes starting inside the loop region are touched.

#include "core/Model.h"

namespace sonora::ui::midiedit
{

// UI-35: original native MIDI tools. Scope uses note indices plus a content-time interval.
enum class Tool { Arpeggiate, Chop, Connect, Ornament, Quantize, Recombine, Span, Strum, TimeWarp,
                  Humanize, FitScale, Reverse, Invert, Legato, Rhythm, Seed, Shape, Stacks };
struct ToolSettings
{
    Tool tool = Tool::Quantize;
    double rate = 0.25, amount = 1.0, gate = 0.8, density = 0.75;
    int parts = 4, lowPitch = 60, highPitch = 72, root = 0;
    bool scale = false;
    int scaleName = 0; // model::kScales index used with `root` when `scale` is set
    uint32_t seed = 1;
};
const char* toolName(Tool);
bool isGenerator(Tool);
model::Clip applyTool(const model::Clip&, const ToolSettings&, const std::vector<int>& indices, double start, double end);

// Mirrors note positions inside the loop region (the last note becomes the first).
model::Clip reverse(const model::Clip& clip);
// Mirrors pitches around the middle of the clip's pitch range (highest <-> lowest).
model::Clip invert(const model::Clip& clip);
// Lengthens every note until the next note start (same pitch is not required, like Live); the last note is kept.
model::Clip legato(const model::Clip& clip);
// Doubles the loop and repeats its notes in the new second half.
model::Clip duplicateLoop(const model::Clip& clip);
// Scales note positions, durations and the loop region by `factor` (2 == half speed). Notes outside the loop move too.
model::Clip stretch(const model::Clip& clip, double factor);

} // namespace sonora::ui::midiedit

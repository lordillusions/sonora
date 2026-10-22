#pragma once
#include "core/Edit.h"
#include "core/ResynthModel.h"

namespace sonora::engine
{
// Shared result commit for the clip command, device Create MIDI action and offline probe.
model::EditCommand resynthTrack(const model::Song&, const model::ResynthModel&, model::Device instrument,
                               int afterTrack, double start, int scene = -1);
}

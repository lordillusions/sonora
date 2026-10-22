#pragma once
// COL-1: JSON wire form of model::Op (docs/COLLAB_DESIGN.md section 3). Decoding is strict: unknown fields, wrong
// types and missing required fields throw OpError("invalid_request"). Value ranges are checked by opToCommand,
// against the song the op applies to.
//
// Field names follow the control API (trackId, clipId, scene, laneId, startBeat, durationBeats...). Notes are
// addressed by `noteKey`, never by array index, so concurrent edits of different notes in one clip both apply.

#include "core/Op.h"

#include <juce_core/juce_core.h>

#include <utility>

namespace sonora::model
{
juce::var opToJson(const Op&);
Op opFromJson(const juce::var&);

// A committed op as peers exchange it: {"v":1,"op":{...},"clipUids":[...]}.
juce::var committedOpToJson(const Op&, const OpIdentities&);
std::pair<Op, OpIdentities> committedOpFromJson(const juce::var&);

// Compact single-line text with enough digits that every double parses back to the identical value.
juce::String opMessageToString(const juce::var&);

} // namespace sonora::model

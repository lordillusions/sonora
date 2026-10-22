#pragma once
// UI-41: bounded worker-thread tempo estimation from the sample cache's detected attacks.
#include <span>
namespace sonora::engine
{
struct TempoEstimate { double bpm = 0, confidence = 0; };
TempoEstimate estimateSampleTempo(std::span<const double> onsets, double preferredBpm);
}

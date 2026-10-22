#pragma once
// UI-38: offline, pre-device clip rendering for consolidation and audio analysis.
#include "engine/AudioClipPlayer.h"
#include <atomic>
namespace sonora::engine
{
struct ClipRenderResult
{
    std::shared_ptr<SampleData> sample;
    std::string error;
};
ClipRenderResult renderClipRange(const model::Song&, const std::vector<model::Clip>&, double start, double end,
                                 double sampleRate, SampleCache&, const std::atomic<bool>& cancel);
}

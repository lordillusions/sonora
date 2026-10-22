#pragma once
#include "core/ResynthModel.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>

namespace sonora::engine
{
struct ResynthAnalysisResult
{
    std::shared_ptr<const model::ResynthModel> model;
    std::string error;
    bool cancelled = false;
    double fitSeconds = 0; // > 0 when the source has too many notes: the leading part that fits the note limit
};
// Worker only. Input is bounded before any analysis storage is allocated.
ResynthAnalysisResult analyzeResynth(const juce::AudioBuffer<float>&, double sampleRate, const std::string& name,
                                    const std::atomic<bool>& cancel, std::atomic<float>& progress);
std::vector<model::MidiNote> resynthPhrase(const model::ResynthModel&, double beatsPerSecond);
} // namespace sonora::engine

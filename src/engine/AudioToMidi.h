#pragma once
// UI-38: local pitch/onset analysis. Worker only, no external service/model download.
#include "core/Model.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <atomic>
namespace sonora::engine
{
enum class AudioToMidiMode { Melody, Harmony, Drums, Slices };
struct AudioToMidiResult
{
    std::vector<model::MidiNote> notes;
    std::vector<double> slices; // seconds, start of each detected hit/slice
    bool cancelled = false;
};
AudioToMidiResult audioToMidi(const juce::AudioBuffer<float>&, double sampleRate, double tempo, AudioToMidiMode,
                             const std::atomic<bool>& cancel, std::atomic<float>& progress);
}

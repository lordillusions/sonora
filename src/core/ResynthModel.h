#pragma once

// RESYN-1: portable, immutable analysis data. No audio/UI dependencies.
#include "core/Model.h"
#include <array>

namespace sonora::model
{
struct ResynthFrame
{
    static constexpr int harmonics = 96, noiseBands = 4;
    float time = 0, pitch = 0, confidence = 0;
    std::array<float, harmonics> cosine{}, sine{};
    std::array<float, noiseBands> noise{};
};

struct ResynthNote
{
    float start = 0, end = 0, root = 60, confidence = 0;
    float sustain = 0;
    std::vector<ResynthFrame> frames;
};

struct ResynthModel
{
    static constexpr int version = 2, maxNotes = 64, maxFrames = 2048;
    static constexpr double maxSeconds = 30;
    std::string name;
    int analysisChannel = 0;
    double sampleRate = 48000;
    int sourceFrames = 0; // duration metadata, retained after removing reference PCM
    std::vector<float> source; // mono reference; Synth never reads this waveform
    std::vector<ResynthNote> notes;
    double duration() const { return (double) (sourceFrames > 0 ? sourceFrames : (int) source.size()) / sampleRate; }
};

// Bounded little-endian, versioned payload with checksum. Used by SonoraState, not an external file reference.
std::shared_ptr<const NativeContent> encodeResynth(const ResynthModel&, std::string& error);
std::shared_ptr<const ResynthModel> decodeResynth(const NativeContent&, std::string& error);
bool validateResynth(const ResynthModel&, std::string& error);
bool trimResynthNote(ResynthModel&, int note, float start, float end);
void removeResynthSource(ResynthModel&); // message/worker thread; Undo retains the prior immutable payload
} // namespace sonora::model

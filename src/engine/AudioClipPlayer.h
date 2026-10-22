#pragma once
// Stateful playback of warped audio clips (ROADMAP ENG-5).
//
// One AudioPlayState lives in each TrackNode (audio thread only). It plays the clip that is
// active on the track and keeps the state the time-stretchers need between blocks:
//
//   Beats mode                       -> slice voices: each slice (transient or grid) is played at
//                                       its natural speed from its onset; gaps are filled by looping
//                                       the slice tail (Loop Off / Forward / Back-and-Forth).
//   Tones / Texture / Complex(Pro)   -> WSOLA granular time-stretch (pitch preserved).
//   Re-Pitch and unwarped clips      -> the stateless resampler (renderAudioClip).
//
// All buffers are allocated in prepare() (message thread); render() never allocates.
// Quality notes: Complex/Complex Pro use the same WSOLA engine as Tones with larger grains (no
// formant control). TODO(ENG-5b): phase-vocoder for Complex/Complex Pro, TransientEnvelope.

#include "engine/ClipPlayers.h"

#include <vector>

namespace sonora::engine
{

class AudioPlayState
{
public:
    void prepare(double sampleRate, int maxBlockSize);
    void reset() { active_ = nullptr; }

    // Adds the clip's audio for `seg` into `out` (2 channels).
    void render(const PreparedClip& clip, const BlockSegment& seg, double tempo, juce::AudioBuffer<float>& out);

private:
    struct Voice
    {
        bool active = false;
        double pos = 0.0;       // source samples
        double sliceStart = 0.0, sliceEnd = 0.0; // source samples
        double loopFrom = 0.0;  // start of the gap-filling loop region
        int direction = 1;      // +1 / -1 (ping-pong)
        float gain = 0.0f;      // fade state
        bool fadingOut = false;
    };

    struct Grains
    {
        int size = 2048, hop = 512, mask = 0;
        std::vector<float> ring[2];
        std::vector<float> window;
        long long counter = 0;      // output samples produced since (re)start
        long long nextGrain = 0;
        double prevChosen = -1.0;   // source start of the previous grain (samples), < 0 = none
        unsigned rng = 1;
    };

    void restart(const PreparedClip& clip);
    void renderBeats(const PreparedClip& clip, const SampleData& sample, const BlockSegment& seg, double tempo,
                     juce::AudioBuffer<float>& out);
    void renderGrains(const PreparedClip& clip, const SampleData& sample, const BlockSegment& seg, double tempo,
                      juce::AudioBuffer<float>& out);

    // Source position (seconds) the warp maps timeline beat `beat` to.
    static double sourceSeconds(const PreparedClip& clip, double beat, double secPerBeat);
    static float edgeGain(const PreparedClip& clip, double beat, double fadeBeats);

    void configureGrains(const PreparedClip& clip, double outRate);
    void scheduleGrain(const SampleData& sample, long long startOut, double srcPos, double pitch, bool search,
                       long long skipBefore, float jitter);
    static float readSample(const float* data, int len, double pos);

    const PreparedClip* active_ = nullptr;
    double expectedBeat_ = 0.0;

    Voice current_, previous_;
    int sliceIndex_ = -2;
    Grains grains_;
};

} // namespace sonora::engine

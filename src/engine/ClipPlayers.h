#pragma once
// Stateless clip renderers. Given a clip and a range of timeline beats, they emit MIDI
// events or add audio for that range. Being stateless makes seeking and looping trivial
// and makes them easy to unit test (tests/test_engine.cpp).

#include "core/Model.h"
#include "engine/SampleCache.h"
#include "engine/Automation.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <memory>

namespace sonora::engine
{

struct BlockSegment;
struct PreparedMidiController { int index = -1; AutomationLane lane; };
std::vector<PreparedMidiController> prepareMidiControllers(const std::vector<model::AutomationEnvelope>&);
// Leaves reserved room for routed input and release messages in the graph's preallocated MIDI buffer.
inline constexpr int midiControllerByteLimit = 65536;
bool renderMidiControllers(const std::vector<PreparedMidiController>&, const BlockSegment&, juce::MidiBuffer&, bool refresh);
bool addMidiControllerEvent(int index, int value, int sample, juce::MidiBuffer&);
void releaseMidiControllers(juce::MidiBuffer&, int sample); // pedal releases + centred bend, all channels

// Immutable, audio-thread-friendly copy of what a clip needs for playback.
struct PreparedClip
{
    model::Clip::Kind kind = model::Clip::Kind::Midi;
    double currentStart = 0, currentEnd = 0;
    model::LoopSettings loop;
    std::vector<model::MidiNote> notes; // sorted by start
    std::vector<model::NoteExpression> noteExpressions;
    std::vector<int> expressionChannels; // parallel to notes, channel 1 for ordinary notes / excess MPE polyphony
    bool expressionOverflow = false;
    uint32_t probabilitySeed = 0;
    bool legato = false; // Session launch: continue at the previous clip's position instead of restarting
    std::vector<PreparedMidiController> midiControllers;

    std::shared_ptr<SampleSlot> sample; // audio only; null when the file is missing, empty until loaded
    bool warped = false;
    model::WarpMode warpMode = model::WarpMode::Beats;
    std::vector<model::WarpMarker> warpMarkers;
    float gain = 1.0f;
    double fadeInBeats = 0.0, fadeOutBeats = 0.0; // clip fades from the edges (never shorter than the automatic edge fade)
    double fadeInCurve = 0.0, fadeOutCurve = 0.0;
    float pitchRatio = 1.0f; // transpose (coarse semitones + fine cents) as a frequency ratio

    // Beats / Tones / Texture / Complex settings (ENG-5)
    int transientResolution = 6, transientLoopMode = 2;
    float granularityTones = 30.0f, granularityTexture = 65.0f, fluctuationTexture = 25.0f;
    // Beats mode with a fixed "Preserve" grid (resolution 0..5): slice starts in source seconds.
    // For resolution 6 (Transients) the slices come from SampleData::onsets instead.
    std::vector<double> gridSliceSeconds;
};

PreparedClip prepareClip(const model::Clip& clip, std::shared_ptr<SampleSlot> sample);
// Session clip: timeline starts at 0 when the clip is launched; a looping clip never ends on its own.
PreparedClip prepareSessionClip(const model::Clip& clip, std::shared_ptr<SampleSlot> sample);
// Convenience for tests/tools: wraps already-loaded data in a ready slot.
PreparedClip prepareClip(const model::Clip& clip, std::shared_ptr<const SampleData> sample);

// A contiguous slice of the audio block in timeline space.
struct BlockSegment
{
    int sampleOffset = 0;
    int numSamples = 0;
    double startBeats = 0.0;
    double samplesPerBeat = 1.0;

    double endBeats() const { return startBeats + numSamples / samplesPerBeat; }
};

// Emits note on/off events (channel 1) for notes whose on/off times fall inside `seg`.
void renderMidiClip(const PreparedClip& clip, const BlockSegment& seg, juce::MidiBuffer& out, bool refreshControllers = false,
                    bool* controllerOverflow = nullptr, const double* earliestOn = nullptr, const double* disabledFrom = nullptr,
                    float velocityScale = 1.0f);

// Adds the clip audio into `out` (2 channels) for the samples of `seg`. Stateless: unwarped
// clips (with transpose) and warped clips in Re-Pitch mode (beats->seconds through the warp
// markers, resampled). Beats/Tones/Texture/Complex are time-stretched by AudioPlayState
// (engine/AudioClipPlayer.h), which falls back to this function for the other modes.
void renderAudioClip(const PreparedClip& clip, const BlockSegment& seg, double tempo, juce::AudioBuffer<float>& out);

// Beat -> seconds through warp markers (piecewise linear, extrapolated at both ends).
double warpBeatsToSeconds(const std::vector<model::WarpMarker>& markers, double beats, double fallbackSecPerBeat);

} // namespace sonora::engine

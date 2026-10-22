#pragma once
// Capture MIDI (Live's retroactive recording): the notes played on the armed tracks are remembered even while nothing is
// recording, and one button press turns the latest passage into a clip.
//
// `record()` is called from the MIDI thread for every Track-role message (it takes a mutex; never the audio thread).
// The passage that is captured is everything since the previous capture, limited to the most recent burst: a pause of
// `kBurstGapSeconds` between two events ends the passage before it.

#include "core/Model.h"

#include <juce_audio_basics/juce_audio_basics.h>

#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
#include <vector>

namespace sonora::midi
{

class MidiCapture
{
public:
    static constexpr double kBurstGapSeconds = 8.0;
    static constexpr size_t kMaxEvents = 20000;

    // `seconds`: a monotonic clock; `beats`/`playing`: the transport when the message arrived.
    void record(const juce::MidiMessage& message, double seconds, double beats, bool playing);
    // True while captured notes are waiting (note-ons since the last capture).
    bool available() const { return noteOns_.load() > 0; }
    void clear();

    struct Result
    {
        std::vector<model::MidiNote> notes; // clip-relative beats
        model::Beats length = 4.0;          // whole bars, at least one
        model::Beats startBeats = 0.0;      // where the passage began on the timeline (0 when the transport was stopped)
        bool wasPlaying = false;
    };
    // Builds the latest passage as notes and removes everything captured so far. `now` closes notes still held.
    std::optional<Result> take(double now, double tempo, double beatsPerBar);

private:
    struct Event
    {
        double seconds, beats;
        bool playing;
        int note;
        float velocity; // 0 for a note-off
        int channel;
    };
    mutable std::mutex mutex_;
    std::deque<Event> events_;
    std::atomic<int> noteOns_{0};
};

} // namespace sonora::midi

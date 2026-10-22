#include "midi/MidiCapture.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace sonora::midi
{

void MidiCapture::record(const juce::MidiMessage& m, double seconds, double beats, bool playing)
{
    if (! m.isNoteOnOrOff())
        return;
    std::lock_guard lock(mutex_);
    events_.push_back({seconds, beats, playing, m.getNoteNumber(), m.isNoteOn() ? m.getFloatVelocity() * 127.0f : 0.0f, m.getChannel()});
    if (m.isNoteOn())
        noteOns_.fetch_add(1);
    while (events_.size() > kMaxEvents)
    {
        if (events_.front().velocity > 0.0f)
            noteOns_.fetch_sub(1);
        events_.pop_front();
    }
}

void MidiCapture::clear()
{
    std::lock_guard lock(mutex_);
    events_.clear();
    noteOns_.store(0);
}

std::optional<MidiCapture::Result> MidiCapture::take(double now, double tempo, double beatsPerBar)
{
    std::vector<Event> burst;
    {
        std::lock_guard lock(mutex_);
        if (events_.empty())
            return std::nullopt;
        // walk back from the newest event until a long pause
        size_t first = events_.size() - 1;
        while (first > 0 && events_[first].seconds - events_[first - 1].seconds <= kBurstGapSeconds)
            --first;
        burst.assign(events_.begin() + (std::ptrdiff_t) first, events_.end());
        events_.clear();
        noteOns_.store(0);
    }
    // a passage that starts with releases of notes held before it only has the releases dropped below
    const double secondsPerBeat = 60.0 / std::max(20.0, tempo);
    const bool onTimeline = std::all_of(burst.begin(), burst.end(), [](const Event& e) { return e.playing; });
    const double firstSeconds = burst.front().seconds, firstBeat = burst.front().beats;
    auto beatOf = [&](const Event& e) { return onTimeline ? e.beats : (e.seconds - firstSeconds) / secondsPerBeat; };

    Result result;
    result.wasPlaying = onTimeline;
    const double bar = std::max(0.25, beatsPerBar);
    // timeline passages start on the bar the first note falls in; free playing starts at the first note
    result.startBeats = onTimeline ? std::floor(std::max(0.0, firstBeat) / bar) * bar : 0.0;
    const double offset = result.startBeats;

    struct Open { double start; float velocity; };
    std::map<std::pair<int, int>, std::vector<Open>> open; // (channel, note) -> sounding starts
    double last = 0.0;
    for (const auto& e : burst)
    {
        const double beat = beatOf(e) - offset;
        last = std::max(last, beat);
        const auto key = std::make_pair(e.channel, e.note);
        if (e.velocity > 0.0f)
            open[key].push_back({beat, e.velocity});
        else if (auto it = open.find(key); it != open.end() && ! it->second.empty())
        {
            const Open started = it->second.front();
            it->second.erase(it->second.begin());
            model::MidiNote note;
            note.start = std::max(0.0, started.start);
            note.duration = std::max(0.0625, beat - started.start);
            note.pitch = e.note;
            note.velocity = std::clamp(started.velocity, 1.0f, 127.0f);
            result.notes.push_back(note);
        }
        // a release without a start (the note was held before the passage) is dropped
    }
    // notes still sounding when the button was pressed end at the capture
    const double endNow = onTimeline ? last : (now - firstSeconds) / secondsPerBeat - offset;
    for (const auto& [key, starts] : open)
        for (const auto& s : starts)
        {
            model::MidiNote note;
            note.start = std::max(0.0, s.start);
            note.duration = std::max(0.0625, std::max(endNow, s.start + 0.25) - s.start);
            note.pitch = key.second;
            note.velocity = std::clamp(s.velocity, 1.0f, 127.0f);
            result.notes.push_back(note);
            last = std::max(last, note.start + note.duration);
        }
    if (result.notes.empty())
        return std::nullopt;
    std::sort(result.notes.begin(), result.notes.end(), [](const model::MidiNote& a, const model::MidiNote& b) {
        if (a.start < b.start) return true;
        if (a.start > b.start) return false;
        return a.pitch < b.pitch;
    });
    double end = 0.0;
    for (const auto& n : result.notes) end = std::max(end, n.start + n.duration);
    result.length = std::max(bar, std::ceil(end / bar - 1e-9) * bar);
    return result;
}

} // namespace sonora::midi

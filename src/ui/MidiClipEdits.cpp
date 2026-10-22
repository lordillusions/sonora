#include "ui/MidiClipEdits.h"
#include "core/Scales.h"

#include <algorithm>
#include <cmath>
#include <random>
#include <set>
#include <numeric>

namespace sonora::ui::midiedit
{

namespace
{
bool inLoop(const model::Clip& clip, const model::MidiNote& n)
{
    return n.start >= clip.loop.loopStart - 1e-9 && n.start < clip.loop.loopEnd - 1e-9;
}
} // namespace

const char* toolName(Tool tool)
{
    static constexpr const char* names[] = {"Arpeggiate", "Chop", "Connect", "Ornament", "Quantize", "Recombine", "Span", "Strum", "Time Warp",
                                            "Humanize", "Fit to Scale", "Reverse", "Invert", "Legato", "Rhythm", "Seed", "Shape", "Stacks"};
    return names[std::clamp((int) tool, 0, 17)];
}
bool isGenerator(Tool tool) { return (int) tool >= (int) Tool::Rhythm; }

model::Clip applyTool(const model::Clip& source, const ToolSettings& settings, const std::vector<int>& indices, double start, double end)
{
    auto clip = source;
    if (! std::isfinite(start) || ! std::isfinite(end) || end <= start) return clip;
    const double rate = std::clamp(settings.rate, 0.03125, 16.0), amount = std::clamp(settings.amount, 0.0, 1.0);
    const int parts = std::clamp(settings.parts, 1, 64), low = std::clamp(settings.lowPitch, 0, 127), high = std::clamp(settings.highPitch, low, 127);
    std::mt19937 random(settings.seed);
    auto unit = [&] { return (double) random() / (double) std::mt19937::max(); };
    auto scalePitch = [&](int pitch) {
        pitch = std::clamp(pitch, 0, 127);
        if (! settings.scale) return pitch;
        auto fits = [&](int p) { return model::inScale(p, settings.root, settings.scaleName); };
        for (int delta = 0; delta < 12; ++delta)
        { if (pitch - delta >= 0 && fits(pitch - delta)) return pitch - delta; if (pitch + delta <= 127 && fits(pitch + delta)) return pitch + delta; }
        return pitch;
    };
    auto generatedPitch = [&](int pitch) {
        pitch = std::clamp(pitch, low, high);
        if (! settings.scale) return pitch;
        for (int delta = 0; delta < 128; ++delta)
        {
            if (pitch - delta >= low && scalePitch(pitch - delta) == pitch - delta) return pitch - delta;
            if (pitch + delta <= high && scalePitch(pitch + delta) == pitch + delta) return pitch + delta;
        }
        return pitch;
    };
    std::set<int> chosen(indices.begin(), indices.end());
    std::vector<model::MidiNote> selected;
    for (size_t i = 0; i < source.notes.size(); ++i)
        if (chosen.count((int) i)) selected.push_back(source.notes[i]);
    std::stable_sort(selected.begin(), selected.end(), [](const auto& a, const auto& b) { return a.start < b.start || (std::abs(a.start - b.start) < 1e-9 && a.pitch < b.pitch); });
    std::vector<model::MidiNote> result;
    constexpr size_t limit = 65536;
    auto append = [&](model::MidiNote note, bool fresh = false) {
        if (result.size() >= limit || ! std::isfinite(note.start) || ! std::isfinite(note.duration)) return;
        note.start = std::max(0.0, note.start); note.duration = std::max(0.001, note.duration); note.pitch = std::clamp(note.pitch, 0, 127);
        note.velocity = std::clamp(note.velocity, 1.0f, 127.0f);
        if (fresh) note.noteId = -1;
        result.push_back(note);
    };
    if (isGenerator(settings.tool))
    {
        const int steps = std::min(16384, (int) std::ceil((end - start) / rate));
        for (int i = 0; i < steps && result.size() < limit; ++i)
        {
            const double beat = start + (double) i * rate;
            if (settings.tool != Tool::Stacks && unit() > std::clamp(settings.density, 0.0, 1.0)) continue;
            model::MidiNote note; note.start = beat; note.duration = std::min(end - beat, rate * std::clamp(settings.gate, 0.05, 2.0));
            if (settings.tool == Tool::Rhythm) note.pitch = generatedPitch(low);
            else if (settings.tool == Tool::Seed) note.pitch = generatedPitch(low + (int) (unit() * (double) (high - low + 1)));
            else if (settings.tool == Tool::Shape) note.pitch = generatedPitch(low + (int) std::lround((high - low) * (0.5 - 0.5 * std::cos(2.0 * 3.141592653589793 * i / std::max(1, steps - 1)))));
            else
            {
                note.pitch = generatedPitch(low + (i % 4 == 0 ? 0 : i % 4 == 1 ? 5 : i % 4 == 2 ? 7 : 3));
                note.duration = std::min(end - beat, rate * std::clamp(settings.gate, 0.05, 2.0));
                const int base = note.pitch;
                std::set<int> pitches;
                for (int voice = 0; voice < std::min(parts, 8); ++voice)
                {
                    // Stacked thirds: scale steps when fitting to the scale, major thirds otherwise.
                    const int stacked = settings.scale ? model::transposeInScale(base, 2 * voice, settings.root, model::scaleAt(settings.scaleName))
                                                       : base + voice * 4;
                    note.pitch = generatedPitch(std::min(high, stacked)); if (pitches.insert(note.pitch).second) append(note, true);
                }
                continue;
            }
            note.velocity = settings.tool == Tool::Seed ? (float) (65.0 + unit() * 55.0) : (i % parts == 0 ? 110.0f : 90.0f);
            append(note, true);
        }
        std::erase_if(clip.notes, [&](const auto& note) { return note.start >= start && note.start < end; });
    }
    else
    {
        if (selected.empty()) return clip;
        int lowest = 127, highest = 0;
        for (const auto& note : selected) { lowest = std::min(lowest, note.pitch); highest = std::max(highest, note.pitch); }
        if (settings.tool == Tool::Arpeggiate)
        {
            const int steps = std::min(16384, (int) std::ceil((end - start) / rate));
            for (int step = 0; step < steps; ++step)
            {
                const double beat = start + step * rate;
                std::vector<model::MidiNote> sounding;
                for (const auto& note : selected) if (note.start <= beat + 1e-9 && note.start + note.duration > beat + 1e-9) sounding.push_back(note);
                if (sounding.empty()) continue;
                std::sort(sounding.begin(), sounding.end(), [](const auto& a, const auto& b) { return a.pitch < b.pitch; });
                auto note = sounding[(size_t) step % sounding.size()]; note.start = beat;
                note.duration = std::min(end - beat, rate * std::clamp(settings.gate, 0.05, 2.0)); append(note, true);
            }
        }
        else for (size_t i = 0; i < selected.size() && result.size() < limit; ++i)
        {
            auto note = selected[i];
            const auto next = std::find_if(selected.begin() + (long) i + 1, selected.end(), [&](const auto& other) { return other.start > note.start + 1e-9; });
            switch (settings.tool)
            {
                case Tool::Chop:
                    for (int j = 0; j < parts; ++j)
                    { auto cut = note; cut.start += j * note.duration / parts; cut.duration = note.duration / parts * std::clamp(settings.gate, 0.05, 1.0); append(cut, true); }
                    continue;
                case Tool::Connect:
                    if (next != selected.end()) for (int j = 1; j < parts; ++j)
                    { auto bridge = note; bridge.start += (next->start - note.start) * j / parts; bridge.pitch = scalePitch((int) std::lround(note.pitch + (next->pitch - note.pitch) * (double) j / parts)); bridge.duration = (next->start - note.start) / parts * 0.8; append(bridge, true); }
                    break;
                case Tool::Ornament:
                {
                    auto grace = note; grace.duration = std::min(rate * 0.25, note.duration * 0.25); grace.velocity *= 0.65f;
                    append(grace, true); note.start += grace.duration; note.duration -= grace.duration; break;
                }
                case Tool::Quantize: note.start += (std::round(note.start / rate) * rate - note.start) * amount; break;
                case Tool::Recombine: note.pitch = selected[(i + (size_t) parts) % selected.size()].pitch; break;
                case Tool::Span: note.duration = std::max(0.001, std::min(rate * settings.gate, end - note.start)); break;
                case Tool::Legato: note.duration = std::max(0.001, (next != selected.end() ? next->start : end) - note.start); break;
                case Tool::Strum: note.start = std::clamp(note.start + rate * amount * (note.pitch - lowest) / std::max(1, highest - lowest), start, end - 0.001); break;
                case Tool::TimeWarp: note.start = start + (end - start) * std::pow(std::clamp((note.start - start) / (end - start), 0.0, 1.0), 1.0 + amount * 3); break;
                case Tool::Humanize: note.start = std::clamp(note.start + (unit() - 0.5) * rate * amount, start, end - 0.001); note.velocity += (float) ((unit() - 0.5) * 30 * amount); break;
                case Tool::FitScale: note.pitch = scalePitch(note.pitch); break;
                case Tool::Reverse: note.start = std::clamp(start + end - note.start - note.duration, start, end - 0.001); break;
                case Tool::Invert: note.pitch = lowest + highest - note.pitch; break;
                case Tool::Arpeggiate: case Tool::Rhythm: case Tool::Seed: case Tool::Shape: case Tool::Stacks: break;
            }
            append(note);
        }
        clip.notes.clear();
        for (size_t i = 0; i < source.notes.size(); ++i) if (! chosen.count((int) i)) clip.notes.push_back(source.notes[i]);
    }
    clip.notes.insert(clip.notes.end(), result.begin(), result.end());
    std::stable_sort(clip.notes.begin(), clip.notes.end(), [](const auto& a, const auto& b) { return a.start < b.start || (std::abs(a.start - b.start) < 1e-9 && a.pitch < b.pitch); });
    return clip;
}

model::Clip reverse(const model::Clip& clip)
{
    auto c = clip;
    const double a = c.loop.loopStart, b = c.loop.loopEnd;
    for (auto& n : c.notes)
        if (inLoop(clip, n))
            n.start = std::clamp(a + b - (n.start + n.duration), a, std::max(a, b - 1e-6));
    return c;
}

model::Clip invert(const model::Clip& clip)
{
    auto c = clip;
    int low = 127, high = 0;
    for (const auto& n : c.notes)
        if (inLoop(clip, n))
        {
            low = std::min(low, n.pitch);
            high = std::max(high, n.pitch);
        }
    if (low > high)
        return c;
    for (auto& n : c.notes)
        if (inLoop(clip, n))
            n.pitch = low + high - n.pitch;
    return c;
}

model::Clip legato(const model::Clip& clip)
{
    auto c = clip;
    std::vector<double> starts;
    for (const auto& n : c.notes)
        if (inLoop(clip, n))
            starts.push_back(n.start);
    std::sort(starts.begin(), starts.end());
    for (auto& n : c.notes)
    {
        if (! inLoop(clip, n))
            continue;
        const auto next = std::upper_bound(starts.begin(), starts.end(), n.start + 1e-9);
        if (next != starts.end())
            n.duration = *next - n.start;
    }
    return c;
}

model::Clip duplicateLoop(const model::Clip& clip)
{
    auto c = clip;
    const double length = c.loop.loopEnd - c.loop.loopStart;
    if (length <= 0)
        return c;
    for (const auto& n : clip.notes)
        if (inLoop(clip, n))
        {
            auto copy = n;
            copy.start += length;
            copy.noteId = -1;
            c.notes.push_back(copy);
        }
    c.loop.loopEnd += length;
    c.currentEnd = std::max(c.currentEnd, c.currentStart) + length; // the clip's visible length grows with the loop
    return c;
}

model::Clip stretch(const model::Clip& clip, double factor)
{
    auto c = clip;
    if (factor <= 0)
        return c;
    for (auto& n : c.notes)
    {
        n.start *= factor;
        n.duration *= factor;
    }
    c.loop.loopStart *= factor;
    c.loop.loopEnd *= factor;
    c.loop.startRelative *= factor;
    c.currentEnd = c.currentStart + (c.currentEnd - c.currentStart) * factor;
    return c;
}

} // namespace sonora::ui::midiedit

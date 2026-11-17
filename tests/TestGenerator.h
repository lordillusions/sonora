#pragma once
// PLG-25a: a test-only MidiGenerator with a fixed pattern, used by the foundation's tests (engine timing, state, clip
// commands, the VST3 MIDI bridge, the clip menu). Not a product device.
//
// Every step of Rate whose bit is on in the 16-step pattern (non-parameter state, through PatternSlot) plays the held
// note number (step mod held count), or C3 (48) when nothing is held and Fixed is on. A "walk" adds 0, 1, 2 semitones in
// turn to the fired steps of each 32-step cycle: state with history, rebuilt in located(), as a real scale walk would be.
// Burst plays that many extra notes a semitone apart on every step (the MIDI cap test).

#include "core/PatternGrid.h"
#include "core/PatternRandom.h"
#include "devices/MidiGenerator.h"
#include "devices/PatternSlot.h"

#include <array>
#include <memory>

namespace sonora::testkit
{
struct TestPattern
{
    std::array<uint8_t, 16> on{1, 1, 0, 1, 1, 0, 1, 1, 1, 1, 0, 1, 0, 1, 1, 1};
    int8_t transpose = 0;
};

class TestGenerator final : public devices::MidiGenerator
{
public:
    enum Param { Rate, Gate, Seed, Latch, Fixed, Burst, Swing, NumParams };
    static constexpr int kCycleSteps = 32;

    TestGenerator()
        : MidiGenerator({devices::makeParam("Rate", "Rate", 0, 12, model::kRate1_16, "int"),
                         devices::makeParam("Gate", "Gate", 5, 400, 50, "int"),
                         devices::makeParam("Seed", "Seed", 1, 999, 7, "int"),
                         devices::makeParam("Latch", "Latch", 0, 1, 0, "", true),
                         devices::makeParam("Fixed", "Fixed", 0, 1, 0, "", true),
                         devices::makeParam("Burst", "Burst", 0, 127, 0, "int"),
                         devices::makeParam("Swing", "Swing", 0, 1, 0, "%")})
    {
    }
    std::string name() const override { return "Test Generator"; }
    std::string stateFormat() const override { return "sonora-test-generator-v1"; }
    std::vector<uint8_t> saveState() const override
    {
        devices::StateWriter w;
        for (auto bit : pattern.edited().on) w.u8(bit);
        w.i32(pattern.edited().transpose);
        return w.take();
    }
    bool loadState(const uint8_t* data, size_t size, std::string& error) override
    {
        devices::StateReader r(data, size);
        TestPattern p;
        for (auto& bit : p.on) bit = r.u8();
        p.transpose = (int8_t) r.i32();
        if (! r.ok()) { error = "truncated"; return false; }
        pattern.set(p);
        return true;
    }
    std::unique_ptr<devices::MidiGenerator> createEmpty() const override { return std::make_unique<TestGenerator>(); }

    devices::PatternSlot<TestPattern> pattern;
    int locates = 0;
    double lastLocate = -1;

protected:
    void blockStarted() override { pattern.update(); }
    bool latchOn() const override { return value(Latch) > 0.5f; }
    bool runsWhenStopped() const override { return heldCount() > 0 || value(Fixed) > 0.5f; }
    double step() const { return model::grid::rateBeats((int) std::lround(value(Rate))); }
    bool fires(int64_t i) const { return pattern.live().on[(size_t) (((i % 16) + 16) % 16)] != 0; }
    void located(double beat) override
    {
        ++locates;
        lastLocate = beat;
        const int64_t first = model::grid::indexAt(beat, step());
        const int64_t cycle = first - (((first % kCycleSteps) + kCycleSteps) % kCycleSteps);
        walk_ = 0;
        // Replay the steps of this cycle that start before `beat`; the first one at or after it is generated next.
        int64_t i = cycle;
        for (; model::grid::stepBeat(i, step(), value(Swing)) < beat - 1e-9; ++i)
            if (fires(i)) ++walk_;
        next_ = i;
    }
    void generate(double from, double to, Sink& sink) override
    {
        const double length = step();
        const double swing = value(Swing);
        const auto range = model::grid::stepsIn(from, to, length);
        const auto& p = pattern.live();
        for (int64_t i = range.first; i < range.end; ++i)
        {
            const double beat = model::grid::stepBeat(i, length, swing);
            if (beat < from || beat >= to) continue;
            if (i < next_) continue; // already played (a step swung across a span boundary)
            next_ = i + 1;
            if (((i % kCycleSteps) + kCycleSteps) % kCycleSteps == 0) walk_ = 0;
            if (! fires(i)) continue;
            const int offset = walk_ % 3;
            ++walk_;
            int pitch = -1;
            if (heldCount() > 0) pitch = held((int) (((i % heldCount()) + heldCount()) % heldCount())).pitch;
            else if (value(Fixed) > 0.5f) pitch = 48;
            if (pitch < 0) continue;
            pitch += p.transpose + offset;
            const uint32_t seed = (uint32_t) std::lround(value(Seed));
            const int velocity = model::PatternRandom::range(60, 120, seed, i);
            const double gate = length * value(Gate) / 100.0;
            sink.note(beat, gate, pitch, velocity);
            for (int b = 1; b <= (int) std::lround(value(Burst)); ++b) sink.note(beat, gate, std::min(127, pitch + b), velocity);
        }
    }

private:
    int walk_ = 0;
    int64_t next_ = 0;
};
} // namespace sonora::testkit

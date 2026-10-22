#pragma once
// Simple polyphonic sine synth used when a track's instrument cannot be loaded
// (unimplemented native instrument, missing plugin). Lets you hear MIDI parts while
// developing. Shown in the UI with a "placeholder" badge. Can be disabled in settings.

#include "devices/DeviceProcessor.h"

#include <array>

namespace sonora::devices
{

class PlaceholderSynth final : public DeviceProcessor
{
public:
    explicit PlaceholderSynth(std::string replacing) : replacing_(std::move(replacing)) {}

    std::string name() const override { return replacing_ + " (placeholder)"; }
    bool isInstrument() const override { return true; }

    void prepare(double sampleRate, int) override { sampleRate_ = sampleRate; reset(); }
    void reset() override { voices_ = {}; sustainPedal_ = false; bendRatio_ = 1; }

    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override
    {
        if (isBypassed())
            return;
        int pos = 0;
        for (const auto meta : midi)
        {
            render(audio, pos, meta.samplePosition);
            pos = meta.samplePosition;
            if (meta.numBytes > 3) continue; // unsupported SysEx; never construct an allocating MidiMessage here
            const auto m = meta.getMessage();
            if (m.isNoteOn())
                noteOn(m.getNoteNumber(), m.getFloatVelocity());
            else if (m.isNoteOff())
                noteOff(m.getNoteNumber());
            else if (m.isController() && m.getControllerNumber() == 64)
            {
                sustainPedal_ = m.getControllerValue() >= 64;
                if (! sustainPedal_) for (auto& v : voices_) if (! v.keyDown) v.releasing = true;
            }
            else if (m.isPitchWheel())
                bendRatio_ = std::pow(2.0, (m.getPitchWheelValue() - 8192) / (8192.0 * 6.0)); // fallback +/-2 semitones
            else if (m.isResetAllControllers())
            {
                bendRatio_ = 1; sustainPedal_ = false;
                for (auto& v : voices_) if (! v.keyDown) v.releasing = true;
            }
            else if (m.isAllNotesOff() || m.isAllSoundOff())
                for (auto& v : voices_)
                    v.releasing = true;
        }
        render(audio, pos, audio.getNumSamples());
    }

private:
    struct Voice
    {
        int note = -1;
        double phase = 0, inc = 0;
        float amp = 0, target = 0;
        bool releasing = false, keyDown = true;
    };

    void noteOn(int note, float vel)
    {
        Voice* v = &voices_[0];
        for (auto& cand : voices_)
            if (cand.note < 0) { v = &cand; break; }
        v->note = note;
        v->phase = 0;
        v->inc = juce::MathConstants<double>::twoPi * juce::MidiMessage::getMidiNoteInHertz(note) / sampleRate_;
        v->target = 0.15f * vel;
        v->amp = 0;
        v->releasing = false; v->keyDown = true;
    }

    void noteOff(int note)
    {
        for (auto& v : voices_)
            if (v.note == note && v.keyDown)
            {
                v.keyDown = false;
                if (! sustainPedal_) v.releasing = true;
            }
    }

    void render(juce::AudioBuffer<float>& audio, int start, int end)
    {
        float* l = audio.getWritePointer(0);
        float* r = audio.getWritePointer(1);
        for (auto& v : voices_)
        {
            if (v.note < 0)
                continue;
            for (int i = start; i < end; ++i)
            {
                const float goal = v.releasing ? 0.0f : v.target;
                v.amp += (goal - v.amp) * (v.releasing ? 0.0015f : 0.01f);
                const float s = (float) std::sin(v.phase) * v.amp;
                v.phase += v.inc * bendRatio_;
                l[i] += s;
                r[i] += s;
            }
            if (v.releasing && v.amp < 1e-4f)
                v.note = -1;
        }
    }

    std::string replacing_;
    double sampleRate_ = 44100.0, bendRatio_ = 1;
    bool sustainPedal_ = false;
    std::array<Voice, 16> voices_{};
};

} // namespace sonora::devices

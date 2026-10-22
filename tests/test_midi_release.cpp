#include "TestFramework.h"

#include "devices/MidiReleaseFilter.h"
#include "engine/ClipPlayers.h"

using namespace sonora;

namespace
{
// The engine's release at a loop wrap / locate: All Notes Off, pedal-ups and centred bend on every channel.
void addReleaseBurst(juce::MidiBuffer& midi, int sample)
{
    for (int channel = 1; channel <= 16; ++channel)
        midi.addEvent(juce::MidiMessage::allNotesOff(channel), sample);
    engine::releaseMidiControllers(midi, sample);
}

struct Counts
{
    int allNotesOff = 0, noteOffs = 0, noteOns = 0, pedals = 0, bends = 0, total = 0;
};

Counts count(const juce::MidiBuffer& midi)
{
    Counts c;
    for (const auto event : midi)
    {
        const auto m = event.getMessage();
        ++c.total;
        if (m.isAllNotesOff()) ++c.allNotesOff;
        else if (m.isNoteOn()) ++c.noteOns;
        else if (m.isNoteOff()) ++c.noteOffs;
        else if (m.isController()) ++c.pedals;
        else if (m.isPitchWheel()) ++c.bends;
    }
    return c;
}
} // namespace

TEST_CASE("midi release: a loop-wrap burst becomes note-offs for the held notes only")
{
    devices::MidiReleaseFilter filter;
    filter.prepare();

    juce::MidiBuffer first;
    first.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 10);
    first.addEvent(juce::MidiMessage::noteOn(1, 72, (juce::uint8) 100), 20);
    first.addEvent(juce::MidiMessage::noteOff(1, 72), 30);
    filter.process(first, true);
    CHECK(filter.anyNoteHeld());

    juce::MidiBuffer wrap;
    wrap.addEvent(juce::MidiMessage::noteOn(1, 64, (juce::uint8) 90), 5); // before the wrap: still held at the wrap
    addReleaseBurst(wrap, 100);
    wrap.addEvent(juce::MidiMessage::noteOn(1, 62, (juce::uint8) 100), 100); // first note after the wrap
    filter.process(wrap, true);

    const auto c = count(wrap);
    CHECK(c.allNotesOff == 0);
    CHECK(c.pedals == 0); // pedals were never pressed: the pedal-ups change nothing
    CHECK(c.bends == 0);
    CHECK(c.noteOffs == 2); // 60 and 64, not 72 (already released)
    CHECK(c.noteOns == 2);

    // Order is kept: the note-offs land at the wrap, before the new note at the same sample.
    std::vector<std::pair<int, int>> order; // (sample, note), note-offs negative
    for (const auto event : wrap)
    {
        const auto m = event.getMessage();
        order.emplace_back(event.samplePosition, m.isNoteOn() ? m.getNoteNumber() : -m.getNoteNumber());
    }
    REQUIRE(order.size() == 4);
    CHECK(order[0] == std::make_pair(5, 64));
    CHECK(order[1].first == 100 && order[1].second < 0);
    CHECK(order[2].first == 100 && order[2].second < 0);
    CHECK(order[3] == std::make_pair(100, 62));
}

TEST_CASE("midi release: nothing held means the burst disappears")
{
    devices::MidiReleaseFilter filter;
    filter.prepare();
    juce::MidiBuffer midi;
    addReleaseBurst(midi, 0);
    filter.process(midi, true);
    CHECK(midi.isEmpty());
    CHECK(! filter.anyNoteHeld());
}

TEST_CASE("midi release: pressed pedals and bent channels still get their reset")
{
    devices::MidiReleaseFilter filter;
    filter.prepare();
    juce::MidiBuffer before;
    before.addEvent(juce::MidiMessage::controllerEvent(1, 64, 127), 0);
    before.addEvent(juce::MidiMessage::pitchWheel(2, 10000), 0);
    before.addEvent(juce::MidiMessage::noteOn(3, 40, (juce::uint8) 80), 0);
    before.addEvent(juce::MidiMessage::noteOn(3, 40, (juce::uint8) 0), 1); // velocity-0 note-on is a note-off
    filter.process(before, true);
    CHECK(! filter.anyNoteHeld());

    juce::MidiBuffer wrap;
    addReleaseBurst(wrap, 50);
    filter.process(wrap, true);
    const auto c = count(wrap);
    CHECK(c.total == 2);
    CHECK(c.pedals == 1); // sustain up on channel 1
    CHECK(c.bends == 1);  // centre on channel 2

    // Released now: a second burst carries nothing.
    juce::MidiBuffer again;
    addReleaseBurst(again, 0);
    filter.process(again, true);
    CHECK(again.isEmpty());
}

TEST_CASE("midi release: stopped transport keeps the real panic")
{
    devices::MidiReleaseFilter filter;
    filter.prepare();
    juce::MidiBuffer notes;
    notes.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
    filter.process(notes, true);

    juce::MidiBuffer stop;
    addReleaseBurst(stop, 0);
    const int before = stop.getNumEvents();
    filter.process(stop, false);
    CHECK(stop.getNumEvents() == before);
    CHECK(count(stop).allNotesOff == 16);
    CHECK(! filter.anyNoteHeld()); // the panic itself released the tracked notes
}

TEST_CASE("midi release: unprepared filter never rewrites (no allocation on the audio thread)")
{
    devices::MidiReleaseFilter filter;
    juce::MidiBuffer midi;
    addReleaseBurst(midi, 0);
    const int before = midi.getNumEvents();
    filter.process(midi, true);
    CHECK(midi.getNumEvents() == before);
}

TEST_CASE("midi release: SysEx and other messages pass through untouched")
{
    devices::MidiReleaseFilter filter;
    filter.prepare();
    juce::MidiBuffer midi;
    const juce::uint8 sysex[] = {0xf0, 0x7e, 0x7f, 0x06, 0x01, 0xf7};
    midi.addEvent(sysex, (int) sizeof(sysex), 0);
    midi.addEvent(juce::MidiMessage::controllerEvent(1, 1, 64), 1); // mod wheel
    addReleaseBurst(midi, 2);
    filter.process(midi, true);
    CHECK(midi.getNumEvents() == 2);
    int sysexCount = 0;
    for (const auto event : midi)
        if (event.data[0] == 0xf0 && event.numBytes == (int) sizeof(sysex)) ++sysexCount;
    CHECK(sysexCount == 1);
}

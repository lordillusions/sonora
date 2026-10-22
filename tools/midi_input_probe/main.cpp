// ENG-9d / ENG-14: own virtual MIDI source for native recording interoperability.
// Creates no persistent hardware/configuration. The endpoint disappears when the tool exits.
#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_events/juce_events.h>
#include <cstdio>
#include <iostream>
#include <string>

int main()
{
    juce::ScopedJuceInitialiser_GUI init;
    auto output = juce::MidiOutput::createNewDevice("Sonora Verification Input");
    if (! output) { std::fprintf(stderr, "Could not create virtual MIDI source\n"); return 1; }
    std::printf("Virtual source ready: Sonora Verification Input. Enter sends a short pattern; long holds five beats; alt sends distinct CC values; q exits.\n");
    std::fflush(stdout);
    for (std::string line; std::getline(std::cin, line) && line != "q";)
    {
        const bool alternate = line == "alt";
        output->sendMessageNow(juce::MidiMessage::noteOn(1, 77, (juce::uint8) 100));
        output->sendMessageNow(juce::MidiMessage::controllerEvent(1, 1, alternate ? 64 : 110));
        output->sendMessageNow(juce::MidiMessage::controllerEvent(1, 64, 127));
        output->sendMessageNow(juce::MidiMessage::pitchWheel(1, 12288));
        juce::Thread::sleep(250);
        output->sendMessageNow(juce::MidiMessage::noteOff(1, 77));
        juce::Thread::sleep(line == "long" ? 2250 : 250);
        output->sendMessageNow(juce::MidiMessage::controllerEvent(1, 1, alternate ? 32 : 15));
        output->sendMessageNow(juce::MidiMessage::controllerEvent(1, 64, 0));
        output->sendMessageNow(juce::MidiMessage::pitchWheel(1, 8192));
        std::printf("Sent pattern (note 77, CC1 %d->%d, CC64 127->0, bend 4096->0); endpoint remains ready\n", alternate ? 64 : 110, alternate ? 32 : 15);
        std::fflush(stdout);
    }
    for (int channel = 1; channel <= 16; ++channel)
    {
        output->sendMessageNow(juce::MidiMessage::controllerEvent(channel, 64, 0));
        output->sendMessageNow(juce::MidiMessage::allNotesOff(channel));
    }
    return 0;
}

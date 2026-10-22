#pragma once
// MIDI controller support.
//
//  - Opens hardware MIDI inputs (CoreMIDI via JUCE). Each input has two roles like in Live:
//      Track  = notes/pitch bend/aftertouch go to the armed tracks,
//      Remote = CC/notes matching a mapping control the app (volume, pan, devices, transport).
//    Roles are remembered per device (saveSettings/loadSettings, MIDI-1).
//  - Track messages go through a lock-free FIFO to the audio thread with their arrival time,
//    and are placed at the matching sample offset inside the block (MIDI-5).
//  - Remote messages matching a mapping become ControlChange events on the message thread.
//  - MIDI learn: beginLearn(target), then move a knob / press a key.
//
// Mappings can be saved/loaded (MappingStore.h, MIDI-4) and imported from .als (MIDI-3).
// Not covered: control-surface scripts (docs/adr/0005-midi-controllers.md).

#include "midi/ControlTarget.h"
#include "midi/InputBlock.h"

#include <juce_audio_devices/juce_audio_devices.h>

#include <array>
#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace sonora::midi
{

struct InputRoles
{
    bool track = true;
    bool remote = true;
};

// Sample offset inside a block for a message that arrived at `timestamp` (seconds, JUCE
// hi-res clock). Messages older than the block start land on sample 0. Pure, unit tested.
int placeInBlock(double timestamp, double blockStartTime, int numSamples, double sampleRate);

class MidiManager : private juce::MidiInputCallback
{
public:
    MidiManager();
    ~MidiManager() override;

    // ---- devices (message thread)
    juce::Array<juce::MidiDeviceInfo> availableInputs() const;
    void setInputRoles(const juce::String& identifier, const juce::String& name, InputRoles roles);
    InputRoles inputRoles(const juce::String& identifier) const; // {false,false} when unknown/closed
    void enableAllInputs();
    // Simple on/off (both roles), kept for existing callers.
    void setInputEnabled(const juce::String& identifier, bool enabled);
    bool isInputEnabled(const juce::String& identifier) const;

    // Remembers roles across runs: {"inputs":[{"id","name","track","remote"}]}.
    bool saveSettings(const juce::File& file) const;
    // Applies saved roles to the devices that are present; unknown devices are kept for later.
    bool loadSettings(const juce::File& file);

    // ---- mappings + MIDI learn (message thread)
    void beginLearn(const ControlTarget& target);
    void cancelLearn();
    bool isLearning() const { return learning_.load(); }
    std::vector<Mapping> mappings() const;
    void setMappings(std::vector<Mapping> m);
    void addMapping(const Mapping& m); // replaces mappings with the same target or same source
    void removeMapping(const ControlTarget& target, bool keyboard = false);
    // Called (message thread) after the mapping list changed (learn, add, remove).
    std::function<void()> onMappingsChanged;

    // How callbacks reach the message thread (default: MessageManager::callAsync). Tests run them inline.
    std::function<void(std::function<void()>)> postToMessageThread = [](std::function<void()> f) {
        juce::MessageManager::callAsync(std::move(f));
    };

    // Called on the message thread when a mapped control moves.
    std::function<void(const ControlTarget&, const ControlChange&)> onControl;
    // Called on the message thread for every incoming message (activity LED in the UI).
    std::function<void()> onActivity;
    // MIDI-thread tap on every message that goes to the armed tracks (Capture MIDI). Must not block.
    std::function<void(const juce::MidiMessage&)> onTrackMessage;

    // ---- audio thread
    // Moves pending Track-role messages into `dest`, positioned inside the block by arrival time.
    void popForAudio(juce::MidiBuffer& dest, int numSamples, double sampleRate, InputBlock* sources = nullptr);

    // Entry point for the MIDI thread (also used by tests to simulate input).
    void handleMessage(const juce::MidiMessage& msg, bool trackRole, bool remoteRole, uint32_t port = 0);
    std::atomic<uint64_t> droppedInputEvents{0}; // FIFO or per-block byte/event capacity

private:
    void handleIncomingMidiMessage(juce::MidiInput*, const juce::MidiMessage&) override;
    void openOrCloseDevice(const juce::String& identifier);

    struct Slot
    {
        std::atomic<juce::MidiInput*> input{nullptr};
        std::atomic<bool> track{false}, remote{false};
        std::atomic<uint32_t> token{0};
    };
    static constexpr int kMaxInputs = 16;
    std::array<Slot, kMaxInputs> slots_;
    std::vector<std::unique_ptr<juce::MidiInput>> inputs_;
    struct Remembered
    {
        juce::String id, name;
        InputRoles roles;
    };
    std::vector<Remembered> remembered_;

    static constexpr int kFifoSize = 1024;
    juce::AbstractFifo fifo_{kFifoSize};
    std::array<juce::MidiMessage, kFifoSize> fifoData_;
    std::array<uint32_t, kFifoSize> fifoPorts_{};
    InputBlock scratchBlock_; // used by callers that do not request source metadata
    double lastPopTime_ = 0.0; // audio thread

    mutable std::mutex mappingMutex_;
    std::vector<Mapping> mappings_;
    std::atomic<bool> learning_{false};
    ControlTarget learnTarget_;
    struct RemoteHold { Mapping mapping; uint32_t port = 0; bool released = false; };
    std::vector<RemoteHold> remoteHolds_; // MIDI/message threads under mappingMutex_, never audio
    void releaseRemoteHolds(std::optional<uint32_t> port = std::nullopt, bool obsoleteOnly = false);
};

} // namespace sonora::midi

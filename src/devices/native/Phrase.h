#pragma once
// Sonora Phrase (PLG-25, docs/PHRASE.md): melody generator. A key, a scale and chords (a progression typed in, the held
// notes, or a progression shared from another Phrase through a link channel) drive one part: Arp, question / answer
// Stabs, Pad or Bass. The notes come from core/PhraseEngine (pure function of state, parameters, chords and song
// position); this device feeds it from the parameters, the PatternSlot state, the held notes and the harmony link.
//
// A MIDI effect in Sonora (catalog "SonoraPhrase", MIDI Effects > Generators); a VST3 instrument that sends MIDI
// (SonoraPhrase_VST3, code SnPh) with an audition voice in the plug-in wrapper.

#include "core/HarmonyLink.h"
#include "core/PhraseEngine.h"
#include "devices/MidiGenerator.h"
#include "devices/PatternSlot.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sonora::devices
{
class Phrase final : public MidiGenerator
{
public:
    enum Param
    {
        Key, Scale, Chords, LinkChannel, Part, Pattern, Density, Swing, Gate, Velocity, Accent, Octave, Shift, Humanize,
        KickGuard, Seed, Latch, Audition, NumParams
    };
    enum ChordsMode { FromProgression, FromMidi, FromLink };
    enum class LinkStatus { Off, Leading, Following, NoLeader };
    static constexpr const char* kStateFormat = "sonora-phrase-v1";

    explicit Phrase(model::HarmonyLink& link = model::HarmonyLink::shared());
    ~Phrase() override;
    std::string name() const override { return "Sonora Phrase"; }
    std::vector<UiGroup> uiGroups() const override; // the generic panel (the face replaces it where faces are shown)

    // ---- state (message thread): the working copy; publishState() hands it to the audio thread.
    const model::PhraseState& state() const { return slot_.edited(); }
    model::PhraseState& editState() { return slot_.edit(); }
    void publishState();
    void setState(const model::PhraseState&);
    // Dice / Answer / Mutate on the current part's slot (message thread; publish included).
    void dice(int slot, model::DiceKeep keep = model::DiceKeep::None);
    void answer(int from, int to, model::AnswerKind kind = model::AnswerKind::Answer);
    void mutate(int slot);
    model::PhrasePart part() const { return (model::PhrasePart) std::clamp((int) std::lround(getParameter(Part)), 0, 3); }

    // The performance values from the parameters (and the link when following).
    model::PhraseLive liveValues() const;
    // The chords as the device plays them now (message thread copy for faces and To Clip: the link and held chord as last
    // seen by the audio thread).
    LinkStatus linkStatus() const { return (LinkStatus) linkStatus_.load(std::memory_order_relaxed); }
    // Key / scale / progression in use (the leader's when following).
    int keyInUse() const;
    int scaleInUse() const;
    model::Progression progressionInUse() const;
    // The held chord the audio thread saw last (MIDI In); count 0 when none.
    model::ChordTones heldChord() const;

    // Plays `pitches` (up to 6, one beat long) once at the next block, also with the transport stopped: the chord
    // palette's audition (the track's instrument in Sonora, the audition voice in the VST3). Any thread.
    void audition(const int* pitches, int count);
    uint32_t stateVersion() const { return stateVersion_.load(std::memory_order_relaxed); }

    // ---- presets (PLG-25, devices/native/PhrasePresets.cpp)
    struct Preset
    {
        std::string category, name, description;
        model::PhrasePart part;
        std::vector<std::pair<int, float>> values; // parameters
        std::function<void(model::PhraseState&)> build; // writes patterns, form and (unless kept) the progression
    };
    static const std::vector<Preset>& presets();
    void applyPreset(int index); // message thread
    int matchingPreset() const;  // the preset the settings still equal, or -1

    // ---- MidiGenerator
    std::string stateFormat() const override { return kStateFormat; }
    std::vector<uint8_t> saveState() const override;
    bool loadState(const uint8_t* data, size_t size, std::string& error) override;
    std::unique_ptr<MidiGenerator> createEmpty() const override { return std::make_unique<Phrase>(link_); }
    bool listensToInput() const override { return std::lround(getParameter(Chords)) == FromMidi; }
    double clipLengthBeats() const override;

    static std::vector<uint8_t> encodeState(const model::PhraseState&);
    static bool decodeState(const uint8_t* data, size_t size, model::PhraseState& out, std::string& error);

protected:
    void generate(double fromBeat, double toBeat, Sink& sink) override;
    void located(double beat) override;
    void blockStarted() override;
    void heldChanged() override;
    bool latchOn() const override { return value(Latch) > 0.5f; }
    bool runsWhenStopped() const override { return heldCount() > 0 || auditionFresh_; }

private:
    void updateLink();

    model::HarmonyLink& link_;
    const uint64_t id_;
    PatternSlot<model::PhraseState> slot_;
    std::atomic<uint32_t> stateVersion_{1};
    int presetIndex_ = -1;
    std::shared_ptr<model::PhraseState> presetState_; // what applyPreset wrote (matchingPreset compares)
    std::vector<float> presetValues_;

    // audio thread
    uint32_t seenVersion_ = 0, publishedVersion_ = 0;
    int ledChannel_ = -1, lastKey_ = -1, lastScale_ = -1;
    bool superseded_ = false;
    model::HarmonyLinkPayload linkData_;
    bool following_ = false;
    model::ChordTones held_;
    double heldSince_ = 0;
    // shared with the message thread (faces): the link's chords and the held chord, published as copies
    mutable PatternSlot<model::HarmonyLinkPayload> linkSeen_; // audio thread writes, the message thread reads
    std::atomic<int> linkStatus_{0};
    std::atomic<uint64_t> heldPacked_{0};
    std::atomic<uint64_t> auditionRequest_{0}; // up to 6 pitches, 7 bits each, + count
    std::array<int, 6> auditionPitches_{};
    int auditionCount_ = 0;
    bool auditionFresh_ = false;
};
} // namespace sonora::devices

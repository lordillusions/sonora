#pragma once
// Sonora Seraph (PLG-27): a choir instrument. Wordless human voices from one singer to a full choir: men, women, mixed,
// boys and solo voices; chords or one legato line; slow chant phrases around each held note with parallel organum and a
// drone (ison); a trance gate; a cathedral. Pure synthesis: every singer is a glottal source through five formant
// resonators from published measurements of sung vowels (SeraphFormants.h), and no two singers are alike (tuning,
// timing, throat, vibrato, drift, jitter, shimmer, breath). docs/SERAPH.md is the full description.
//
//   notes -> [chant phrase player] -> singers (note x section x singers) -> sum -> [gate: pre] -> tone -> space
//         -> [gate: post] -> output
//
// Determinism: what a singer is comes from a hash of (seed, note, part, singer); the chant phrase and the gate are
// functions of the song position (a free clock from the first note when the transport is stopped).
// Audio thread: no allocation, no locks; the custom gate pattern is handed over through atomics.

#include "devices/NativeDevice.h"
#include "devices/native/SeraphVoice.h"
#include "devices/native/SerumHall.h"
#include "dsp/TranceGate.h"

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace sonora::devices
{
class Seraph final : public NativeDevice
{
public:
    enum Param
    {
        // choir
        Section, Singers, MenOctave, Fold, Mode,
        // mouth
        Vowel, Hum, Size, Bright, VowelMotion, MotionRate, VowelSteps, Onset,
        // voice
        Tension, Breath, Effort,
        // human
        Vibrato, VibratoRate, VibratoDelay, Drift, Detune, Timing, Scoop, Human, SizeSpread,
        // envelope
        Attack, Release, Swell, Glide, Dynamics,
        // chant
        Chant, Key, Scale, PhraseParam, ChantRate, Organum, Ison, IsonLevel,
        // gate
        Gate, GatePattern, GateRate, GateLength, GateSmooth, GateDepth, GateSwing, GatePosition,
        // tone, space, output
        LowCut, Air, Warmth, Space, SpaceSize, Distance, Width, Output, Seed,
        // PLG-27b: appended so every saved set and preset still loads (they sing Voices)
        Source,
        NumParams
    };

    enum SectionKind { Men, Women, Mixed, Boys, SoloBass, SoloTenor, SoloAlto, SoloSoprano, kSections };
    static constexpr const char* kSectionNames[kSections] = {"Men", "Women", "Mixed", "Boys", "Solo Bass", "Solo Tenor", "Solo Alto",
                                                             "Solo Soprano"};
    enum ModeKind { Poly, Legato };
    enum SourceKind { VoicesSource, RealSource };
    static constexpr int kVowelStepModes = 7;
    static constexpr const char* kVowelStepNames[kVowelStepModes] = {"Off", "A-O", "O-U", "A-E-O", "A-O-U", "A-E-I-O-U", "Random"};
    enum OnsetChoice { Soft, H, M, N, L, Y, W, Cycle, RandomOnset, kOnsetChoices };
    static constexpr const char* kOnsetNames[kOnsetChoices] = {"Soft", "H", "M", "N", "L", "Y", "W", "Cycle", "Random"};
    static constexpr int kChantRates = 4;
    static constexpr const char* kChantRateNames[kChantRates] = {"1/4 Bar", "1/2 Bar", "1 Bar", "2 Bars"};
    static constexpr double kChantRateBeats[kChantRates] = {1, 2, 4, 8};
    enum OrganumKind { OrganumOff, Fourth, Fifth, Octave, FifthOctave, OctaveBelow, kOrganums };
    static constexpr const char* kOrganumNames[kOrganums] = {"Off", "Fourth", "Fifth", "Octave", "Fifth + Octave", "Octave Below"};
    enum IsonKind { IsonOff, IsonRoot, IsonRootFifth, kIsons };
    static constexpr const char* kIsonNames[kIsons] = {"Off", "Root", "Root + Fifth"};
    enum GatePositionKind { BeforeSpace, AfterSpace };
    static constexpr int kSpaceSizes = 5;
    static constexpr const char* kSpaceSizeNames[kSpaceSizes] = {"Room", "Chapel", "Church", "Cathedral", "Endless"};
    static constexpr float kSpaceSeconds[kSpaceSizes] = {1.2f, 2.5f, 4.5f, 8.0f, 20.0f};
    static constexpr const char* kKeyNames[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

    static constexpr int kMaxNotes = 16;
    static constexpr int kMaxSingers = 64;
    static constexpr int kMaxSingersPerNote = 8;

    // ---- chant phrases: scale-step offsets from the tonic, a length in units each
    struct Phrase
    {
        const char* name;
        int count;
        int offset[8];
        int length[8];
        int totalUnits() const
        {
            int t = 0;
            for (int i = 0; i < count; ++i) t += length[i];
            return t;
        }
    };
    static const std::vector<Phrase>& phrases();
    // The phrase step sounding at `units` (phrase units from the cycle start) and its absolute index (cycle * count + step).
    static void phraseStepAt(const Phrase&, double units, int& step, long long& absoluteStep);
    // The pitch the chant sings for a held note at `units`: the note moved to the nearest scale note, then the step offset.
    static int chantPitch(int heldNote, const Phrase&, int step, int key, int scale);
    static int snapToScale(int note, int key, int scale);

    // ---- presets
    struct Preset
    {
        const char* name;
        const char* category;
        const char* description;
        std::vector<std::pair<int, float>> values; // every parameter not listed takes its default
        std::vector<float> gate;                   // a custom gate pattern (empty: none)
        std::string pack = {};                     // PLG-27h: "Men", "Women" or "All": the Seraph Voices pack singers it loads
    };
    static const std::vector<Preset>& presets();
    static const std::vector<std::string>& presetCategories();
    int matchingPreset() const; // the preset the current settings are, or -1
    void applyPreset(int index);  // message thread
    // Name of the last preset applied or restored ("" when none). Message thread.
    const std::string& presetName() const { return presetName_; }
    void setPresetName(std::string name) { presetName_ = std::move(name); }

    // ---- the custom gate pattern (Gate Pattern = Custom). Any thread; the audio thread reads it every block.
    static constexpr int kCustomPattern = dsp::TranceGate::kFactoryPatterns; // the last choice of Gate Pattern
    void setCustomGate(const float* levels, int steps);
    void setCustomGateStep(int step, float level);
    int customGateSteps() const { return customSteps_.load(std::memory_order_relaxed); }
    float customGateLevel(int step) const;
    // The pattern the gate plays now (factory or custom), for the face.
    int gateSteps() const;
    float gateLevel(int step) const;

    // ---- Real voices (PLG-27b). Message thread: files are read and analysed here (cached by path and date), then handed
    // to the audio thread whole. With Source = Real and nothing loaded, the singers keep the synthesis.
    static constexpr int kMaxRealVoices = 64;
    bool addRealFile(const std::string& path, std::string& error);  // false + the reason (unreadable, no stable pitch)
    // Several files, analysed in parallel first (a pack singer, a preset, a reopened set); the reasons of the refused.
    int addRealFiles(const std::vector<std::string>& paths, std::vector<std::string>* errors = nullptr);
    void addRealVoice(std::shared_ptr<const seraph::RealVoice>);   // already analysed (tests, packs)
    void removeRealVoice(int index);
    void clearRealVoices();
    std::vector<std::shared_ptr<const seraph::RealVoice>> realVoices() const { return realVoices_; }
    std::vector<std::string> realPaths() const;
    // Reads and analyses a file (cached). Null + the reason when it cannot be a Real voice.
    static std::shared_ptr<const seraph::RealVoice> analyseFile(const std::string& path, std::string& error);
    // How far (semitones) the last note started was from the recording that sang it; -1 none.
    float realDistance() const { return realDistance_.load(std::memory_order_relaxed); }
    bool realActive() const; // Source = Real with voices loaded

    // ---- the Seraph Voices pack (PLG-27h): recorded singers from VocalSet (CC BY 4.0), an optional sample pack. Its
    // singers are folders of sung notes ("Female 2/Female 2 ah A4.flac"); ATTRIBUTION.txt's first line is the credit.
    static constexpr const char* kPackId = "sonora-seraph-voices";
    static constexpr const char* kPackCredit =
        "\"VocalSet: A Singing Voice Dataset\" by Julia Wilkins, Prem Seetharaman, Alison Wahl and Bryan Pardo "
        "(https://doi.org/10.5281/zenodo.1193957), licensed under CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/). "
        "A selection of long tones, trimmed and levelled, in Sonora's Seraph Voices pack.";
    struct PackSinger
    {
        std::string name;               // "Female 2"
        std::vector<std::string> files; // its sung notes
    };
    struct PackVoices
    {
        std::string folder, credit;     // empty folder: not installed
        std::vector<PackSinger> singers;
    };
    // Looks in `packsRoot` (empty: SONORA_PACKS_ROOT, else ~/Music/Sonora/Packs). Message thread.
    static PackVoices findPackVoices(const std::string& packsRoot = {});
    // Loads a pack singer's notes as Real voices (Source = Real). False when the pack or the singer is missing.
    bool loadPackSinger(const std::string& singer, const std::string& packsRoot = {});
    // Whether any Real voice loaded now comes from the pack (the face shows the credit then).
    bool packVoicesLoaded(const std::string& packsRoot = {}) const;
    // The last preset that wanted the pack while it is not installed ("" when none): the face offers the download.
    const std::string& packWanted() const { return packWanted_; }
    std::string packsRoot; // where applyPreset looks for the pack (the app sets its Packs folder; empty: the default)

    // ---- native content: the custom gate and the preset name (app: model::NativeContent; VST3: in its state)
    static constexpr const char* kContentFormat = "SonoraSeraph/State";
    std::string encodeContent() const;           // "preset <name>", "gate <steps> <levels...>" and "real <path>" lines
    bool decodeContent(const std::string& text); // false when it holds neither
    std::vector<std::string> realErrors;         // why Real files in the content could not be loaded (the last decode)
    // The same state as VST3 state attributes: "gate", "preset", "real0", "real1"... (message thread).
    std::map<std::string, std::string> stateAttributes() const;
    void applyStateAttributes(const std::map<std::string, std::string>&);
    std::string gateText() const;                // "<steps> <level> <level> ..."
    bool setGateText(const std::string& text);

    Seraph();
    ~Seraph() override;
    std::string name() const override { return "Sonora Seraph"; }
    bool isInstrument() const override { return true; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void setHostInfo(const HostInfo& info) override { host_ = info; }
    std::vector<UiGroup> uiGroups() const override;

    // ---- face and tests (any thread, relaxed)
    struct SingerView
    {
        float pan = 0, level = 0;
        float pitch = 0; // the MIDI note the singer is going to (fractional; glides start from the previous one)
        float hz = 0;    // its fundamental now
        int type = -1;   // -1 idle, 0..3 bass..soprano, 4 boys
        const seraph::RealVoice* real = nullptr; // Real: the recording it sings (compare only; owned by the device)
    };
    SingerView singer(int index) const;
    // Real: where singer `index` reads its recording (samples). Tests, between blocks only.
    double singerRealPosition(int index) const
    {
        return index >= 0 && index < kMaxSingers ? voices_[(size_t) index].realPosition() : 0.0;
    }
    int activeSingers() const { return activeSingers_.load(std::memory_order_relaxed); }
    float lowestHz() const { return lowestHz_.load(std::memory_order_relaxed); } // the lowest sounding fundamental, 0 = none
    int gateStepNow() const { return gateStep_.load(std::memory_order_relaxed); } // -1 when not gating
    int chantStepNow() const { return chantStep_.load(std::memory_order_relaxed); } // -1 when no chant
    // The section's voice type for each singer of a note at `pitch` (tests): the types in singer order.
    std::vector<int> typesFor(int pitch) const;

private:
    struct Part
    {
        int offset = 0;     // semitones from the slot's line (organum, men's octave)
        int fold = 0;       // octave shift that brings the part into a range (Fold)
    };
    struct Slot
    {
        bool used = false, held = false, sustained = false, ison = false;
        int key = -1;          // the MIDI note (ison: -1)
        float velocity = 0.8f;
        unsigned long long order = 0, releasedAt = 0;
        int line = 60;         // the pitch the slot sings now (chant: the phrase note)
        long long chantStep = -1;
        double freeStart = 0;  // free clock at the note-on (chant when stopped)
        int partCount = 0;
        Part parts[6];
    };
    struct Link
    {
        int slot = -1, part = 0;
    };

    void noteOn(int key, float velocity);
    void noteOff(int key);
    void releaseSlot(int slot);
    int takeSlot(); // a free slot, or the oldest released, or the oldest held
    void startSlot(int slot, bool isIson);
    int takeSinger(int forSlot);
    void updateChant(double songBeats, bool playing);
    void updateIson();
    double clockFor(const Slot&, double songBeats, bool playing) const;
    float noteVowelStep(long long index) const;
    int noteOnset(long long index) const;
    void readShared();
    void renderVoices(float* left, float* right, int n);
    void updateFaceState();

    double sampleRate_ = 44100.0;
    HostInfo host_;
    std::shared_ptr<const seraph::GlottalTables> tables_;
    seraph::Shared shared_;
    std::array<seraph::SeraphVoice, kMaxSingers> voices_;
    std::array<Link, kMaxSingers> links_;
    std::array<Slot, kMaxNotes + 1> slots_; // the last is the ison
    std::array<int, kMaxNotes> legatoKeys_{};
    int legatoCount_ = 0;
    unsigned long long counter_ = 0, noteCount_ = 0;
    bool sustain_ = false;
    double freeBeats_ = 0, gateStart_ = 0, segmentOffsetBeats_ = 0;
    float bend_ = 0, modWheel_ = 0;

    // post
    dsp::TranceGate gate_;
    struct Biquad
    {
        float b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0;
        float z1[2]{}, z2[2]{};
        float process(float x, int ch)
        {
            const float y = b0 * x + z1[ch];
            z1[ch] = b1 * x - a1 * y + z2[ch];
            z2[ch] = b2 * x - a2 * y;
            return y;
        }
        void clear() { z1[0] = z1[1] = z2[0] = z2[1] = 0; }
    };
    Biquad lowCut_, warmth_, air_, wetHighPass_;
    float distanceLp_[2]{};
    serum_hall::Hall hall_;
    int hallSize_ = -1;
    float hallDistance_ = -1;
    int quietBlocks_ = 0;
    static constexpr int kChunk = 256;
    std::array<float, kChunk> left_{}, right_{};

    // custom gate
    std::array<std::atomic<float>, dsp::TranceGate::kMaxSteps> custom_;
    std::atomic<int> customSteps_{16};

    // face
    std::array<std::atomic<float>, kMaxSingers> viewPan_, viewLevel_, viewPitch_, viewHz_;
    std::array<std::atomic<int>, kMaxSingers> viewType_;
    std::array<std::atomic<const seraph::RealVoice*>, kMaxSingers> viewReal_;
    std::atomic<int> activeSingers_{0}, gateStep_{-1}, chantStep_{-1};
    std::atomic<float> lowestHz_{0};

    std::string presetName_, packWanted_;

    // Real voices: the message thread's list and the bank the audio thread sings from (swapped lock-free; the audio
    // thread acknowledges the bank it uses and the one its releasing singers still read).
    struct RealBank
    {
        std::vector<std::shared_ptr<const seraph::RealVoice>> voices;
    };
    void publishRealBank();
    void collectRealGarbage();
    const seraph::RealVoice* chooseReal(int type, float pitch, int singer) const;
    std::vector<std::shared_ptr<const seraph::RealVoice>> realVoices_;
    std::vector<std::unique_ptr<const RealBank>> realOwned_;
    std::atomic<const RealBank*> realPending_{nullptr}, realInUse_{nullptr}, realPrevious_{nullptr};
    const RealBank* realBank_ = nullptr;     // audio thread
    const RealBank* realOldBank_ = nullptr;  // audio thread: the bank before, while singers still read it
    std::atomic<float> realDistance_{-1.0f};
};
} // namespace sonora::devices

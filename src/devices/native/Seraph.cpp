#include "devices/native/Seraph.h"

#include "core/Scales.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <mutex>
#include <thread>
#include <sstream>

namespace sonora::devices
{
namespace
{
using namespace seraph;
constexpr double kPi = 3.14159265358979323846;
constexpr int kSegment = 64; // chant and MIDI resolution inside a block

uint32_t mix(uint32_t h)
{
    h ^= h >> 16;
    h *= 0x7feb352dU;
    h ^= h >> 15;
    h *= 0x846ca68bU;
    h ^= h >> 16;
    return h;
}
uint32_t hash(uint32_t a, uint32_t b, uint32_t c, uint32_t d)
{
    return mix(a * 0x9E3779B1U ^ mix(b + 0x85EBCA77U) ^ mix(c * 0xC2B2AE3DU + 0x27D4EB2FU) ^ mix(d + 0x165667B1U));
}
float unit(uint32_t h) { return (float) (mix(h) >> 8) * (1.0f / 16777216.0f); }
float gaussian(uint32_t h)
{
    float s = 0;
    for (uint32_t i = 0; i < 4; ++i) s += unit(h + i * 0x632BE5ABU) * 2.0f - 1.0f;
    return s * 0.8660254f;
}

std::vector<ParameterInfo> seraphParameters()
{
    using S = Seraph;
    auto names = [](const char* const* list, int count) { return std::vector<std::string>(list, list + count); };
    std::vector<std::string> keys = names(S::kKeyNames, 12), scales, patterns, phraseNames;
    for (const auto& s : model::kScales) scales.emplace_back(s.name);
    for (const auto& p : dsp::TranceGate::factory()) patterns.emplace_back(p.name);
    patterns.emplace_back("Custom");
    for (const auto& p : S::phrases()) phraseNames.emplace_back(p.name);
    std::vector<std::string> rates(dsp::TranceGate::kRateNames, dsp::TranceGate::kRateNames + dsp::TranceGate::kRates);

    std::vector<ParameterInfo> p(S::NumParams);
    p[S::Section] = makeParam("Section", "Section", 0, S::kSections - 1, S::Mixed, "", false, names(S::kSectionNames, S::kSections));
    p[S::Singers] = makeParam("Singers", "Singers", 1, S::kMaxSingersPerNote, 5, "int");
    p[S::MenOctave] = makeParam("MenOctave", "Men Octave", 0, 1, 0, "", true);
    p[S::Fold] = makeParam("Fold", "Fold", 0, 1, 1, "", true);
    p[S::Mode] = makeParam("Mode", "Mode", 0, 1, 0, "", false, {"Poly", "Legato"});
    p[S::Vowel] = makeParam("Vowel", "Vowel", 0, 4, 0, "vowel");
    p[S::Hum] = makeParam("Hum", "Hum", 0, 1, 0, "%");
    p[S::Size] = makeParam("Size", "Size", -0.2f, 0.2f, 0, "%");
    p[S::Bright] = makeParam("Bright", "Bright", -12, 12, 0, "dB");
    p[S::VowelMotion] = makeParam("VowelMotion", "Vowel Motion", 0, 1, 0.2f, "%");
    p[S::MotionRate] = makeParam("MotionRate", "Motion Rate", 0.02f, 2.0f, 0.15f, "Hz");
    p[S::VowelSteps] = makeParam("VowelSteps", "Vowel Steps", 0, S::kVowelStepModes - 1, 0, "", false, names(S::kVowelStepNames, S::kVowelStepModes));
    p[S::Onset] = makeParam("Onset", "Onset", 0, S::kOnsetChoices - 1, 0, "", false, names(S::kOnsetNames, S::kOnsetChoices));
    p[S::Tension] = makeParam("Tension", "Tension", 0, 1, 0.5f, "%");
    p[S::Breath] = makeParam("Breath", "Breath", 0, 1, 0.2f, "%");
    p[S::Effort] = makeParam("Effort", "Effort", 0, 1, 0.5f, "%");
    p[S::Vibrato] = makeParam("Vibrato", "Vibrato", 0, 100, 12, "cents");
    p[S::VibratoRate] = makeParam("VibratoRate", "Vibrato Rate", 4, 7.5f, 5.5f, "Hz");
    p[S::VibratoDelay] = makeParam("VibratoDelay", "Vibrato Delay", 0, 1500, 300, "ms");
    // PLG-27d: 24, fitted to VocalSet's straight long tones (54 notes, 6 singers: the pitch smoothed over half a second
    // wanders with an SD of 5.7 cents; Drift 8 gave 1.5, 24 gives 4.8)
    p[S::Drift] = makeParam("Drift", "Drift", 0, 30, 24, "cents");
    p[S::Detune] = makeParam("Detune", "Detune", 0, 25, 8, "cents");
    p[S::Timing] = makeParam("Timing", "Timing", 0, 80, 25, "ms");
    p[S::Scoop] = makeParam("Scoop", "Scoop", 0, 100, 10, "cents");
    p[S::Human] = makeParam("Human", "Human", 0, 1, 0.5f, "%");
    p[S::SizeSpread] = makeParam("SizeSpread", "Size Spread", 0, 0.08f, 0.04f, "%");
    p[S::Attack] = makeParam("Attack", "Attack", 5, 3000, 120, "ms");
    p[S::Release] = makeParam("Release", "Release", 20, 6000, 600, "ms");
    p[S::Swell] = makeParam("Swell", "Swell", 0, 1, 0, "%");
    p[S::Glide] = makeParam("Glide", "Glide", 0, 800, 120, "ms");
    p[S::Dynamics] = makeParam("Dynamics", "Dynamics", 0, 1, 0.6f, "%");
    p[S::Chant] = makeParam("Chant", "Chant", 0, 1, 0, "", true);
    p[S::Key] = makeParam("Key", "Key", 0, 11, 9, "", false, keys);
    p[S::Scale] = makeParam("Scale", "Scale", 0, (float) model::kScales.size() - 1, 5, "", false, scales);
    p[S::PhraseParam] = makeParam("Phrase", "Phrase", 0, (float) S::phrases().size() - 1, 0, "", false, phraseNames);
    p[S::ChantRate] = makeParam("ChantRate", "Chant Rate", 0, S::kChantRates - 1, 2, "", false, names(S::kChantRateNames, S::kChantRates));
    p[S::Organum] = makeParam("Organum", "Organum", 0, S::kOrganums - 1, 0, "", false, names(S::kOrganumNames, S::kOrganums));
    p[S::Ison] = makeParam("Ison", "Ison", 0, S::kIsons - 1, 0, "", false, names(S::kIsonNames, S::kIsons));
    p[S::IsonLevel] = makeParam("IsonLevel", "Ison Level", 0, 1, 0.5f, "%");
    p[S::Gate] = makeParam("Gate", "Gate", 0, 1, 0, "", true);
    p[S::GatePattern] = makeParam("GatePattern", "Gate Pattern", 0, (float) patterns.size() - 1, 0, "", false, patterns);
    p[S::GateRate] = makeParam("GateRate", "Gate Rate", 0, dsp::TranceGate::kRates - 1, dsp::TranceGate::Sixteenth, "", false, rates);
    p[S::GateLength] = makeParam("GateLength", "Gate Length", 0.1f, 1.0f, 0.65f, "%");
    p[S::GateSmooth] = makeParam("GateSmooth", "Gate Smooth", 1, 50, 5, "ms");
    p[S::GateDepth] = makeParam("GateDepth", "Gate Depth", 0, 1, 1, "%");
    p[S::GateSwing] = makeParam("GateSwing", "Gate Swing", 0, 1, 0, "%");
    p[S::GatePosition] = makeParam("GatePosition", "Gate Position", 0, 1, 0, "", false, {"Before Space", "After Space"});
    p[S::LowCut] = makeParam("LowCut", "Low Cut", 40, 400, 80, "Hz");
    p[S::Air] = makeParam("Air", "Air", 0, 9, 2, "dB");
    p[S::Warmth] = makeParam("Warmth", "Warmth", 0, 6, 1, "dB");
    p[S::Space] = makeParam("Space", "Space", 0, 1, 0.35f, "%");
    p[S::SpaceSize] = makeParam("SpaceSize", "Space Size", 0, S::kSpaceSizes - 1, 2, "", false, names(S::kSpaceSizeNames, S::kSpaceSizes));
    p[S::Distance] = makeParam("Distance", "Distance", 0, 1, 0.3f, "%");
    p[S::Width] = makeParam("Width", "Width", 0, 1, 0.7f, "%");
    p[S::Output] = makeParam("Output", "Output", -24, 12, 0, "dB");
    p[S::Seed] = makeParam("Seed", "Seed", 0, 99, 0, "int");
    p[S::Source] = makeParam("Source", "Source", 0, 1, 0, "", false, {"Voices", "Real"});
    return p;
}

// RBJ cookbook biquads.
template <typename B>
void designHighPass(B& b, double sr, double hz, double q)
{
    const double w = 2.0 * kPi * std::min(hz, sr * 0.45) / sr, alpha = std::sin(w) / (2.0 * q), c = std::cos(w), a0 = 1.0 + alpha;
    b.b0 = (float) ((1.0 + c) * 0.5 / a0);
    b.b1 = (float) (-(1.0 + c) / a0);
    b.b2 = b.b0;
    b.a1 = (float) (-2.0 * c / a0);
    b.a2 = (float) ((1.0 - alpha) / a0);
}
template <typename B>
void designPeak(B& b, double sr, double hz, double q, double db)
{
    const double a = std::pow(10.0, db / 40.0), w = 2.0 * kPi * hz / sr, alpha = std::sin(w) / (2.0 * q), c = std::cos(w);
    const double a0 = 1.0 + alpha / a;
    b.b0 = (float) ((1.0 + alpha * a) / a0);
    b.b1 = (float) (-2.0 * c / a0);
    b.b2 = (float) ((1.0 - alpha * a) / a0);
    b.a1 = b.b1;
    b.a2 = (float) ((1.0 - alpha / a) / a0);
}
template <typename B>
void designHighShelf(B& b, double sr, double hz, double db)
{
    const double a = std::pow(10.0, db / 40.0), w = 2.0 * kPi * std::min(hz, sr * 0.4) / sr, c = std::cos(w);
    const double alpha = std::sin(w) / 2.0 * std::sqrt(2.0), sa = 2.0 * std::sqrt(a) * alpha;
    const double a0 = (a + 1.0) - (a - 1.0) * c + sa;
    b.b0 = (float) (a * ((a + 1.0) + (a - 1.0) * c + sa) / a0);
    b.b1 = (float) (-2.0 * a * ((a - 1.0) + (a + 1.0) * c) / a0);
    b.b2 = (float) (a * ((a + 1.0) + (a - 1.0) * c - sa) / a0);
    b.a1 = (float) (2.0 * ((a - 1.0) - (a + 1.0) * c) / a0);
    b.a2 = (float) (((a + 1.0) - (a - 1.0) * c - sa) / a0);
}

// The hall settings per Space Size (Serum hall Size / Decay) and its wet level, calibrated by the decay-time test.
struct HallSetting
{
    float size, decay, wet;
};
// Decay solved for 1.2 / 2.5 / 4.5 / 8 / 20 s (RT60 from a Schroeder fit, -5..-25 dB, of a voice-like band 150 Hz - 1 kHz at
// Distance 30 %; Distance moves it by about 10 %; Chapel set in the device by the decay-time test, a choir chord),
// wet levels equalised for a steady choir-like input.
constexpr HallSetting kHallSettings[Seraph::kSpaceSizes] = {
    {0.25f, 0.2157f, 1.01f}, {0.40f, 0.2850f, 0.97f}, {0.55f, 0.4303f, 0.94f}, {0.70f, 0.5185f, 0.92f}, {0.85f, 0.6709f, 0.81f},
};

float softClip(float x)
{
    const float a = std::abs(x);
    if (a <= 0.9f) return x;
    const float y = 0.9f + 0.1f * std::tanh((a - 0.9f) * 10.0f);
    return x < 0 ? -y : y;
}

// Voice types of a section, low to high.
int sectionTypes(int section, bool menOctave, int types[4], bool& boys)
{
    boys = false;
    switch (section)
    {
        case Seraph::Men: types[0] = Bass; types[1] = Tenor; return 2;
        case Seraph::Women: types[0] = Alto; types[1] = Soprano; return 2;
        case Seraph::Boys: types[0] = Soprano; boys = true; return 1;
        case Seraph::SoloBass: types[0] = Bass; return 1;
        case Seraph::SoloTenor: types[0] = Tenor; return 1;
        case Seraph::SoloAlto: types[0] = Alto; return 1;
        case Seraph::SoloSoprano: types[0] = Soprano; return 1;
        case Seraph::Mixed:
        default:
            if (menOctave) { types[0] = Alto; types[1] = Soprano; return 2; }
            types[0] = Bass; types[1] = Tenor; types[2] = Alto; types[3] = Soprano;
            return 4;
    }
}

// The types of `candidates` that sing `pitch`, after folding it by octaves when none does (Fold) or falling back to the
// nearest one (strain). Returns the count; `fold` receives the octave shift.
int fittingTypes(const int* candidates, int count, bool boys, int pitch, bool fold, int out[4], int& shift)
{
    auto fits = [&](int type, int p) {
        int low, high;
        range(type, boys, low, high);
        return p >= low && p <= high;
    };
    shift = 0;
    auto collect = [&](int p) {
        int n = 0;
        for (int i = 0; i < count; ++i)
            if (fits(candidates[i], p)) out[n++] = candidates[i];
        return n;
    };
    int n = collect(pitch);
    if (n > 0) return n;
    if (fold)
        for (int octaves : {12, -12, 24, -24, 36, -36, 48, -48})
            if ((n = collect(pitch + octaves)) > 0)
            {
                shift = octaves;
                return n;
            }
    int best = candidates[0], bestDistance = 1 << 30;
    for (int i = 0; i < count; ++i)
    {
        int low, high;
        range(candidates[i], boys, low, high);
        const int d = pitch < low ? low - pitch : pitch > high ? pitch - high : 0;
        if (d < bestDistance) { bestDistance = d; best = candidates[i]; }
    }
    out[0] = best;
    return 1;
}
} // namespace

// ---- phrases

const std::vector<Seraph::Phrase>& Seraph::phrases()
{
    // Scale-step offsets from the tonic and lengths in units. Descriptive names; none comes from a recording or a notated work.
    static const std::vector<Phrase> list = {
        {"Neighbour", 3, {0, 1, 0}, {2, 1, 1}},
        {"Sigh", 3, {0, -1, 0}, {2, 1, 1}},
        {"Rise", 3, {0, 1, 2}, {1, 1, 2}},
        {"Fall", 3, {2, 1, 0}, {1, 1, 2}},
        {"Arch", 3, {0, 2, 1}, {1, 1, 2}},
        {"Return", 3, {0, 2, 0}, {2, 1, 1}},
        {"Lean", 2, {1, 0}, {1, 3}},
        {"Recite", 5, {0, 0, 0, 1, 0}, {1, 1, 1, 1, 2}},
        {"Psalm", 6, {0, 0, 0, 0, -1, 0}, {1, 1, 1, 1, 1, 3}},
        {"Turn", 5, {0, 1, 0, -1, 0}, {1, 1, 1, 1, 2}},
        {"Third", 4, {0, 2, 1, 0}, {2, 1, 1, 2}},
        {"Call", 3, {0, 4, 3}, {1, 2, 1}},
        {"Answer", 3, {3, 1, 0}, {1, 1, 2}},
        {"Amen", 2, {1, 0}, {2, 4}},
        {"Drone", 1, {0}, {4}},
        {"Lament", 5, {0, -1, -2, -1, 0}, {1, 1, 1, 1, 2}},
        {"Descent", 4, {3, 2, 1, 0}, {1, 1, 1, 3}},
        {"Lift", 5, {0, 0, 1, 2, 1}, {2, 1, 1, 2, 2}},
        {"Low Turn", 4, {0, -1, -2, 0}, {2, 1, 1, 4}},
        {"Echo", 4, {0, 1, 0, 1}, {1, 1, 1, 1}},
    };
    return list;
}

void Seraph::phraseStepAt(const Phrase& phrase, double units, int& step, long long& absoluteStep)
{
    const int total = std::max(1, phrase.totalUnits());
    const double cycles = std::floor(units / total);
    double within = units - cycles * total;
    step = 0;
    for (int i = 0; i < phrase.count; ++i)
    {
        if (within < phrase.length[i] || i == phrase.count - 1) { step = i; break; }
        within -= phrase.length[i];
    }
    absoluteStep = (long long) cycles * phrase.count + step;
}

int Seraph::snapToScale(int note, int key, int scale)
{
    for (int d = 0; d < 12; ++d)
    {
        if (model::inScale(note - d, key, scale)) return note - d; // ties go down: the step below
        if (model::inScale(note + d, key, scale)) return note + d;
    }
    return note;
}

int Seraph::chantPitch(int heldNote, const Phrase& phrase, int step, int key, int scale)
{
    const int tonic = snapToScale(heldNote, key, scale);
    step = std::clamp(step, 0, phrase.count - 1);
    return model::transposeInScale(tonic, phrase.offset[step], key, model::scaleAt(scale));
}

// ---- construction

Seraph::Seraph() : NativeDevice(seraphParameters())
{
    for (auto& c : custom_) c.store(0.0f);
    const auto& first = dsp::TranceGate::factory()[0];
    for (int i = 0; i < dsp::TranceGate::kMaxSteps; ++i) custom_[(size_t) i].store(first.level[i]);
    customSteps_.store(first.steps);
    for (size_t i = 0; i < kMaxSingers; ++i)
    {
        viewPan_[i].store(0);
        viewLevel_[i].store(0);
        viewPitch_[i].store(0);
        viewHz_[i].store(0);
        viewReal_[i].store(nullptr);
        viewType_[i].store(-1);
    }
}

Seraph::~Seraph() = default;

void Seraph::prepare(double sampleRate, int)
{
    sampleRate_ = sampleRate > 0 ? sampleRate : 44100.0;
    tables_ = GlottalTables::forRate(sampleRate_);
    (void) loudnessGain(Tenor, 0, 0, 3, 220); // builds the loudness table here, not on the audio thread
    for (auto& v : voices_) v.prepare(sampleRate_);
    gate_.prepare(sampleRate_);
    hall_.prepare(sampleRate_);
    hallSize_ = -1;
    hallDistance_ = -1;
    reset();
}

void Seraph::reset()
{
    for (auto& v : voices_) v.reset();
    for (auto& l : links_) l = {};
    for (auto& s : slots_) s = {};
    legatoCount_ = 0;
    sustain_ = false;
    freeBeats_ = gateStart_ = 0;
    bend_ = modWheel_ = 0;
    gate_.reset();
    for (auto* b : {&lowCut_, &warmth_, &air_, &wetHighPass_}) b->clear();
    distanceLp_[0] = distanceLp_[1] = 0;
    hall_.reset();
    quietBlocks_ = 1 << 20;
}

// ---- custom gate and content

void Seraph::setCustomGate(const float* levels, int steps)
{
    steps = std::clamp(steps, 1, dsp::TranceGate::kMaxSteps);
    for (int i = 0; i < dsp::TranceGate::kMaxSteps; ++i)
        custom_[(size_t) i].store(i < steps ? std::clamp(levels[i], 0.0f, 1.0f) : 0.0f, std::memory_order_relaxed);
    customSteps_.store(steps, std::memory_order_relaxed);
}

void Seraph::setCustomGateStep(int step, float level)
{
    if (step < 0 || step >= dsp::TranceGate::kMaxSteps) return;
    custom_[(size_t) step].store(std::clamp(level, 0.0f, 1.0f), std::memory_order_relaxed);
}

float Seraph::customGateLevel(int step) const
{
    return step >= 0 && step < dsp::TranceGate::kMaxSteps ? custom_[(size_t) step].load(std::memory_order_relaxed) : 0.0f;
}

int Seraph::gateSteps() const
{
    const int pattern = (int) std::lround(getParameter(GatePattern));
    if (pattern >= kCustomPattern) return customGateSteps();
    return dsp::TranceGate::factory()[(size_t) std::clamp(pattern, 0, kCustomPattern - 1)].steps;
}

float Seraph::gateLevel(int step) const
{
    const int pattern = (int) std::lround(getParameter(GatePattern));
    if (pattern >= kCustomPattern) return customGateLevel(step);
    return dsp::TranceGate::factory()[(size_t) std::clamp(pattern, 0, kCustomPattern - 1)].level[std::clamp(step, 0, dsp::TranceGate::kMaxSteps - 1)];
}

std::string Seraph::gateText() const
{
    std::string out = std::to_string(customGateSteps());
    char buffer[32];
    for (int i = 0; i < customGateSteps(); ++i)
    {
        std::snprintf(buffer, sizeof buffer, " %.4g", (double) customGateLevel(i));
        out += buffer;
    }
    return out;
}

bool Seraph::setGateText(const std::string& text)
{
    std::istringstream values(text);
    int steps = 0;
    values >> steps;
    if (! values || steps < 1 || steps > dsp::TranceGate::kMaxSteps) return false;
    float levels[dsp::TranceGate::kMaxSteps]{};
    for (int i = 0; i < steps; ++i)
        if (! (values >> levels[i])) return false;
    setCustomGate(levels, steps);
    return true;
}

std::map<std::string, std::string> Seraph::stateAttributes() const
{
    std::map<std::string, std::string> out{{"gate", gateText()}, {"preset", presetName_}};
    const auto paths = realPaths();
    for (size_t i = 0; i < paths.size(); ++i) out["real" + std::to_string(i)] = paths[i];
    return out;
}

void Seraph::applyStateAttributes(const std::map<std::string, std::string>& attributes)
{
    realErrors.clear();
    if (const auto gate = attributes.find("gate"); gate != attributes.end() && ! setGateText(gate->second))
        realErrors.push_back("the saved gate pattern could not be read");
    if (const auto preset = attributes.find("preset"); preset != attributes.end()) presetName_ = preset->second;
    clearRealVoices();
    std::vector<std::string> paths;
    for (int i = 0; i < kMaxRealVoices; ++i)
    {
        const auto it = attributes.find("real" + std::to_string(i));
        if (it == attributes.end()) break;
        paths.push_back(it->second);
    }
    addRealFiles(paths, &realErrors);
}

std::string Seraph::encodeContent() const
{
    // Plain text, one item per line, so a set file stays readable: "preset <name>", "gate <steps> <level> <level> ...".
    std::string out = "seraph 1\npreset " + presetName_ + "\ngate " + gateText() + "\n";
    for (const auto& path : realPaths()) out += "real " + path + "\n";
    return out;
}

bool Seraph::decodeContent(const std::string& text)
{
    realErrors.clear();
    clearRealVoices();
    std::istringstream in(text);
    std::string line;
    bool any = false;
    std::vector<std::string> paths;
    while (std::getline(in, line))
    {
        if (line.rfind("preset ", 0) == 0) { presetName_ = line.substr(7); any = true; }
        else if (line.rfind("gate ", 0) == 0) any = setGateText(line.substr(5)) || any;
        else if (line.rfind("real ", 0) == 0)
        {
            paths.push_back(line.substr(5));
            any = true;
        }
    }
    addRealFiles(paths, &realErrors);
    return any;
}

// ---- Real voices (PLG-27b)

std::shared_ptr<const RealVoice> Seraph::analyseFile(const std::string& path, std::string& error)
{
    static std::mutex mutex;
    static std::map<std::string, std::pair<int64_t, std::shared_ptr<const RealVoice>>> cache;
    const juce::File file(juce::String::fromUTF8(path.c_str()));
    const int64_t stamp = file.getLastModificationTime().toMilliseconds() ^ file.getSize();
    {
        const std::lock_guard<std::mutex> lock(mutex);
        if (const auto it = cache.find(path); it != cache.end() && it->second.first == stamp) return it->second.second;
    }
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr || reader->sampleRate <= 0 || reader->lengthInSamples <= 0)
    {
        error = "cannot read " + file.getFileName().toStdString();
        return nullptr;
    }
    const int frames = (int) std::min<int64_t>(reader->lengthInSamples, (int64_t) (30.0 * reader->sampleRate));
    juce::AudioBuffer<float> audio((int) std::max(1u, std::min(2u, reader->numChannels)), frames);
    reader->read(&audio, 0, frames, 0, true, audio.getNumChannels() > 1);
    std::string reason;
    auto voice = analyseRealVoice(audio.getArrayOfReadPointers(), audio.getNumChannels(), frames, reader->sampleRate,
                                  file.getFileNameWithoutExtension().toStdString(), reason);
    if (voice == nullptr)
    {
        error = file.getFileName().toStdString() + ": " + reason;
        return nullptr;
    }
    auto withPath = std::make_shared<RealVoice>(*voice);
    withPath->path = path;
    const std::lock_guard<std::mutex> lock(mutex);
    cache[path] = {stamp, withPath};
    return withPath;
}

bool Seraph::addRealFile(const std::string& path, std::string& error)
{
    for (const auto& v : realVoices_)
        if (v->path == path) return true;
    auto voice = analyseFile(path, error);
    if (voice == nullptr) return false;
    addRealVoice(std::move(voice));
    return true;
}

int Seraph::addRealFiles(const std::vector<std::string>& paths, std::vector<std::string>* errors)
{
    // Warm the analysis cache on every core, then add in order (each file's analysis is independent).
    std::atomic<size_t> next{0};
    const unsigned workers = std::max(1u, std::min(std::thread::hardware_concurrency(), (unsigned) paths.size()));
    std::vector<std::thread> threads;
    for (unsigned t = 0; t < workers; ++t)
        threads.emplace_back([&] {
            for (size_t i = next++; i < paths.size(); i = next++)
            {
                std::string ignored;
                analyseFile(paths[i], ignored);
            }
        });
    for (auto& t : threads) t.join();
    int added = 0;
    for (const auto& path : paths)
    {
        std::string error;
        if (addRealFile(path, error)) ++added;
        else if (errors) errors->push_back(error);
    }
    return added;
}

void Seraph::addRealVoice(std::shared_ptr<const RealVoice> voice)
{
    if (voice == nullptr || (int) realVoices_.size() >= kMaxRealVoices) return;
    realVoices_.push_back(std::move(voice));
    publishRealBank();
}

void Seraph::removeRealVoice(int index)
{
    if (index < 0 || (size_t) index >= realVoices_.size()) return;
    realVoices_.erase(realVoices_.begin() + index);
    publishRealBank();
}

void Seraph::clearRealVoices()
{
    if (realVoices_.empty() && realPending_.load() == nullptr) return;
    realVoices_.clear();
    publishRealBank();
}

std::vector<std::string> Seraph::realPaths() const
{
    std::vector<std::string> out;
    for (const auto& v : realVoices_)
        if (! v->path.empty()) out.push_back(v->path);
    return out;
}

bool Seraph::realActive() const { return std::lround(getParameter(Source)) == RealSource && ! realVoices_.empty(); }

void Seraph::publishRealBank()
{
    auto bank = std::make_unique<RealBank>();
    bank->voices = realVoices_;
    realPending_.store(bank.get(), std::memory_order_release);
    realOwned_.push_back(std::move(bank));
    collectRealGarbage();
}

void Seraph::collectRealGarbage()
{
    // Keep what is pending, what the audio thread uses and still reads, and the two newest (it may be between loading
    // the pending bank and acknowledging it).
    const auto* pending = realPending_.load(std::memory_order_acquire);
    const auto* used = realInUse_.load(std::memory_order_acquire);
    const auto* previous = realPrevious_.load(std::memory_order_acquire);
    for (size_t i = 0; i + 2 < realOwned_.size();)
    {
        const auto* b = realOwned_[i].get();
        if (b != pending && b != used && b != previous) realOwned_.erase(realOwned_.begin() + (long) i);
        else ++i;
    }
}

const RealVoice* Seraph::chooseReal(int type, float pitch, int singer) const
{
    if (realBank_ == nullptr || realBank_->voices.empty()) return nullptr;
    // The recording sung closest to the note, inside the singer's range when one is, on the vowel asked for when the file
    // names one; the singers of a note share out the close ones, so one recording sings with several.
    const auto& voices = realBank_->voices;
    int low = 0, high = 127;
    range(type == kVoiceTypes ? Soprano : type, type == kVoiceTypes, low, high);
    const float vowel = value(Vowel);
    std::array<float, kMaxRealVoices> cost{};
    float best = 1e9f;
    for (size_t i = 0; i < voices.size() && i < (size_t) kMaxRealVoices; ++i)
    {
        const auto& v = *voices[i];
        float c = std::abs(v.midi - pitch);
        if (v.midi < (float) low - 7.0f || v.midi > (float) high + 7.0f) c += 8.0f; // another voice type's recording
        if (v.vowel >= 0) c += 1.5f * std::abs((float) v.vowel - vowel);
        cost[i] = c;
        best = std::min(best, c);
    }
    int count = 0;
    for (size_t i = 0; i < voices.size() && i < (size_t) kMaxRealVoices; ++i)
        if (cost[i] <= best + 3.0f) ++count;
    int pick = singer % std::max(1, count);
    for (size_t i = 0; i < voices.size() && i < (size_t) kMaxRealVoices; ++i)
        if (cost[i] <= best + 3.0f && pick-- == 0) return voices[i].get();
    return voices.front().get();
}

// ---- the Seraph Voices pack (PLG-27h)

Seraph::PackVoices Seraph::findPackVoices(const std::string& packsRoot)
{
    PackVoices found;
    juce::File root;
    if (! packsRoot.empty()) root = juce::File(juce::String::fromUTF8(packsRoot.c_str()));
    else if (const auto env = juce::SystemStats::getEnvironmentVariable("SONORA_PACKS_ROOT", {}); env.isNotEmpty()) root = juce::File(env);
    else root = juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("Sonora").getChildFile("Packs");
    if (! root.isDirectory()) return found;
    for (const auto& entry : juce::RangedDirectoryIterator(root, false, "*", juce::File::findDirectories))
    {
        const auto folder = entry.getFile();
        juce::var manifest;
        if (juce::JSON::parse(folder.getChildFile("pack.json").loadFileAsString(), manifest).failed()) continue;
        if (manifest.getProperty("id", {}).toString() != juce::String(kPackId)) continue;
        found.folder = folder.getFullPathName().toStdString();
        juce::StringArray lines;
        lines.addLines(folder.getChildFile("ATTRIBUTION.txt").loadFileAsString());
        found.credit = lines.isEmpty() ? std::string(kPackCredit) : lines[0].trim().toStdString();
        juce::Array<juce::File> singers;
        for (const auto& s : juce::RangedDirectoryIterator(folder, false, "*", juce::File::findDirectories)) singers.add(s.getFile());
        std::sort(singers.begin(), singers.end(), [](const juce::File& a, const juce::File& b) { return a.getFileName().compareNatural(b.getFileName()) < 0; });
        for (const auto& singerFolder : singers)
        {
            PackSinger singer;
            singer.name = singerFolder.getFileName().toStdString();
            juce::Array<juce::File> files;
            for (const auto& f : juce::RangedDirectoryIterator(singerFolder, false, "*.flac;*.wav", juce::File::findFiles)) files.add(f.getFile());
            std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b) { return a.getFileName().compareNatural(b.getFileName()) < 0; });
            for (const auto& f : files) singer.files.push_back(f.getFullPathName().toStdString());
            if (! singer.files.empty()) found.singers.push_back(std::move(singer));
        }
        break;
    }
    return found;
}

bool Seraph::loadPackSinger(const std::string& singer, const std::string& root)
{
    const auto pack = findPackVoices(root.empty() ? packsRoot : root);
    for (const auto& s : pack.singers)
    {
        if (s.name != singer) continue;
        addRealFiles(s.files);
        setParameter(Source, (float) RealSource);
        return true;
    }
    return false;
}

bool Seraph::packVoicesLoaded(const std::string& root) const
{
    const auto pack = findPackVoices(root.empty() ? packsRoot : root);
    if (pack.folder.empty()) return false;
    for (const auto& path : realPaths())
        if (path.rfind(pack.folder, 0) == 0) return true;
    return false;
}

// ---- notes

std::vector<int> Seraph::typesFor(int pitch) const
{
    const int section = std::clamp((int) std::lround(value(Section)), 0, kSections - 1);
    int types[4], fit[4], shift = 0;
    bool boys = false;
    const int count = sectionTypes(section, value(MenOctave) > 0.5f && section == Mixed, types, boys);
    const int n = fittingTypes(types, count, boys, pitch, value(Fold) > 0.5f, fit, shift);
    const int singers = section >= SoloBass ? 1 : std::clamp((int) std::lround(value(Singers)), 1, kMaxSingersPerNote);
    std::vector<int> out;
    for (int i = 0; i < singers; ++i) out.push_back(boys ? kVoiceTypes : fit[i % n]);
    return out;
}

float Seraph::noteVowelStep(long long index) const
{
    static constexpr int kSequences[kVowelStepModes][5] = {{0}, {0, 3}, {0, 1}, {0, 1, 3}, {0, 3, 4}, {0, 1, 2, 3, 4}, {0}};
    static constexpr int kLengths[kVowelStepModes] = {1, 2, 2, 3, 3, 5, 1};
    const int mode = std::clamp((int) std::lround(value(VowelSteps)), 0, kVowelStepModes - 1);
    if (mode == kVowelStepModes - 1)
        return (float) std::min(4, (int) (unit(hash((uint32_t) value(Seed), (uint32_t) index, 0x766F77u, 1)) * 5.0f));
    const long long i = ((index % kLengths[mode]) + kLengths[mode]) % kLengths[mode];
    return (float) kSequences[mode][i];
}

int Seraph::noteOnset(long long index) const
{
    const int choice = std::clamp((int) std::lround(value(Onset)), 0, kOnsetChoices - 1);
    switch (choice)
    {
        case Soft: return OnsetSoft;
        case H: return OnsetH;
        case M: return OnsetM;
        case N: return OnsetN;
        case L: return OnsetL;
        case Y: return OnsetY;
        case W: return OnsetW;
        case Cycle:
        {
            static constexpr int kCycle[3] = {OnsetM, OnsetN, OnsetL};
            return kCycle[((index % 3) + 3) % 3];
        }
        default: return (int) std::min(6.0f, unit(hash((uint32_t) value(Seed), (uint32_t) index, 0x6F6E73u, 2)) * 7.0f);
    }
}

int Seraph::takeSlot()
{
    int best = -1;
    for (int i = 0; i < kMaxNotes; ++i)
        if (! slots_[(size_t) i].used) return i;
    unsigned long long oldest = ~0ull;
    for (int i = 0; i < kMaxNotes; ++i) // the oldest released note goes first
    {
        const auto& s = slots_[(size_t) i];
        if (! s.held && ! s.sustained && s.releasedAt < oldest) { oldest = s.releasedAt; best = i; }
    }
    if (best < 0)
        for (int i = 0; i < kMaxNotes; ++i)
            if (slots_[(size_t) i].order < oldest) { oldest = slots_[(size_t) i].order; best = i; }
    for (size_t v = 0; v < kMaxSingers; ++v)
        if (links_[v].slot == best)
        {
            voices_[v].kill();
            links_[v].slot = -1;
        }
    return best;
}

int Seraph::takeSinger(int forSlot)
{
    for (int v = 0; v < kMaxSingers; ++v)
        if (voices_[(size_t) v].idle()) return v;
    // A singer of the oldest released note; then of the oldest held note (not the one being started).
    int best = -1;
    unsigned long long bestKey = ~0ull;
    float bestLevel = 2.0f;
    for (int v = 0; v < kMaxSingers; ++v)
    {
        const int s = links_[(size_t) v].slot;
        const bool releasedSinger = s < 0 || (! slots_[(size_t) s].held && ! slots_[(size_t) s].sustained);
        if (! releasedSinger || s == forSlot) continue;
        const unsigned long long key = s < 0 ? 0 : slots_[(size_t) s].releasedAt;
        const float level = voices_[(size_t) v].level();
        if (key < bestKey || (key == bestKey && level < bestLevel)) { bestKey = key; bestLevel = level; best = v; }
    }
    if (best >= 0) return best;
    for (int v = 0; v < kMaxSingers; ++v)
    {
        const int s = links_[(size_t) v].slot;
        if (s < 0 || s == forSlot || slots_[(size_t) s].ison) continue;
        if (slots_[(size_t) s].order < bestKey) { bestKey = slots_[(size_t) s].order; best = v; }
    }
    return best;
}

double Seraph::clockFor(const Slot& slot, double songBeats, bool playing) const
{
    return playing ? songBeats : freeBeats_ - slot.freeStart;
}

void Seraph::startSlot(int index, bool isIson)
{
    auto& slot = slots_[(size_t) index];
    const int section = std::clamp((int) std::lround(value(Section)), 0, kSections - 1);
    const bool solo = section >= SoloBass;
    const int singers = solo ? 1 : std::clamp((int) std::lround(value(Singers)), 1, kMaxSingersPerNote);
    const bool menOctave = section == Mixed && value(MenOctave) > 0.5f;
    const bool fold = value(Fold) > 0.5f;
    const uint32_t seed = (uint32_t) std::lround(value(Seed));
    const float width = value(Width);

    struct PartPlan
    {
        int offset, count, types[4], typeCount;
        bool boys;
        float gain;
    };
    PartPlan plans[6]{};
    int planCount = 0;
    long long noteIndex = 0;
    if (isIson)
    {
        const int ison = (int) std::lround(value(Ison));
        const int key = std::clamp((int) std::lround(value(Key)), 0, 11);
        slot.line = 40 + ((key - 40) % 12 + 12) % 12;
        plans[planCount++] = {0, 3, {Bass, Tenor}, 2, false, 1.0f};
        if (ison == IsonRootFifth) plans[planCount++] = {7, 2, {Bass, Tenor}, 2, false, 0.8f};
        noteIndex = 0;
    }
    else
    {
        PartPlan main{};
        main.offset = 0;
        main.count = singers;
        main.typeCount = sectionTypes(section, menOctave, main.types, main.boys);
        main.gain = 1.0f;
        plans[planCount++] = main;
        if (menOctave) plans[planCount++] = {-12, singers, {Bass, Tenor}, 2, false, 1.0f};
        const int organum = (int) std::lround(value(Organum));
        static constexpr int kIntervals[kOrganums][2] = {{0, 0}, {5, 0}, {7, 0}, {12, 0}, {7, 12}, {-12, 0}};
        for (int k = 0; k < 2 && organum > OrganumOff; ++k)
            if (kIntervals[organum][k] != 0)
            {
                PartPlan p = main;
                p.offset = kIntervals[organum][k];
                p.count = std::max(1, (singers + 1) / 2);
                p.gain = 0.85f;
                // the octave below goes to the men when the section has them
                plans[planCount++] = p;
            }
        // where the note starts: the chant's phrase note at this moment, or the key itself
        slot.line = slot.key;
        slot.chantStep = -1;
        noteIndex = (long long) noteCount_++;
        if (value(Chant) > 0.5f)
        {
            const auto& list = phrases();
            const auto& phrase = list[(size_t) std::clamp((int) std::lround(value(PhraseParam)), 0, (int) list.size() - 1)];
            const double unitBeats = kChantRateBeats[std::clamp((int) std::lround(value(ChantRate)), 0, kChantRates - 1)];
            int step = 0;
            long long absolute = 0;
            phraseStepAt(phrase, clockFor(slot, host_.beats + segmentOffsetBeats_, host_.playing) / unitBeats, step, absolute);
            slot.line = chantPitch(slot.key, phrase, step, std::clamp((int) std::lround(value(Key)), 0, 11),
                                   (int) std::lround(value(Scale)));
            slot.chantStep = absolute;
            noteIndex = absolute;
        }
    }

    // singers: count them first so the pan spreads over the whole note
    int total = 0;
    for (int p = 0; p < planCount; ++p) total += plans[p].count;
    const float spreadSalt = unit(hash(seed, (uint32_t) slot.key + 977u, 3, 4));
    const float vowelStep = isIson ? 0.0f : noteVowelStep(noteIndex);
    const int onset = isIson ? OnsetSoft : noteOnset(noteIndex);
    slot.partCount = planCount;
    int g = 0;
    for (int p = 0; p < planCount; ++p)
    {
        const auto& plan = plans[p];
        int fit[4], shift = 0;
        const int n = fittingTypes(plan.types, plan.typeCount, plan.boys, slot.line + plan.offset, fold, fit, shift);
        slot.parts[p] = {plan.offset, shift};
        for (int i = 0; i < plan.count; ++i, ++g)
        {
            const int v = takeSinger(index);
            if (v < 0) return;
            const int type = fit[i % n];
            const uint32_t h = hash(seed, (uint32_t) (isIson ? 200 : slot.key), (uint32_t) p, (uint32_t) i);
            const bool alone = total == 1;
            Traits t;
            t.tuneCents = value(Detune) * gaussian(h + 1) * (alone ? 0.3f : 1.0f);
            t.delayMs = value(Timing) * unit(h + 2) * (alone ? 0.3f : 1.0f);
            t.throat = 1.0f + value(SizeSpread) * (2.0f * unit(h + 3) - 1.0f) * (alone ? 0.0f : 1.0f);
            t.vibRate = alone ? 1.0f : 1.0f + 0.15f * (2.0f * unit(h + 4) - 1.0f);
            t.vibDepth = alone ? 1.0f : 0.6f + 0.8f * unit(h + 5);
            t.vibPhase = unit(h + 6);
            t.vowelOffset = alone ? 0.0f : 0.25f * (2.0f * unit(h + 7) - 1.0f);
            // pan: spread over the note's singers (golden-ratio order), seated a little by voice type; Width 100 % puts the outer
            // singers hard left and right (equal-power panning of a uniform spread alone keeps the sides 0.64 correlated)
            static constexpr float kSeat[kVoiceTypes] = {0.35f, 0.15f, -0.15f, -0.35f};
            const float spread = alone ? 0.0f : 2.0f * std::fmod((float) g * 0.618034f + spreadSalt, 1.0f) - 1.0f;
            const float seat = isIson ? 0.0f : kSeat[type];
            t.pan = width * std::clamp(1.3f * (0.8f * spread + 0.2f * seat) + 0.1f * (unit(h + 8) - 0.5f), -1.0f, 1.0f);
            t.breath = 1.0f + 0.2f * (2.0f * unit(h + 9) - 1.0f);
            t.brightDb = 1.5f * (2.0f * unit(h + 10) - 1.0f);
            t.attack = 1.0f + 0.15f * (2.0f * unit(h + 11) - 1.0f);
            t.scoopMs = alone ? 60.0f : 40.0f + 40.0f * unit(h + 12);
            t.motionRate = 0.8f + 0.4f * unit(h + 13);
            t.onsetMs = unit(h + 14);
            t.seed = mix(h + 15) | 1u;
            StartInfo info;
            info.type = type;
            info.boys = plan.boys;
            info.pitch = (float) (slot.line + plan.offset + shift);
            info.velocity = std::clamp(slot.velocity * (1.0f + 0.06f * (2.0f * unit(h + 16) - 1.0f)), 0.01f, 1.0f);
            info.vowelStep = vowelStep;
            info.onset = onset;
            // a soloist stands in front of the choir: +9 dB, so one voice is not lost against the sections' sqrt(n) sum
            info.gain = plan.gain * (isIson ? value(IsonLevel) : 1.0f) * (solo ? 2.8f : 1.0f);
            info.drone = isIson;
            info.traits = t;
            if (std::lround(value(Source)) == RealSource)
            {
                info.real = chooseReal(info.boys ? kVoiceTypes : type, info.pitch, g);
                if (info.real != nullptr && p == 0 && i == 0) realDistance_.store(std::abs(info.real->midi - info.pitch), std::memory_order_relaxed);
            }
            voices_[(size_t) v].start(info);
            links_[(size_t) v] = {index, p};
        }
    }
}

void Seraph::releaseSlot(int index)
{
    auto& slot = slots_[(size_t) index];
    slot.held = slot.sustained = false;
    slot.releasedAt = ++counter_;
    for (size_t v = 0; v < kMaxSingers; ++v)
        if (links_[v].slot == index) voices_[v].release();
}

void Seraph::noteOn(int key, float velocity)
{
    const bool legato = std::lround(value(Mode)) == Legato;
    bool anySounding = false;
    for (const auto& v : voices_) anySounding = anySounding || ! v.idle();
    if (! anySounding) gateStart_ = freeBeats_; // the free gate starts with the first note
    if (legato)
    {
        for (int i = 0; i < legatoCount_; ++i)
            if (legatoKeys_[(size_t) i] == key)
            {
                std::copy(legatoKeys_.begin() + i + 1, legatoKeys_.begin() + legatoCount_, legatoKeys_.begin() + i);
                --legatoCount_;
                break;
            }
        if (legatoCount_ == kMaxNotes) std::copy(legatoKeys_.begin() + 1, legatoKeys_.end(), legatoKeys_.begin()), --legatoCount_;
        legatoKeys_[(size_t) legatoCount_++] = key;
        for (int i = 0; i < kMaxNotes; ++i)
        {
            auto& s = slots_[(size_t) i];
            if (! s.used || ! (s.held || s.sustained)) continue;
            // one line: the sounding note glides to the new one, no new attack, the vibrato carries on
            s.key = key;
            s.held = true;
            s.sustained = false;
            s.chantStep = -1;
            if (value(Chant) <= 0.5f)
            {
                s.line = key;
                for (size_t v = 0; v < kMaxSingers; ++v)
                    if (links_[v].slot == i)
                    {
                        const auto& part = s.parts[links_[v].part];
                        voices_[v].glideTo((float) (key + part.offset + part.fold), value(Glide));
                    }
            }
            return;
        }
    }
    else
    {
        for (int i = 0; i < kMaxNotes; ++i)
            if (slots_[(size_t) i].used && slots_[(size_t) i].key == key && (slots_[(size_t) i].held || slots_[(size_t) i].sustained))
                releaseSlot(i);
    }
    const int index = takeSlot();
    if (index < 0) return;
    auto& slot = slots_[(size_t) index];
    slot = {};
    slot.used = true;
    slot.held = true;
    slot.key = key;
    slot.velocity = velocity;
    slot.order = ++counter_;
    slot.freeStart = freeBeats_;
    startSlot(index, false);
}

void Seraph::noteOff(int key)
{
    if (std::lround(value(Mode)) == Legato)
    {
        for (int i = 0; i < legatoCount_; ++i)
            if (legatoKeys_[(size_t) i] == key)
            {
                std::copy(legatoKeys_.begin() + i + 1, legatoKeys_.begin() + legatoCount_, legatoKeys_.begin() + i);
                --legatoCount_;
                break;
            }
        for (int i = 0; i < kMaxNotes; ++i)
        {
            auto& s = slots_[(size_t) i];
            if (! s.used || ! s.held || s.key != key) continue;
            if (legatoCount_ > 0) // back to the note still held, gliding
            {
                const int back = legatoKeys_[(size_t) legatoCount_ - 1];
                s.key = back;
                s.chantStep = -1;
                if (value(Chant) <= 0.5f)
                {
                    s.line = back;
                    for (size_t v = 0; v < kMaxSingers; ++v)
                        if (links_[v].slot == i)
                        {
                            const auto& part = s.parts[links_[v].part];
                            voices_[v].glideTo((float) (back + part.offset + part.fold), value(Glide));
                        }
                }
            }
            else if (sustain_) { s.held = false; s.sustained = true; }
            else releaseSlot(i);
        }
        return;
    }
    for (int i = 0; i < kMaxNotes; ++i)
    {
        auto& s = slots_[(size_t) i];
        if (! s.used || ! s.held || s.key != key) continue;
        if (sustain_) { s.held = false; s.sustained = true; }
        else releaseSlot(i);
    }
}

void Seraph::updateChant(double songBeats, bool playing)
{
    const bool chant = value(Chant) > 0.5f;
    const auto& list = phrases();
    const auto& phrase = list[(size_t) std::clamp((int) std::lround(value(PhraseParam)), 0, (int) list.size() - 1)];
    const double unitBeats = kChantRateBeats[std::clamp((int) std::lround(value(ChantRate)), 0, kChantRates - 1)];
    const int key = std::clamp((int) std::lround(value(Key)), 0, 11), scale = (int) std::lround(value(Scale));
    const float glide = value(Glide);
    int shownStep = -1;
    for (int i = 0; i < kMaxNotes; ++i)
    {
        auto& s = slots_[(size_t) i];
        if (! s.used || ! (s.held || s.sustained)) continue;
        int line = s.key;
        long long absolute = -1;
        if (chant)
        {
            int step = 0;
            phraseStepAt(phrase, clockFor(s, songBeats, playing) / unitBeats, step, absolute);
            line = chantPitch(s.key, phrase, step, key, scale);
            if (shownStep < 0) shownStep = step;
        }
        if (absolute == s.chantStep && line == s.line) continue;
        const bool newSyllable = chant && s.chantStep >= 0 && absolute != s.chantStep;
        s.chantStep = absolute;
        s.line = line;
        for (size_t v = 0; v < kMaxSingers; ++v)
        {
            if (links_[v].slot != i) continue;
            const auto& part = s.parts[links_[v].part];
            voices_[v].glideTo((float) (line + part.offset + part.fold), glide);
            if (newSyllable) voices_[v].rearticulate(noteOnset(absolute), noteVowelStep(absolute));
        }
    }
    chantStep_.store(shownStep, std::memory_order_relaxed);
}

void Seraph::updateIson()
{
    auto& ison = slots_[kMaxNotes];
    bool anyHeld = false;
    for (int i = 0; i < kMaxNotes; ++i) anyHeld = anyHeld || (slots_[(size_t) i].used && (slots_[(size_t) i].held || slots_[(size_t) i].sustained));
    const bool want = anyHeld && std::lround(value(Ison)) != IsonOff;
    if (want && ! ison.held)
    {
        ison = {};
        ison.used = ison.held = ison.ison = true;
        ison.velocity = 0.7f;
        ison.order = ++counter_;
        startSlot(kMaxNotes, true);
    }
    else if (! want && ison.held)
        releaseSlot(kMaxNotes);
}

// ---- audio

void Seraph::readShared()
{
    auto& s = shared_;
    s.sampleRate = sampleRate_;
    s.tables = tables_.get();
    s.vowel = value(Vowel);
    s.hum = value(Hum);
    s.size = 1.0f - value(Size); // Size + is a larger throat: lower formants, darker
    s.brightDb = value(Bright);
    s.motion = value(VowelMotion);
    s.motionRate = value(MotionRate);
    s.tension = value(Tension);
    s.breath = value(Breath);
    s.effort = value(Effort);
    s.vibratoCents = value(Vibrato);
    s.vibratoRate = value(VibratoRate);
    s.vibratoDelayMs = value(VibratoDelay);
    s.driftCents = value(Drift);
    s.human = value(Human);
    s.scoopCents = value(Scoop);
    s.attackMs = value(Attack);
    s.releaseMs = value(Release);
    s.swell = value(Swell);
    s.dynamics = value(Dynamics);
    s.bendSemitones = bend_;
    s.modVibratoCents = modWheel_ * 40.0f;
}

void Seraph::renderVoices(float* left, float* right, int n)
{
    for (auto& v : voices_)
        if (! v.idle()) v.render(left, right, n, shared_);
}

void Seraph::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    const int n = audio.getNumSamples();
    if (n <= 0 || tables_ == nullptr) return;
    juce::ScopedNoDenormals noDenormals;
    if (const auto* pending = realPending_.load(std::memory_order_acquire); pending != realBank_ && realOldBank_ == nullptr)
    {
        // A new bank (taken only once no singer reads the bank before the current one): singers reading a recording the new
        // bank no longer has fade out in a few milliseconds, and the old bank stays until they have.
        realOldBank_ = realBank_;
        realBank_ = pending;
        realInUse_.store(realBank_, std::memory_order_release);
        realPrevious_.store(realOldBank_, std::memory_order_release);
        for (auto& v : voices_)
            if (const auto* r = v.real(); r != nullptr)
            {
                bool kept = false;
                if (realBank_ != nullptr)
                    for (const auto& owned : realBank_->voices) kept = kept || owned.get() == r;
                if (! kept) v.kill();
            }
    }
    if (realOldBank_ != nullptr)
    {
        bool reading = false;
        for (const auto& v : voices_)
            if (const auto* r = v.real(); r != nullptr)
                for (const auto& owned : realOldBank_->voices) reading = reading || owned.get() == r;
        if (! reading)
        {
            realOldBank_ = nullptr;
            realPrevious_.store(nullptr, std::memory_order_release);
        }
    }
    readShared();
    const double tempo = std::clamp(host_.tempo > 0 ? host_.tempo : 120.0, 20.0, 999.0);
    const double beatsPerSample = tempo / 60.0 / sampleRate_;
    const bool playing = host_.playing;

    // gate and tone settings for the block
    const bool gateOn = value(Gate) > 0.5f;
    const bool gateAfter = std::lround(value(GatePosition)) == AfterSpace;
    {
        const int pattern = (int) std::lround(value(GatePattern));
        if (pattern >= kCustomPattern)
        {
            float levels[dsp::TranceGate::kMaxSteps];
            for (int i = 0; i < dsp::TranceGate::kMaxSteps; ++i) levels[i] = custom_[(size_t) i].load(std::memory_order_relaxed);
            gate_.setPattern(levels, customSteps_.load(std::memory_order_relaxed));
        }
        else
        {
            const auto& p = dsp::TranceGate::factory()[(size_t) std::clamp(pattern, 0, kCustomPattern - 1)];
            gate_.setPattern(p.level, p.steps);
        }
        gate_.setShape((int) std::lround(value(GateRate)), value(GateLength), value(GateSmooth), value(GateDepth), value(GateSwing));
    }
    designHighPass(lowCut_, sampleRate_, value(LowCut), 0.7071);
    designPeak(warmth_, sampleRate_, 250.0, 0.8, value(Warmth));
    designHighShelf(air_, sampleRate_, 8000.0, value(Air));
    designHighPass(wetHighPass_, sampleRate_, 300.0, 0.7071);
    const float space = value(Space), distance = value(Distance);
    const int size = std::clamp((int) std::lround(value(SpaceSize)), 0, kSpaceSizes - 1);
    if (size != hallSize_ || std::abs(distance - hallDistance_) > 1.0e-4f)
    {
        serum_hall::Parameters hp;
        hp.size = kHallSettings[size].size;
        hp.decay = kHallSettings[size].decay;
        hp.low = 0.0f;
        hp.high = 0.25f + 0.55f * distance;
        hp.rate = 0.2f;
        hp.depth = 0.25f;
        hp.preDelaySeconds = (100.0f - 90.0f * distance) * 0.001f;
        hall_.setParameters(hp);
        hall_.setMix(1.0f, 1.0f);
        hallSize_ = size;
        hallDistance_ = distance;
    }
    const float wetGain = space > 0.0f ? std::sin(space * 1.5707963f) * kHallSettings[size].wet : 0.0f;
    const float dryGain = std::cos(space * 1.5707963f) * (1.0f - 0.7f * distance);
    const float distanceCoef = (float) std::exp(-2.0 * kPi * (18000.0 - 13000.0 * distance) / sampleRate_);
    const float output = juce::Decibels::decibelsToGain(value(Output));
    float* outLeft = audio.getWritePointer(0);
    float* outRight = audio.getNumChannels() > 1 ? audio.getWritePointer(1) : nullptr;

    auto event = midi.cbegin();
    double lastGateBeat = 0;
    for (int start = 0; start < n;)
    {
        segmentOffsetBeats_ = start * beatsPerSample;
        for (; event != midi.cend() && (*event).samplePosition <= start; ++event)
        {
            const auto m = (*event).getMessage();
            if (isBypassed()) continue;
            if (m.isNoteOn()) noteOn(m.getNoteNumber(), m.getFloatVelocity());
            else if (m.isNoteOff()) noteOff(m.getNoteNumber());
            else if (m.isSustainPedalOn()) sustain_ = true;
            else if (m.isSustainPedalOff())
            {
                sustain_ = false;
                for (int i = 0; i < kMaxNotes; ++i)
                    if (slots_[(size_t) i].used && slots_[(size_t) i].sustained) releaseSlot(i);
            }
            else if (m.isPitchWheel()) bend_ = (float) (m.getPitchWheelValue() - 8192) / 8192.0f * 2.0f;
            else if (m.isControllerOfType(1)) modWheel_ = (float) m.getControllerValue() / 127.0f;
            else if (m.isAllNotesOff() || m.isAllSoundOff())
            {
                for (int i = 0; i <= kMaxNotes; ++i)
                    if (slots_[(size_t) i].used) releaseSlot(i);
                if (m.isAllSoundOff())
                    for (auto& v : voices_) v.kill();
                legatoCount_ = 0;
            }
        }
        if (isBypassed())
            for (int i = 0; i <= kMaxNotes; ++i)
                if (slots_[(size_t) i].used && (slots_[(size_t) i].held || slots_[(size_t) i].sustained)) releaseSlot(i);
        int len = std::min(kSegment, n - start);
        if (event != midi.cend()) len = std::min(len, std::max(1, (*event).samplePosition - start));
        const double songBeat = host_.beats + start * beatsPerSample;
        updateChant(songBeat, playing);
        updateIson();
        for (int i = 0; i <= kMaxNotes; ++i) // a slot whose singers all finished (and no longer held) is free again
        {
            auto& s = slots_[(size_t) i];
            if (! s.used || s.held || s.sustained) continue;
            bool sounding = false;
            for (size_t v = 0; v < kMaxSingers && ! sounding; ++v) sounding = links_[v].slot == i && ! voices_[v].idle();
            if (! sounding)
            {
                for (auto& l : links_)
                    if (l.slot == i) l.slot = -1;
                s.used = false;
            }
        }

        bool anyVoice = false;
        for (const auto& v : voices_) anyVoice = anyVoice || ! v.idle();
        quietBlocks_ = anyVoice ? 0 : quietBlocks_ + len;
        const int tail = (int) ((kSpaceSeconds[size] * 1.6f + 0.5f) * (float) sampleRate_);
        if (quietBlocks_ > tail || isBypassed())
        {
            freeBeats_ += len * beatsPerSample;
            start += len;
            continue;
        }
        std::fill(left_.begin(), left_.begin() + len, 0.0f);
        std::fill(right_.begin(), right_.begin() + len, 0.0f);
        if (anyVoice) renderVoices(left_.data(), right_.data(), len);

        const double gateBeat0 = playing ? songBeat : freeBeats_ - gateStart_;
        for (int i = 0; i < len; ++i)
        {
            float l = left_[(size_t) i], r = right_[(size_t) i];
            float g = 1.0f;
            if (gateOn)
            {
                lastGateBeat = gateBeat0 + i * beatsPerSample;
                g = gate_.next(lastGateBeat);
            }
            if (gateOn && ! gateAfter) { l *= g; r *= g; }
            l = air_.process(warmth_.process(lowCut_.process(l, 0), 0), 0);
            r = air_.process(warmth_.process(lowCut_.process(r, 1), 1), 1);
            if (space > 0.0f)
            {
                distanceLp_[0] = l + distanceCoef * (distanceLp_[0] - l);
                distanceLp_[1] = r + distanceCoef * (distanceLp_[1] - r);
                float wl = 0, wr = 0;
                hall_.process(distanceLp_[0], distanceLp_[1], wl, wr);
                wl = wetHighPass_.process(wl, 0);
                wr = wetHighPass_.process(wr, 1);
                l = distanceLp_[0] * dryGain + wl * wetGain;
                r = distanceLp_[1] * dryGain + wr * wetGain;
            }
            if (gateOn && gateAfter) { l *= g; r *= g; }
            l = softClip(l * output);
            r = softClip(r * output);
            if (outRight != nullptr)
            {
                outLeft[start + i] += l;
                outRight[start + i] += r;
            }
            else outLeft[start + i] += 0.5f * (l + r);
        }
        freeBeats_ += len * beatsPerSample;
        start += len;
    }
    segmentOffsetBeats_ = 0;
    gateStep_.store(gateOn ? gate_.stepAt(lastGateBeat) : -1, std::memory_order_relaxed);
    updateFaceState();
}

void Seraph::updateFaceState()
{
    int active = 0;
    float lowest = 0;
    for (size_t i = 0; i < kMaxSingers; ++i)
    {
        const auto& v = voices_[i];
        const bool idle = v.idle();
        viewType_[i].store(idle ? -1 : (v.boys() ? kVoiceTypes : v.type()), std::memory_order_relaxed);
        viewLevel_[i].store(v.level(), std::memory_order_relaxed);
        viewPan_[i].store(v.pan(), std::memory_order_relaxed);
        viewPitch_[i].store(v.targetPitch(), std::memory_order_relaxed);
        viewHz_[i].store(idle ? 0.0f : v.currentHz(), std::memory_order_relaxed);
        viewReal_[i].store(v.real(), std::memory_order_relaxed);
        if (idle) continue;
        ++active;
        const float hz = v.currentHz();
        if (hz > 0 && (lowest <= 0 || hz < lowest)) lowest = hz;
    }
    activeSingers_.store(active, std::memory_order_relaxed);
    lowestHz_.store(lowest, std::memory_order_relaxed);
}

Seraph::SingerView Seraph::singer(int index) const
{
    SingerView view;
    if (index < 0 || index >= kMaxSingers) return view;
    view.type = viewType_[(size_t) index].load(std::memory_order_relaxed);
    view.level = viewLevel_[(size_t) index].load(std::memory_order_relaxed);
    view.pan = viewPan_[(size_t) index].load(std::memory_order_relaxed);
    view.pitch = viewPitch_[(size_t) index].load(std::memory_order_relaxed);
    view.hz = viewHz_[(size_t) index].load(std::memory_order_relaxed);
    view.real = viewReal_[(size_t) index].load(std::memory_order_relaxed);
    return view;
}

std::vector<DeviceProcessor::UiGroup> Seraph::uiGroups() const
{
    return {{"Choir", {Section, Singers, MenOctave, Fold, Mode}, 3},
            {"Mouth", {Vowel, Hum, Size, Bright, VowelMotion, MotionRate, VowelSteps, Onset}, 4},
            {"Voice", {Tension, Breath, Effort}, 3},
            {"Human", {Vibrato, VibratoRate, VibratoDelay, Drift, Detune, Timing, Scoop, Human, SizeSpread}, 5},
            {"Envelope", {Attack, Release, Swell, Glide, Dynamics}, 3},
            {"Chant", {Chant, Key, Scale, PhraseParam, ChantRate, Organum, Ison, IsonLevel}, 4},
            {"Gate", {Gate, GatePattern, GateRate, GateLength, GateSmooth, GateDepth, GateSwing, GatePosition}, 4},
            {"Space", {LowCut, Air, Warmth, Space, SpaceSize, Distance, Width, Output, Seed}, 5}};
}
} // namespace sonora::devices

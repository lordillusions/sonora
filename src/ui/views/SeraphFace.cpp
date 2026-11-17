#include "ui/views/SeraphFace.h"

#include "core/Scales.h"
#include "devices/native/SeraphFormants.h"
#include "devices/native/SeraphGlottis.h"

#include "include/core/SkPathBuilder.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <random>

namespace sonora::ui
{
namespace
{
using S = devices::Seraph;
namespace sf = devices::seraph;
constexpr const char* kVowelLetters[5] = {"A", "E", "I", "O", "U"};
constexpr const char* kVowelSounds[5] = {"aah", "eh", "ee", "oh", "ooh"};
constexpr SkColor kTypeColours[5] = {0xFFB5895A, 0xFFD9A55E, 0xFFEFC97E, 0xFFFFE6A8, 0xFFFFF4D6}; // bass .. soprano, boys

std::string format(const char* pattern, double value)
{
    char buffer[96];
    std::snprintf(buffer, sizeof buffer, pattern, value);
    return buffer;
}

float hzToX(float hz, const Rect& r)
{
    const float t = std::log(std::clamp(hz, 60.0f, 6000.0f) / 60.0f) / std::log(100.0f);
    return r.x + t * r.w;
}

// The composite formant response (dB) at f.
float responseDb(const sf::FormantSet& set, float f)
{
    std::complex<float> h{};
    for (int i = 0; i < sf::kFormants; ++i)
        h += (i % 2 == 0 ? 1.0f : -1.0f) * set.gain[i] * std::complex<float>(0, f * set.bw[i]) / std::complex<float>(set.hz[i] * set.hz[i] - f * f, f * set.bw[i]);
    return 20.0f * std::log10(std::max(1e-6f, std::abs(h)));
}
} // namespace

SeraphFace::SeraphFace(devices::Seraph& device) : ProFace(device, kFull, kCompact, kAccent), seraph_(device)
{
    gateLane_.name = "Gate steps";
    gateLane_.accent = kAccent;
    gateLane_.kind = StepLane::Kind::Bar;
    gateLane_.steps = seraph_.gateSteps();
    gateLane_.defaultValue = 1.0f;
    gateLane_.get = [this](int step) { return seraph_.gateLevel(step); };
    gateLane_.set = [this](int step, float level) { editGateStep(step, level); };
    gateLane_.playStep = [this] { return value(S::Gate) > 0.5f ? seraph_.gateStepNow() : -1; };
    gateLane_.changed = [this] { repaint(); };

    presetBar_.name = "Preset";
    presetBar_.accent = kAccent;
    for (const auto& p : S::presets()) presetBar_.presets.push_back({p.category, p.name});
    presetBar_.current = [this] { return seraph_.matchingPreset(); };
    presetBar_.select = [this](int index) {
        seraph_.applyPreset(index);
        repaint();
    };
    presetBar_.dice = [this] {
        static std::mt19937 rng(std::random_device{}());
        seraph_.applyPreset((int) (rng() % S::presets().size()));
        repaint();
    };
    presetBar_.changed = [this] { repaint(); };
}

bool SeraphFace::browserDrop(const BrowserDrag& payload, const MouseEvent&)
{
    if (payload.kind != BrowserDrag::Kind::Sample || payload.value.empty()) return false;
    loadVoices({payload.value});
    return true;
}

void SeraphFace::loadVoices(const std::vector<std::string>& paths)
{
    if (paths.empty()) return;
    if (onVoicesDropped)
    {
        onVoicesDropped(paths);
        repaint();
        return;
    }
    realMessage.clear();
    std::vector<std::string> errors;
    const int loaded = seraph_.addRealFiles(paths, &errors);
    if (! errors.empty()) realMessage = errors.back();
    if (loaded > 0) setValue(S::Source, (float) S::RealSource);
    repaint();
}

std::string SeraphFace::headline() const
{
    const int section = std::clamp((int) std::lround(value(S::Section)), 0, S::kSections - 1);
    static const char* const kNames[S::kSections] = {"Men", "Women", "Mixed choir", "Boys", "Solo bass", "Solo tenor", "Solo alto", "Solo soprano"};
    std::string out = kNames[section];
    if (section == S::Mixed && value(S::MenOctave) > 0.5f) out += ", men 8vb";
    if (section < S::SoloBass) out += "  \xC2\xB7  " + std::to_string((int) std::lround(value(S::Singers))) + " a note";
    if (std::lround(value(S::Mode)) == S::Legato) out += "  \xC2\xB7  legato";
    if (std::lround(value(S::Source)) == S::RealSource) out += "  \xC2\xB7  real";
    const int singing = seraph_.activeSingers();
    out += "  \xC2\xB7  " + (singing > 0 ? std::to_string(singing) + " singing" : std::string("silent"));
    return out;
}

float SeraphFace::vowelX(float v) const { return vowel_.x + 14.0f + std::clamp(v, 0.0f, 4.0f) / 4.0f * (vowel_.w - 28.0f); }

std::string SeraphFace::valueText(int param, float v) const
{
    switch (param)
    {
        case S::Vowel:
        {
            const int near = std::clamp((int) std::lround(v), 0, 4);
            if (std::abs(v - (float) near) < 0.12f) return kVowelLetters[near];
            const int a = std::min((int) v, 3);
            return std::string(kVowelLetters[a]) + "\xE2\x86\x92" + kVowelLetters[a + 1];
        }
        case S::Vibrato:
        case S::Drift:
        case S::Detune:
        case S::Scoop: return format("%.0f ct", v);
        case S::Size: return std::abs(v) < 0.005f ? "0%" : format("%+.0f%%", v * 100.0);
        case S::SizeSpread: return format("\xC2\xB1%.0f%%", v * 100.0);
        case S::MotionRate: return v < 0.995f ? format("%.2f Hz", v) : format("%.1f Hz", v);
        case S::VibratoRate: return format("%.1f Hz", v);
        default: return ProFace::valueText(param, v);
    }
}

std::string SeraphFace::idleHelp() const
{
    if (std::lround(value(S::Source)) == S::RealSource)
        return seraph_.realVoices().empty() ? "Real: drop a sung note (a dry, held vowel) on Seraph; the synthesised voices sing until then."
                                            : "Real: your recordings sing every note, each singer reading them its own way.";
    if (value(S::Chant) > 0.5f)
        return "Hold a note: it is the tonic, and the singers chant the phrase around it in the key, locked to the bars.";
    if (std::lround(value(S::Mode)) == S::Legato) return "Play one line: overlapping notes glide without a new attack.";
    return "Play chords for a choir. Hold a note with Chant on for a phrase around it; Gate chops it in time.";
}

int SeraphFace::spectrumType() const
{
    for (int v = 0; v < S::kMaxSingers; ++v)
        if (const auto view = seraph_.singer(v); view.type >= 0) return view.type;
    const auto types = seraph_.typesFor(57);
    return types.empty() ? sf::Tenor : types.front();
}

void SeraphFace::addSlider(const Rect& r, int param, std::string label, std::string tip)
{
    sliders_.push_back({r, param, label});
    auto& area = addArea(r, std::move(tip));
    area.param = param;
    area.label = std::move(label);
    const auto set = [this, r, param](const MouseEvent& e) {
        if (e.alt) { setValue(param, info(param).defaultValue); return; }
        const auto& i = info(param);
        const float t = std::clamp((e.x - r.x) / r.w, 0.0f, 1.0f);
        setValue(param, i.minValue + t * (i.maxValue - i.minValue));
        repaint();
    };
    area.down = set;
    area.drag = set;
}

void SeraphFace::addVowelStrip(const Rect& r)
{
    vowel_ = r;
    auto& area = addArea(r, "Vowel: drag along A E I O U (Alt-click: back to A). The band shows how far Vowel Motion takes it.");
    area.param = S::Vowel;
    area.label = "Vowel";
    const auto set = [this](const MouseEvent& e) {
        if (e.alt) { setValue(S::Vowel, 0.0f); return; }
        setValue(S::Vowel, std::clamp((e.x - vowel_.x - 14.0f) / (vowel_.w - 28.0f), 0.0f, 1.0f) * 4.0f);
        repaint();
    };
    area.down = set;
    area.drag = set;
}

void SeraphFace::selectPattern(int index)
{
    const int count = S::kCustomPattern + 1;
    index = ((index % count) + count) % count;
    setValue(S::GatePattern, (float) index);
    if (index < S::kCustomPattern)
        if (const int rate = dsp::TranceGate::factory()[(size_t) index].rate; rate >= 0) setValue(S::GateRate, (float) rate);
    repaint();
}

void SeraphFace::editGateStep(int step, float level)
{
    // Editing a factory pattern starts a custom one from it.
    if ((int) std::lround(value(S::GatePattern)) < S::kCustomPattern)
    {
        float levels[dsp::TranceGate::kMaxSteps];
        const int steps = seraph_.gateSteps();
        for (int i = 0; i < steps; ++i) levels[i] = seraph_.gateLevel(i);
        seraph_.setCustomGate(levels, steps);
        setValue(S::GatePattern, (float) S::kCustomPattern);
    }
    seraph_.setCustomGateStep(step, level);
    repaint();
}

void SeraphFace::addPatternChooser(const Rect& r)
{
    pattern_ = r;
    auto& area = addArea(r, "Gate pattern: click the arrows (or the name) for the next one; Custom is yours, edited on the steps.");
    area.param = S::GatePattern;
    area.label = "Gate pattern";
    area.down = [this](const MouseEvent& e) {
        const int now = (int) std::lround(value(S::GatePattern));
        selectPattern(e.x < pattern_.x + 24 || e.shift ? now - 1 : now + 1);
    };
}

void SeraphFace::build(Layout layout)
{
    sliders_.clear();
    stalls_ = vowel_ = spectrum_ = phrase_ = pattern_ = scale_ = {};
    const auto sectionSegments = [&](const Rect& row1, const Rect& row2, float text) {
        static const char* const kShort[S::kSections] = {"Men", "Women", "Mixed", "Boys", "Bass", "Tenor", "Alto", "Soprano"};
        static const char* const kTips[S::kSections] = {
            "Men: basses and tenors.", "Women: altos and sopranos.", "Mixed: all four voice types share the range.",
            "Boys: trebles, clear and light.", "Solo bass: one singer.", "Solo tenor: one singer.", "Solo alto: one singer.",
            "Solo soprano: one singer."};
        for (int i = 0; i < S::kSections; ++i)
        {
            const Rect& row = i < 4 ? row1 : row2;
            const float w = (row.w - 12.0f) / 4.0f;
            addSegment({row.x + (float) (i % 4) * (w + 4.0f), row.y, w, row.h}, S::Section, (float) i, kShort[i], kTips[i]).textSize = text;
        }
    };

    if (layout == Layout::Compact)
    {
        sectionSegments({8, 8, 236, 24}, {8, 36, 236, 24}, 10.0f);
        addVowelStrip({8, 68, 236, 36});
        addToggle({8, 112, 64, 24}, S::Gate, "Gate", "Gate: chops the choir in time with the song.");
        addPatternChooser({76, 112, 168, 24});
        stalls_ = {252, 8, 196, 154};
        addArea(stalls_, "The choir: one dot per singer, placed where you hear it, glowing as it sings. Open the window for everything.");
        addKnob({456, 4, 72, 80}, S::Singers, "SINGERS", "Singers on every note.");
        addKnob({532, 4, 72, 80}, S::Breath, "BREATH", "Breath: air in the voices.");
        addKnob({608, 4, 72, 80}, S::Vibrato, "VIBRATO", "Vibrato depth.");
        addKnob({456, 88, 72, 80}, S::Space, "SPACE", "Space: how much of the hall you hear.");
        addKnob({532, 88, 72, 80}, S::Output, "OUTPUT", "Output level.");
        addKnob({608, 88, 72, 80}, S::Attack, "ATTACK", "Attack: how slowly the voices come in.");
        addSegments({684, 8, 68, 154}, S::Mode, {"Poly", "Legato"}, true, 4, "Poly for chords, Legato for one gliding line.");
        return;
    }

    // ---- header
    addPart(presetBar_, {200, 14, 430, 32}, "Presets: the arrows step through them, the name opens the list by family, the dice picks one.");
    addSegments({642, 16, 150, 28}, S::Source, {"Voices", "Real"}, false, 3,
                "Voices: the synthesised singers. Real: recordings you load (drop a sung note from the Browser or the Finder) sing instead.");

    // ---- CHOIR
    sectionSegments({28, 90, 348, 26}, {28, 120, 348, 26}, 11.0f);
    stalls_ = {28, 154, 348, 112};
    addArea(stalls_, "The stalls: one dot per sounding singer, placed by pan, sized by voice type, glowing with its level.");
    std::vector<std::string> counts;
    for (int i = 1; i <= S::kMaxSingersPerNote; ++i) counts.push_back(std::to_string(i));
    addSegments({94, 274, 282, 24}, S::Singers, counts, false, 3, "Singers on every note (the solo sections always use one).");
    addToggle({28, 306, 120, 24}, S::MenOctave, "Men octave", "Mixed: the men double every note an octave below.");
    addToggle({152, 306, 76, 24}, S::Fold, "Fold", "Fold: a note out of every range is sung an octave in, rather than strained.");
    addSegments({232, 306, 144, 24}, S::Mode, {"Poly", "Legato"}, false, 4, "Poly for chords; Legato: one line, overlapping notes glide.");

    // ---- MOUTH
    addVowelStrip({408, 90, 432, 38});
    addSlider({460, 132, 380, 14}, S::Hum, "HUM", "Hum: towards a closed mouth (F1 down to 250 Hz, the upper formants 24 dB down).");
    spectrum_ = {408, 152, 432, 92};
    lastSource_ = (int) std::lround(value(S::Source));
    if (lastSource_ == S::RealSource)
    {
        addArea(spectrum_, "Real voices: the recordings that sing. Drop a sung note here (from the Browser or the Finder).");
        // PLG-27h: the Seraph Voices pack's singers, one click each; or the way to get the pack
        seraph_.packsRoot = packsRoot;
        pack_ = devices::Seraph::findPackVoices(packsRoot);
        if (! pack_.folder.empty())
        {
            const int count = std::min((int) pack_.singers.size(), 6);
            for (int i = 0; i < count; ++i)
            {
                const auto name = pack_.singers[(size_t) i].name;
                const std::string chip = (name.rfind("Female", 0) == 0 ? "W" : name.rfind("Male", 0) == 0 ? "M" : "") + name.substr(name.find_last_of(' ') + 1);
                auto& b = addButton({450 + (float) i * 38, 156, 34, 18}, chip, [this, name] {
                    const auto pack = devices::Seraph::findPackVoices(packsRoot);
                    for (const auto& s : pack.singers)
                        if (s.name == name) loadVoices(s.files);
                }, "Seraph Voices pack: load " + name + "'s sung notes (VocalSet, CC BY 4.0: music made with them needs the credit line).");
                b.textSize = 9.5f;
                b.lit = [this, name] {
                    for (const auto& path : seraph_.realPaths())
                        if (path.find("/" + name + "/") != std::string::npos) return true;
                    return false;
                };
            }
        }
        else if (onGetPack)
            addButton({620, 156, 150, 18}, "Get the Seraph Voices pack", [this] { if (onGetPack) onGetPack(); },
                      "Download the optional Seraph Voices pack: recorded singers from VocalSet (CC BY 4.0). Music made with them needs the "
                      "credit line shown here.").textSize = 9.5f;
        if (seraph_.packVoicesLoaded(packsRoot))
            addButton({776, 208, 58, 22}, "Copy credit", [this] { juce::SystemClipboard::copyTextToClipboard(juce::String::fromUTF8(packCredit().c_str())); },
                      "Copy the credit line: music made with the pack's voices must carry it where credits go.").textSize = 9.5f;
        if (onChooseVoices)
            addButton({776, 156, 58, 22}, "Load\xE2\x80\xA6", [this] { if (onChooseVoices) onChooseVoices(); },
                      "Load sung notes from files (or drop them anywhere on Seraph).").textSize = 10.5f;
        addButton({776, 182, 58, 22}, "Clear", [this] {
            if (onClearVoices) onClearVoices();
            else seraph_.clearRealVoices();
            realMessage.clear();
            repaint();
        }, "Clear: forget the loaded recordings.").textSize = 10.5f;
    }
    else
        addArea(spectrum_, "The mouth: the formant curve of the vowel and the harmonics of the lowest note singing.");
    addSegments({470, 252, 370, 22}, S::VowelSteps, {"Off", "A-O", "O-U", "A-E-O", "A-O-U", "AEIOU", "Rnd"}, false, 3,
                "Vowel Steps: the vowel changes from note to note in this order, around Vowel.");
    addSegments({470, 280, 370, 22}, S::Onset, {"Soft", "H", "M", "N", "L", "Y", "W", "M-N-L", "Rnd"}, false, 3,
                "Onset: how each note begins: plain, a breath, a hum (m, n), l, or a glide from ee (y) or oo (w).");
    addSlider({470, 308, 170, 20}, S::VowelMotion, "MOTION", "Vowel Motion: a slow drift of the vowel, each singer on its own path.");
    addSlider({706, 308, 134, 20}, S::MotionRate, "RATE", "Motion Rate: how fast the vowel drifts.");

    // ---- VOICE
    const auto knob = [&](float x, float y, float w, float h, int param, const char* label, const char* tip) -> Control& {
        return addKnob({x, y, w, h}, param, label, tip);
    };
    knob(872, 88, 86, 118, S::Tension, "TENSION", "Tension: breathy (open, soft) to pressed (bright, ringing).").caption = [this] {
        const float t = value(S::Tension);
        return std::string(t < 0.35f ? "breathy" : t > 0.65f ? "pressed" : "modal");
    };
    knob(962, 88, 86, 118, S::Breath, "BREATH", "Breath: air through the voices, louder while the throat is open.").caption = [] { return std::string("air"); };
    knob(1052, 88, 86, 118, S::Effort, "EFFORT", "Effort: velocity moves tension and brightness (soft notes breathier and darker).")
        .caption = [] { return std::string("velocity"); };
    knob(872, 210, 86, 118, S::Size, "SIZE", "Size: larger throats (darker) or smaller ones (brighter, younger).").caption = [this] {
        const float s = value(S::Size);
        return std::string(s > 0.02f ? "larger" : s < -0.02f ? "smaller" : "natural");
    };
    knob(962, 210, 86, 118, S::Bright, "BRIGHT", "Bright: tilts the upper formants up or down.").bipolar = true;
    knob(1052, 210, 86, 118, S::SizeSpread, "SPREAD", "Size Spread: how different the singers' throats are.").caption = [] { return std::string("throats"); };

    // ---- HUMAN, ENVELOPE, OUT
    const std::tuple<int, const char*, const char*> human[8] = {
        {S::Vibrato, "VIBRATO", "Vibrato depth in cents (each singer a little different)."},
        {S::VibratoRate, "RATE", "Vibrato rate."},
        {S::VibratoDelay, "DELAY", "Vibrato Delay: the vibrato starts this long after the note and fades in."},
        {S::Drift, "DRIFT", "Drift: slow wandering of the pitch, like a singer holding a long note."},
        {S::Detune, "DETUNE", "Detune: how far apart the singers are tuned."},
        {S::Timing, "TIMING", "Timing: the singers start up to this late."},
        {S::Scoop, "SCOOP", "Scoop: notes start this far flat and slide up to pitch."},
        {S::Human, "HUMAN", "Human: jitter and shimmer, the tiny period-to-period wobble of a real voice."}};
    static const char* const kHumanCaptions[8] = {"depth", "per second", "before it", "wander", "apart", "late starts", "from below", "jitter"};
    for (int i = 0; i < 8; ++i)
        knob(26 + (float) i * 67, 372, 64, 122, std::get<0>(human[i]), std::get<1>(human[i]), std::get<2>(human[i])).caption = [i] {
            return std::string(kHumanCaptions[i]);
        };
    const std::tuple<int, const char*, const char*> envelope[5] = {
        {S::Attack, "ATTACK", "Attack: how slowly the voices come in."},
        {S::Release, "RELEASE", "Release: how long they take to fade."},
        {S::Swell, "SWELL", "Swell: a held note grows in level and brightness over two seconds."},
        {S::Glide, "GLIDE", "Glide: time between notes in Legato and between chant notes."},
        {S::Dynamics, "DYNAMICS", "Dynamics: how much velocity changes the level."}};
    static const char* const kEnvelopeCaptions[5] = {"fade in", "fade out", "grows", "between notes", "velocity"};
    for (int i = 0; i < 5; ++i)
        knob(588 + (float) i * 66, 372, 64, 122, std::get<0>(envelope[i]), std::get<1>(envelope[i]), std::get<2>(envelope[i])).caption = [i] {
            return std::string(kEnvelopeCaptions[i]);
        };
    knob(938, 372, 64, 122, S::Width, "WIDTH", "Width: how far the singers spread left and right.").caption = [] { return std::string("stereo"); };
    knob(1006, 372, 64, 122, S::LowCut, "LOW CUT", "Low Cut: removes rumble under the voices.").caption = [] { return std::string("rumble"); };
    knob(1074, 372, 64, 122, S::Output, "OUTPUT", "Output level (soft-clipped at the top).").caption = [] { return std::string("level"); };

    // ---- CHANT
    addToggle({290, 516, 90, 20}, S::Chant, "Chant on", "Chant: every held note becomes a tonic the singers chant a phrase around.");
    phrase_ = {28, 540, 348, 50};
    {
        auto& area = addArea(phrase_, "Phrase: the notes the chant sings around the held note (steps of the scale). Click for the next one.");
        area.param = S::PhraseParam;
        area.label = "Phrase";
        area.down = [this](const MouseEvent& e) {
            const int count = (int) S::phrases().size();
            const int now = (int) std::lround(value(S::PhraseParam));
            const int next = e.x < phrase_.x + 24 || e.shift ? now - 1 : now + 1;
            setValue(S::PhraseParam, (float) (((next % count) + count) % count));
            repaint();
        };
    }
    addSegments({28, 596, 348, 22}, S::Key, std::vector<std::string>(S::kKeyNames, S::kKeyNames + 12), false, 2, "Key: the chant's tonic note names and the ison's root.");
    scale_ = {28, 624, 200, 22};
    {
        auto& area = addArea(scale_, "Scale of the chant: click for the next (Shift: the previous).");
        area.param = S::Scale;
        area.label = "Scale";
        area.down = [this](const MouseEvent& e) {
            const int count = (int) model::kScales.size();
            const int next = (int) std::lround(value(S::Scale)) + (e.shift || e.x < scale_.x + 24 ? -1 : 1);
            setValue(S::Scale, (float) (((next % count) + count) % count));
            repaint();
        };
    }
    addSegments({234, 624, 142, 22}, S::ChantRate, {"1/4", "1/2", "1", "2"}, false, 2, "Chant Rate: bars per unit of the phrase (longer is slower).");
    addSegments({28, 652, 348, 22}, S::Organum, {"Off", "4th", "5th", "8ve", "5+8", "8vb"}, false, 2,
                "Organum: extra voices in exact parallel (a fourth, a fifth, an octave above, both, an octave below).");
    addSegments({28, 680, 200, 22}, S::Ison, {"No ison", "Root", "Root+5"}, false, 2, "Ison: a held drone on the key's root in the basses while you hold a note.");
    addSlider({262, 684, 114, 16}, S::IsonLevel, "LEVEL", "Ison Level.");

    // ---- GATE
    addToggle({754, 516, 90, 20}, S::Gate, "Gate on", "Gate: chops the choir in time with the song.");
    addPart(gateLane_, {408, 540, 432, 60}, "Gate steps: drag to paint each step's level (Alt-click: full). Editing a factory pattern starts Custom.");
    addPatternChooser({408, 606, 260, 22});
    addSegments({676, 606, 164, 22}, S::GatePosition, {"Pre space", "Post space"}, false, 3,
                "Before the space the hall's tail fills the gaps; after it the cuts are hard.");
    addSegments({408, 634, 432, 22}, S::GateRate, std::vector<std::string>(dsp::TranceGate::kRateNames, dsp::TranceGate::kRateNames + dsp::TranceGate::kRates), false, 3,
                "Gate Rate: the length of a step.");
    addSlider({460, 664, 160, 16}, S::GateLength, "LENGTH", "Gate Length: how much of each step is open.");
    addSlider({680, 664, 160, 16}, S::GateSmooth, "SMOOTH", "Gate Smooth: attack and release of every step.");
    addSlider({460, 688, 160, 16}, S::GateDepth, "DEPTH", "Gate Depth: 100 % closes fully, less leaves some voice in the gaps.");
    addSlider({680, 688, 160, 16}, S::GateSwing, "SWING", "Gate Swing: every second step comes later.");

    // ---- SPACE
    addSegments({872, 540, 260, 22}, S::SpaceSize, {"Room", "Chapel", "Church", "Cath.", "Endless"}, false, 3,
                "Space Size: from a room (1.2 s) to a cathedral (8 s) and endless (20 s).");
    knob(872, 572, 64, 122, S::Space, "SPACE", "Space: how much of the hall you hear.").caption = [] { return std::string("hall"); };
    knob(938, 572, 64, 122, S::Distance, "DISTANCE", "Distance: from the front row to the far end of the nave (less direct sound, darker).").caption = [] { return std::string("far away"); };
    knob(1004, 572, 64, 122, S::Air, "AIR", "Air: a shelf at 8 kHz.").caption = [] { return std::string("8 kHz"); };
    knob(1070, 572, 64, 122, S::Warmth, "WARMTH", "Warmth: a lift around 250 Hz.").caption = [] { return std::string("250 Hz"); };
}

bool SeraphFace::animate()
{
    bool dirty = false;
    if (const int steps = seraph_.gateSteps(); steps != lastSteps_)
    {
        lastSteps_ = steps;
        gateLane_.steps = steps;
        gateLane_.beatEvery = steps % 3 == 0 ? 3 : 4;
        dirty = true;
    }
    if (const auto count = seraph_.realVoices().size(); count != lastRealCount_) { lastRealCount_ = count; dirty = true; }
    if (layoutMode() == Layout::Full && (int) std::lround(value(S::Source)) != lastSource_)
    {
        rebuild(); // the mouth's display: the spectrum or the Real panel
        dirty = true;
    }
    if (layoutMode() == Layout::Full && lastSource_ == S::RealSource && ++packScanTicks_ >= 30)
    {
        // the pack installed or removed meanwhile, its voices loaded or not: once a second
        packScanTicks_ = 0;
        const auto pack = devices::Seraph::findPackVoices(packsRoot);
        const std::string signature = pack.folder + "|" + std::to_string(pack.singers.size()) + "|" + (seraph_.packVoicesLoaded(packsRoot) ? "1" : "0")
                                      + "|" + seraph_.packWanted();
        if (signature != packSignature_)
        {
            packSignature_ = signature;
            rebuild();
            dirty = true;
        }
    }
    if (const int g = seraph_.gateStepNow(); g != lastGateStep_) { lastGateStep_ = g; dirty = true; }
    if (const int c = seraph_.chantStepNow(); c != lastChantStep_) { lastChantStep_ = c; dirty = true; }
    if (const int a = seraph_.activeSingers(); a != lastActive_) { lastActive_ = a; dirty = true; }
    for (int v = 0; v < S::kMaxSingers; ++v)
    {
        const float level = seraph_.singer(v).level;
        if (std::abs(level - lastLevels_[v]) > 0.02f) { lastLevels_[v] = level; dirty = true; }
    }
    return dirty;
}

// ------------------------------------------------------------------ paint

void SeraphFace::paintStalls(SkCanvas& c, const Rect& r, bool compact)
{
    pro::well(c, r, 8);
    // The stalls: four rows (sopranos front, basses back), drawn as soft arcs.
    static constexpr float kRowY[5] = {0.82f, 0.62f, 0.42f, 0.24f, 0.24f}; // bass, tenor, alto, soprano, boys
    for (int row = 0; row < 4; ++row)
    {
        SkPathBuilder arc;
        const float y = r.y + r.h * kRowY[row];
        arc.moveTo(r.x + 14, y + 6);
        arc.quadTo(r.x + r.w * 0.5f, y - 8, r.x + r.w - 14, y + 6);
        c.drawPath(arc.detach(), pro::stroke(0xFF1E2229, 1.0f));
    }
    int shown = 0;
    for (int v = 0; v < S::kMaxSingers; ++v)
    {
        const auto view = seraph_.singer(v);
        if (view.type < 0) continue;
        ++shown;
        const int type = std::clamp(view.type, 0, 4);
        const float x = r.x + 16 + (view.pan + 1.0f) * 0.5f * (r.w - 32);
        const float curve = -8.0f * (1.0f - std::pow(2.0f * (x - r.x) / r.w - 1.0f, 2.0f));
        const float y = r.y + r.h * kRowY[type] + curve + (float) ((v * 7) % 5 - 2) * (compact ? 1.5f : 2.5f);
        const float radius = (compact ? 0.8f : 1.0f) * (type == 0 ? 6.5f : type == 1 ? 5.8f : type == 2 ? 5.0f : type == 3 ? 4.4f : 4.0f);
        const float level = std::clamp(view.level, 0.0f, 1.0f);
        if (level > 0.05f) c.drawCircle(x, y, radius + 6.0f * level, pro::fill(paint::withAlpha(kAccent, (uint8_t) (70 * level))));
        c.drawCircle(x, y, radius, pro::fill(paint::withAlpha(kTypeColours[type], (uint8_t) (80 + 175 * level))));
    }
    if (shown == 0)
        pro::text(c, compact ? "play to hear the choir" : "the stalls are empty: play a chord", {r.x, r.y + r.h * 0.5f - 9, r.w, 18}, pro::kFaint,
                  compact ? 10.0f : 11.5f, Align::Centre);
    else if (! compact)
    {
        float lx = r.x + 10;
        static const char* const kLegend[4] = {"S", "A", "T", "B"};
        for (int i = 0; i < 4; ++i)
        {
            c.drawCircle(lx + 3, r.y + r.h - 10, 3, pro::fill(kTypeColours[3 - i]));
            pro::text(c, kLegend[i], {lx + 8, r.y + r.h - 17, 14, 14}, pro::kFaint, 9.5f, Align::Left, true);
            lx += 24;
        }
    }
}

void SeraphFace::paintVowelStrip(SkCanvas& c, const Rect& r, bool compact)
{
    pro::well(c, r, 6);
    const float vowelValue = value(S::Vowel), motion = value(S::VowelMotion);
    // the motion range
    if (motion > 0.01f)
    {
        const float lo = vowelX(std::max(0.0f, vowelValue - 1.2f * motion)), hi = vowelX(std::min(4.0f, vowelValue + 1.2f * motion));
        pro::rounded(c, {lo, r.y + 4, hi - lo, r.h - 8}, 4, paint::withAlpha(kAccent, 40));
    }
    const float marker = vowelX(vowelValue);
    c.drawLine(marker, r.y + 3, marker, r.y + r.h - 3, pro::stroke(paint::withAlpha(kAccent, 200), 2.0f));
    c.drawCircle(marker, r.y + 4, 3.5f, pro::fill(kAccent));
    for (int i = 0; i < 5; ++i)
    {
        const float x = vowelX((float) i);
        const bool near = std::abs(vowelValue - (float) i) < 0.5f;
        pro::text(c, kVowelLetters[i], {x - 20, r.y, 40, r.h * (compact ? 1.0f : 0.72f)}, near ? kAccent : pro::kFaint, compact ? 14.0f : 17.0f,
                  Align::Centre, true);
        if (! compact) pro::text(c, kVowelSounds[i], {x - 24, r.y + r.h * 0.62f, 48, r.h * 0.36f}, pro::kFaint, 9.0f, Align::Centre);
    }
}

void SeraphFace::paintSpectrum(SkCanvas& c, const Rect& r)
{
    pro::well(c, r, 6);
    const int type = spectrumType();
    const bool boys = type == sf::kVoiceTypes;
    float hz = seraph_.lowestHz();
    const bool live = hz > 0.0f;
    if (! live) hz = 440.0f * std::exp2((57.0f - 69.0f) / 12.0f);
    sf::FormantShape shape;
    shape.vowel = value(S::Vowel);
    shape.hum = value(S::Hum);
    shape.scale = (1.0f - value(S::Size)) * (boys ? sf::kBoysFormantScale : 1.0f);
    shape.brightDb = value(S::Bright);
    shape.f0 = hz;
    const auto set = sf::formants(boys ? sf::Soprano : type, shape);
    const Rect plot{r.x + 6, r.y + 16, r.w - 12, r.h - 22};
    const auto dbToY = [&](float db) { return plot.y + plot.h * std::clamp(-db / 60.0f, 0.0f, 1.0f); };
    // harmonics of the note: the source's level through the formants
    const float tension = std::clamp(value(S::Tension), 0.0f, 1.0f);
    const auto& harmonics = sf::GlottalTables::harmonics((int) std::lround((1.0f - tension) * 7.0f));
    float top = -200;
    for (int k = 1; (float) k * hz < 6000.0f && k <= 400; ++k) top = std::max(top, 20.0f * std::log10(harmonics[(size_t) k - 1]) + responseDb(set, hz * (float) k));
    for (int k = 1; (float) k * hz < 6000.0f && k <= 400; ++k)
    {
        const float f = hz * (float) k;
        const float db = 20.0f * std::log10(harmonics[(size_t) k - 1]) + responseDb(set, f) - top;
        const float x = hzToX(f, plot);
        c.drawLine(x, plot.y + plot.h, x, dbToY(db), pro::stroke(paint::withAlpha(kAccent, live ? 170 : 90), 1.2f));
    }
    // the formant curve
    float peak = -200;
    for (int i = 0; i <= 200; ++i) peak = std::max(peak, responseDb(set, 60.0f * std::pow(100.0f, (float) i / 200.0f)));
    SkPathBuilder p;
    for (int i = 0; i <= 200; ++i)
    {
        const float f = 60.0f * std::pow(100.0f, (float) i / 200.0f);
        const float x = hzToX(f, plot), y = dbToY(responseDb(set, f) - peak);
        if (i == 0) p.moveTo(x, y);
        else p.lineTo(x, y);
    }
    c.drawPath(p.detach(), pro::stroke(pro::kText, 1.6f));
    for (int i = 0; i < 3; ++i)
        pro::text(c, "F" + std::to_string(i + 1), {hzToX(set.hz[i], plot) - 12, plot.y - 14, 24, 12}, pro::kFaint, 9.0f, Align::Centre, true);
    static const char* const kTypeNames[5] = {"bass", "tenor", "alto", "soprano", "boy"};
    pro::text(c, std::string(kTypeNames[std::clamp(type, 0, 4)]) + (live ? format(" \xC2\xB7 %.0f Hz", hz) : std::string(" \xC2\xB7 A3")),
              {r.x + 8, r.y + 2, 144, 12}, pro::kFaint, 9.0f, Align::Left, true);
}

void SeraphFace::paintReal(SkCanvas& c, const Rect& outer)
{
    pro::well(c, outer, 6);
    static const char* const kNotes[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
    const auto voices = seraph_.realVoices();
    const float textW = outer.w - 84;
    // PLG-27h: the pack's row on top, the credit at the bottom when its voices sing
    pro::text(c, "PACK", {outer.x + 10, outer.y + 4, 34, 18}, pro::kFaint, 9.0f, Align::Left, true);
    if (pack_.folder.empty())
        pro::text(c, onGetPack ? "recorded singers, CC BY 4.0:" : "Seraph Voices pack: get it in Sonora's Browser > Packs",
                  {outer.x + 42, outer.y + 4, 200, 18}, pro::kDim, 9.0f, Align::Left);
    const bool credit = seraph_.packVoicesLoaded(packsRoot);
    const Rect r{outer.x, outer.y + 22, outer.w, outer.h - 22 - (credit ? 26.0f : 0.0f)};
    if (credit)
    {
        pro::text(c, "Credit required for music made with these voices:", {outer.x + 10, outer.y + outer.h - 28, textW, 12}, kAccent, 8.5f, Align::Left,
                  true);
        pro::text(c, packCredit(), {outer.x + 10, outer.y + outer.h - 15, textW, 12}, pro::kDim, 7.5f, Align::Left);
    }
    else if (! seraph_.packWanted().empty() && pack_.folder.empty())
        pro::text(c, "\"" + seraph_.packWanted() + "\" sings the Seraph Voices pack: get it above (the synthesis sings meanwhile)",
                  {outer.x + 10, outer.y + outer.h - 15, textW, 12}, kAccent, 8.5f, Align::Left, true);
    if (voices.empty())
    {
        pro::text(c, "REAL VOICES: drop a sung note here", {r.x + 10, r.y + 2, textW, 14}, kAccent, 10.5f, Align::Left, true);
        pro::text(c, "A dry, single held note on one vowel (ah, oh, oo), no reverb, two seconds or more.", {r.x + 10, r.y + 18, textW, 12},
                  pro::kDim, 9.0f, Align::Left);
        pro::text(c, "One file sings the whole range; notes a fifth apart, a man and a woman sound better.", {r.x + 10, r.y + 31, textW, 12},
                  pro::kDim, 9.0f, Align::Left);
        pro::text(c, "Until then the synthesised voices sing.", {r.x + 10, r.y + 44, textW, 12}, pro::kDim, 9.0f, Align::Left);
    }
    else
    {
        const int shown = std::min((int) voices.size(), std::max(1, (int) ((r.h - 16) / 16)));
        for (int i = 0; i < shown; ++i)
        {
            const auto& v = *voices[(size_t) i];
            const int note = (int) std::lround(v.midi);
            char line[160];
            std::snprintf(line, sizeof line, "%s%d  \xC2\xB7  %s  \xC2\xB7  %.1f s held", kNotes[((note % 12) + 12) % 12], note / 12 - 1,
                          v.vowel >= 0 ? kVowelSounds[v.vowel] : "vowel ?", v.sustainSeconds());
            pro::text(c, v.name, {r.x + 10, r.y + 2 + (float) i * 15, textW * 0.55f, 14}, pro::kText, 10.0f, Align::Left, true);
            pro::text(c, line, {r.x + 10 + textW * 0.55f, r.y + 2 + (float) i * 15, textW * 0.45f, 14}, pro::kDim, 9.0f, Align::Left);
        }
        if ((int) voices.size() > shown)
            pro::text(c, "+" + std::to_string(voices.size() - (size_t) shown) + " more", {r.x + 10, r.y + r.h - 16, 80, 14}, pro::kFaint, 9.5f, Align::Left);
        // how far the last note was from the recording that sang it
        const float distance = seraph_.realDistance();
        if (distance >= 0.0f)
        {
            const SkColor colour = distance <= 3.0f ? 0xFF7FD48A : distance <= 7.0f ? kAccent : 0xFFE5735E;
            char text[96];
            std::snprintf(text, sizeof text, "last note %.0f st from its recording%s", (double) distance, distance > 7.0f ? ": load one nearer" : "");
            pro::text(c, text, {r.x + 100, r.y + r.h - 16, textW - 90, 14}, colour, 9.5f, Align::Right, true);
        }
    }
    if (! realMessage.empty()) pro::text(c, realMessage, {r.x + 10, r.y + r.h - 14, textW, 13}, 0xFFE5735E, 9.0f, Align::Left);
}

void SeraphFace::paintPhrase(SkCanvas& c, const Rect& r)
{
    pro::well(c, r, 6);
    const auto& list = S::phrases();
    const auto& phrase = list[(size_t) std::clamp((int) std::lround(value(S::PhraseParam)), 0, (int) list.size() - 1)];
    const bool on = value(S::Chant) > 0.5f;
    pro::text(c, "\xE2\x97\x80", {r.x + 4, r.y, 18, r.h}, pro::kDim, 11, Align::Centre);
    pro::text(c, "\xE2\x96\xB6", {r.x + r.w - 22, r.y, 18, r.h}, pro::kDim, 11, Align::Centre);
    pro::text(c, phrase.name, {r.x + 26, r.y + 4, 110, 16}, on ? pro::kText : pro::kDim, 12, Align::Left, true);
    // the steps: x by length, y by scale step
    int lo = 0, hi = 0;
    for (int i = 0; i < phrase.count; ++i) { lo = std::min(lo, phrase.offset[i]); hi = std::max(hi, phrase.offset[i]); }
    hi = std::max(hi, lo + 2);
    const Rect area{r.x + 140, r.y + 6, r.w - 168, r.h - 12};
    const float total = (float) phrase.totalUnits();
    float x = area.x;
    const int playing = seraph_.chantStepNow();
    for (int i = 0; i < phrase.count; ++i)
    {
        const float w = area.w * (float) phrase.length[i] / total;
        const float y = area.y + area.h - (float) (phrase.offset[i] - lo + 1) / (float) (hi - lo + 1) * area.h;
        const bool lit = on && i == playing;
        pro::rounded(c, {x + 1, y, w - 2, 6}, 2, lit ? kAccent : paint::withAlpha(kAccent, on ? 120 : 60));
        x += w;
    }
    char units[48];
    std::snprintf(units, sizeof units, "%d notes \xC2\xB7 %d units", phrase.count, phrase.totalUnits());
    pro::text(c, units, {r.x + 26, r.y + 24, 110, 14}, pro::kFaint, 9.5f, Align::Left);
}

void SeraphFace::paintPattern(SkCanvas& c, const Rect& r, bool compact)
{
    pro::well(c, r, 5);
    const int pattern = (int) std::lround(value(S::GatePattern));
    const std::string name = pattern >= S::kCustomPattern ? std::string("Custom") : dsp::TranceGate::factory()[(size_t) std::clamp(pattern, 0, S::kCustomPattern - 1)].name;
    pro::text(c, "\xE2\x97\x80", {r.x + 2, r.y, 18, r.h}, pro::kDim, 10, Align::Centre);
    pro::text(c, "\xE2\x96\xB6", {r.x + r.w - 20, r.y, 18, r.h}, pro::kDim, 10, Align::Centre);
    pro::text(c, name, {r.x + 22, r.y, r.w - 44, r.h}, value(S::Gate) > 0.5f ? pro::kText : pro::kDim, compact ? 10.5f : 11.5f, Align::Centre, true);
}

void SeraphFace::paintSlider(SkCanvas& c, const Slider& s)
{
    const auto& i = info(s.param);
    const float t = std::clamp((value(s.param) - i.minValue) / std::max(1e-6f, i.maxValue - i.minValue), 0.0f, 1.0f);
    pro::text(c, s.label, {s.r.x - 56, s.r.y - 1, 52, s.r.h + 2}, pro::kFaint, 9.5f, Align::Right, true);
    pro::rounded(c, s.r, s.r.h * 0.5f, pro::kTrack);
    pro::rounded(c, {s.r.x, s.r.y, std::max(s.r.h, s.r.w * t), s.r.h}, s.r.h * 0.5f, paint::withAlpha(kAccent, 200));
    pro::text(c, valueText(s.param, value(s.param)), {s.r.x, s.r.y - 1, s.r.w - 6, s.r.h + 2}, pro::kText, 9.5f, Align::Right, true);
}

void SeraphFace::paintBack(SkCanvas& c, Layout layout)
{
    if (layout == Layout::Compact)
    {
        paintVowelStrip(c, vowel_, true);
        paintStalls(c, stalls_, true);
        paintPattern(c, pattern_, true);
        return;
    }
    pro::wordmark(c, "seraph", 24, 10, 28, kAccent, version);
    pro::text(c, headline(), {804, 12, 340, 20}, pro::kText, 13, Align::Right, true);
    pro::text(c, idleHelp(), {642, 46, 502, 16}, pro::kDim, 10.5f, Align::Right);

    const std::tuple<const char*, Rect> panels[] = {
        {"CHOIR", {16, 64, 372, 276}},   {"MOUTH", {396, 64, 456, 276}},  {"VOICE", {860, 64, 284, 276}},
        {"HUMAN", {16, 348, 552, 156}},  {"ENVELOPE", {576, 348, 344, 156}}, {"OUT", {928, 348, 216, 156}},
        {"CHANT", {16, 512, 372, 204}},  {"GATE", {396, 512, 456, 204}},  {"SPACE", {860, 512, 284, 204}}};
    for (const auto& [title, r] : panels)
    {
        pro::panel(c, r);
        pro::sectionTitle(c, title, r.x + 12, r.y + 6, kAccent);
    }
    pro::text(c, "SINGERS", {28, 274, 64, 24}, pro::kFaint, 9.5f, Align::Left, true);
    pro::text(c, "STEPS", {408, 252, 60, 22}, pro::kFaint, 9.5f, Align::Left, true);
    pro::text(c, "ONSET", {408, 280, 60, 22}, pro::kFaint, 9.5f, Align::Left, true);
    paintStalls(c, stalls_, false);
    paintVowelStrip(c, vowel_, false);
    if (std::lround(value(S::Source)) == S::RealSource) paintReal(c, spectrum_);
    else paintSpectrum(c, spectrum_);
    paintPhrase(c, phrase_);
    // the scale chooser
    pro::well(c, scale_, 5);
    pro::text(c, "\xE2\x97\x80", {scale_.x + 2, scale_.y, 18, scale_.h}, pro::kDim, 10, Align::Centre);
    pro::text(c, std::string(model::scaleAt((int) std::lround(value(S::Scale))).name), {scale_.x + 22, scale_.y, scale_.w - 30, scale_.h}, pro::kText,
              11.5f, Align::Left, true);
    paintPattern(c, pattern_, false);
    c.drawLine(16, 722, 1144, 722, pro::stroke(pro::kLineSoft, 1.0f));
}

void SeraphFace::paintFront(SkCanvas& c, Layout layout)
{
    for (const auto& s : sliders_) paintSlider(c, s);

    if (layout == Layout::Full) paintHelp(c, {20, 726, 1120, 18});
}
} // namespace sonora::ui

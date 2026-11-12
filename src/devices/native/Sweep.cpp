#include "devices/native/Sweep.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <array>
#include <cmath>

namespace sonora::devices
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kMaxSampleSeconds = 60.0;
constexpr float kDetuneOffsets[5] = {-1.0f, -0.45f, 0.0f, 0.5f, 0.95f};

std::vector<ParameterInfo> sweepParameters()
{
    using S = Sweep;
    std::vector<ParameterInfo> p(S::NumParams);
    p[S::Source] = makeParam("Source", "Source", 0, S::kSources - 1, 0, "", false,
                             std::vector<std::string>(S::kSourceNames, S::kSourceNames + S::kSources));
    p[S::Direction] = makeParam("Direction", "Direction", 0, 1, 0, "", false, {"Up", "Down"});
    p[S::LengthParam] = makeParam("Length", "Length", 0, S::kLengths - 1, 4, "", false,
                                  std::vector<std::string>(S::kLengthNames, S::kLengthNames + S::kLengths));
    p[S::Curve] = makeParam("Curve", "Curve", -1, 1, 0.35f);
    p[S::LowHz] = makeParam("LowHz", "Low", 20, 20000, 200, "Hz");
    p[S::HighHz] = makeParam("HighHz", "High", 20, 20000, 14000, "Hz");
    p[S::Resonance] = makeParam("Resonance", "Resonance", 0, 1, 0.35f, "%");
    p[S::Pitch] = makeParam("Pitch", "Pitch", 0, 48, 12, "st");
    p[S::Detune] = makeParam("Detune", "Detune", 0, 1, 0.35f, "%");
    p[S::Swell] = makeParam("Swell", "Swell", 0, 1, 1, "%");
    p[S::Width] = makeParam("Width", "Width", 0, 1, 0.8f, "%");
    p[S::Rhythm] = makeParam("Rhythm", "Rhythm", 0, 3, 0, "", false, {"Off", "1/4 > 1/16", "1/8 > 1/32", "1/16 > 1/64"});
    p[S::Reverse] = makeParam("Reverse", "Reverse", 0, 1, 1, "", true);
    p[S::Tail] = makeParam("Tail", "Tail", 0, 1, 0.6f, "%");
    p[S::Output] = makeParam("Output", "Output", -36, 12, -6, "dB");
    p[S::Chord] = makeParam("Chord", "Chord", 0, S::kChords - 1, 0, "", false,
                            std::vector<std::string>(S::kChordNames, S::kChordNames + S::kChords));
    p[S::Air] = makeParam("Air", "Air", 0, 1, 0, "%");
    return p;
}

// A small Schroeder/Moorer reverb (Freeverb's tunings), run offline over the sample to give it a tail to reverse.
juce::AudioBuffer<float> withTail(const juce::AudioBuffer<float>& dry, double sampleRate, float amount)
{
    const int tail = amount > 0.001f ? (int) ((1.0 + 4.0 * amount) * sampleRate) : 0;
    juce::AudioBuffer<float> out(2, dry.getNumSamples() + tail);
    out.clear();
    for (int ch = 0; ch < 2; ++ch) out.copyFrom(ch, 0, dry, std::min(ch, dry.getNumChannels() - 1), 0, dry.getNumSamples());
    if (tail == 0) return out;
    static constexpr int kCombs[8] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
    static constexpr int kAllpasses[4] = {556, 441, 341, 225};
    const double scale = sampleRate / 44100.0;
    const float feedback = 0.78f + 0.18f * amount, damp = 0.25f, wet = 0.9f * amount;
    for (int ch = 0; ch < 2; ++ch)
    {
        const int spread = ch == 0 ? 0 : 23;
        std::vector<std::vector<float>> combs, allpasses;
        std::vector<int> combPos(8, 0), allPos(4, 0);
        std::vector<float> lows(8, 0.0f);
        for (int c : kCombs) combs.emplace_back((size_t) std::max(1, (int) ((c + spread) * scale)), 0.0f);
        for (int a : kAllpasses) allpasses.emplace_back((size_t) std::max(1, (int) ((a + spread) * scale)), 0.0f);
        const float* in = out.getReadPointer(ch);
        std::vector<float> result((size_t) out.getNumSamples(), 0.0f);
        for (int s = 0; s < out.getNumSamples(); ++s)
        {
            const float x = in[s] * 0.015f;
            float sum = 0;
            for (size_t c = 0; c < combs.size(); ++c)
            {
                auto& line = combs[c];
                const float y = line[(size_t) combPos[c]];
                lows[c] = y * (1 - damp) + lows[c] * damp;
                line[(size_t) combPos[c]] = x + lows[c] * feedback;
                combPos[c] = (combPos[c] + 1) % (int) line.size();
                sum += y;
            }
            for (size_t a = 0; a < allpasses.size(); ++a)
            {
                auto& line = allpasses[a];
                const float buffered = line[(size_t) allPos[a]];
                line[(size_t) allPos[a]] = sum + buffered * 0.5f;
                sum = buffered - sum;
                allPos[a] = (allPos[a] + 1) % (int) line.size();
            }
            result[(size_t) s] = sum;
        }
        float* o = out.getWritePointer(ch);
        for (int s = 0; s < out.getNumSamples(); ++s) o[s] += wet * result[(size_t) s];
    }
    return out;
}

float polyBlep(double t, double dt)
{
    if (t < dt) { t /= dt; return (float) (t + t - t * t - 1.0); }
    if (t > 1.0 - dt) { t = (t - 1.0) / dt; return (float) (t * t + t + t + 1.0); }
    return 0.0f;
}
} // namespace

Sweep::Sweep() : NativeDevice(sweepParameters()) {}

Sweep::~Sweep() = default;

double Sweep::lengthSeconds() const
{
    const int index = std::clamp((int) std::lround(value(LengthParam)), 0, kLengths - 1);
    return kLengthBeats[index] * 60.0 / std::clamp(host_.tempo, 20.0, 999.0);
}

float Sweep::shaped(float p) const
{
    return std::pow(std::clamp(p, 0.0f, 1.0f), std::pow(4.0f, value(Curve)));
}

float Sweep::cutoffAt(float p) const
{
    const float s = shaped(p), low = value(LowHz), high = std::max(value(HighHz), low);
    const bool up = std::lround(value(Direction)) == Up;
    return up ? low * std::pow(high / low, s) : high * std::pow(low / high, s);
}

float Sweep::pitchAt(float p) const
{
    const float s = shaped(p), range = value(Pitch);
    const bool up = std::lround(value(Direction)) == Up;
    if (std::lround(value(Source)) == Sample && up) return -range * (1.0f - s); // rises to the sample's own pitch at the end
    return up ? range * s : -range * s;
}

float Sweep::levelAt(float p) const
{
    const float s = shaped(p), swell = value(Swell);
    return std::lround(value(Direction)) == Up ? 1.0f - swell * (1.0f - s) : 1.0f - swell * s;
}

float Sweep::gateRateAt(float p) const
{
    const int rhythm = (int) std::lround(value(Rhythm));
    if (rhythm <= 0) return 0.0f;
    const float start = (float) (1 << (rhythm - 1)); // gates per beat: 1, 2, 4
    const float s = shaped(p);
    return std::lround(value(Direction)) == Up ? start * std::pow(4.0f, s) : start * 4.0f * std::pow(0.25f, s);
}

void Sweep::prepare(double sampleRate, int)
{
    const bool changed = std::abs(sampleRate - sampleRate_) > 0.5;
    sampleRate_ = sampleRate > 0 ? sampleRate : 44100.0;
    reset();
    if (changed && sourceAudio_ != nullptr) prepareSample();
}

void Sweep::reset()
{
    active_ = false;
    elapsed_ = 0;
    release_ = 0;
    stopFade_ = 1.0f;
    stopping_ = false;
    for (auto& ch : svf_) ch[0] = ch[1] = 0;
    for (auto& ch : pink_) std::fill(std::begin(ch), std::end(ch), 0.0f);
    airState_[0] = airState_[1] = metalHp_[0] = metalHp_[1] = 0;
    progress_.store(-1);
}

// ---- sample

bool Sweep::loadSample(const std::string& path, std::string& error)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const juce::File file(juce::String::fromUTF8(path.c_str()));
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr || reader->sampleRate <= 0 || reader->lengthInSamples <= 0)
    {
        error = "cannot read " + file.getFileName().toStdString();
        return false;
    }
    const int frames = (int) std::min<int64_t>(reader->lengthInSamples, (int64_t) (kMaxSampleSeconds * reader->sampleRate));
    juce::AudioBuffer<float> audio((int) std::max(1u, std::min(2u, reader->numChannels)), frames);
    reader->read(&audio, 0, frames, 0, true, audio.getNumChannels() > 1);
    setSampleAudio(audio, reader->sampleRate, file.getFileNameWithoutExtension().toStdString());
    samplePath_ = path;
    return true;
}

void Sweep::setSampleAudio(const juce::AudioBuffer<float>& audio, double sampleRate, const std::string& name)
{
    auto copy = std::make_shared<juce::AudioBuffer<float>>(2, audio.getNumSamples());
    for (int ch = 0; ch < 2; ++ch) copy->copyFrom(ch, 0, audio, std::min(ch, audio.getNumChannels() - 1), 0, audio.getNumSamples());
    sourceAudio_ = std::move(copy);
    sourceRate_ = sampleRate > 0 ? sampleRate : 44100.0;
    sampleName_ = name;
    samplePath_.clear();
    prepareSample();
}

void Sweep::clearSample()
{
    sourceAudio_.reset();
    samplePath_.clear();
    sampleName_.clear();
    pending_.store(nullptr, std::memory_order_release);
    collectGarbage();
}

void Sweep::refreshSample()
{
    const float reverse = value(Reverse) > 0.5f && std::lround(value(Direction)) == Up ? 1.0f : 0.0f;
    if (sourceAudio_ != nullptr && (std::abs(value(Tail) - preparedTail_) > 1e-4f || std::abs(reverse - preparedReverse_) > 0.5f))
        prepareSample();
}

void Sweep::prepareSample()
{
    if (sourceAudio_ == nullptr) return;
    // Resample to the device rate (linear: the sweep repitches it anyway), add the tail, reverse for Up.
    const auto& src = *sourceAudio_;
    const double ratio = sourceRate_ / sampleRate_;
    const int frames = std::max(1, (int) ((double) src.getNumSamples() / ratio));
    juce::AudioBuffer<float> resampled(2, frames);
    for (int ch = 0; ch < 2; ++ch)
    {
        const float* in = src.getReadPointer(ch);
        float* out = resampled.getWritePointer(ch);
        for (int s = 0; s < frames; ++s)
        {
            const double pos = (double) s * ratio;
            const int i0 = std::min((int) pos, src.getNumSamples() - 1), i1 = std::min(i0 + 1, src.getNumSamples() - 1);
            out[s] = in[i0] + (in[i1] - in[i0]) * (float) (pos - (double) i0);
        }
    }
    auto prepared = std::make_unique<Prepared>();
    preparedTail_ = value(Tail);
    preparedReverse_ = value(Reverse) > 0.5f && std::lround(value(Direction)) == Up ? 1.0f : 0.0f;
    prepared->audio = withTail(resampled, sampleRate_, preparedTail_);
    prepared->reversed = preparedReverse_ > 0.5f;
    if (prepared->reversed) prepared->audio.reverse(0, prepared->audio.getNumSamples());
    pending_.store(prepared.get(), std::memory_order_release);
    owned_.push_back(std::move(prepared));
    collectGarbage();
}

void Sweep::collectGarbage()
{
    // Keep what is pending, what the audio thread says it uses, and the two newest (it may be between loading pending_ and
    // acknowledging it).
    const auto* pending = pending_.load(std::memory_order_acquire);
    const auto* used = inUse_.load(std::memory_order_acquire);
    for (size_t i = 0; i + 2 < owned_.size();)
    {
        if (owned_[i].get() != pending && owned_[i].get() != used) owned_.erase(owned_.begin() + (long) i);
        else ++i;
    }
}

// ---- types

const std::vector<Sweep::Type>& Sweep::types()
{
    using S = Sweep;
    static const std::vector<Type> list = {
        {"White Riser", "Classic white-noise riser: the filter opens, the level swells, the stereo widens.",
         {{S::Source, S::Noise}, {S::Direction, S::Up}, {S::Curve, 0.35f}, {S::LowHz, 200}, {S::HighHz, 14000}, {S::Resonance, 0.35f},
          {S::Pitch, 12}, {S::Detune, 0.35f}, {S::Swell, 1}, {S::Width, 0.8f}, {S::Rhythm, 0}, {S::Chord, 0}, {S::Air, 0}}},
        {"Pink Wash", "A soft, wide pink-noise wash that opens slowly and blooms at the end.",
         {{S::Source, S::Pink}, {S::Direction, S::Up}, {S::Curve, 0.6f}, {S::LowHz, 120}, {S::HighHz, 9000}, {S::Resonance, 0.15f},
          {S::Pitch, 12}, {S::Detune, 0.35f}, {S::Swell, 1}, {S::Width, 1}, {S::Rhythm, 0}, {S::Chord, 0}, {S::Air, 0}}},
        {"Supersaw Rise", "A detuned saw chord rising an octave, with air on top.",
         {{S::Source, S::Tone}, {S::Direction, S::Up}, {S::Curve, 0.35f}, {S::LowHz, 300}, {S::HighHz, 12000}, {S::Resonance, 0.3f},
          {S::Pitch, 12}, {S::Detune, 0.5f}, {S::Swell, 0.9f}, {S::Width, 0.9f}, {S::Rhythm, 0}, {S::Chord, 2}, {S::Air, 0.25f}}},
        {"Tension Gate", "Noise chopped by a gate that speeds up into the drop.",
         {{S::Source, S::Noise}, {S::Direction, S::Up}, {S::Curve, 0.2f}, {S::LowHz, 400}, {S::HighHz, 16000}, {S::Resonance, 0.45f},
          {S::Pitch, 12}, {S::Detune, 0.35f}, {S::Swell, 0.8f}, {S::Width, 0.6f}, {S::Rhythm, 2}, {S::Chord, 0}, {S::Air, 0}}},
        {"Whoosh Up", "Air rushing past: a resonant band sweeping up through noise.",
         {{S::Source, S::Whoosh}, {S::Direction, S::Up}, {S::Curve, 0}, {S::LowHz, 300}, {S::HighHz, 8000}, {S::Resonance, 0.7f},
          {S::Pitch, 12}, {S::Detune, 0.35f}, {S::Swell, 1}, {S::Width, 1}, {S::Rhythm, 0}, {S::Chord, 0}, {S::Air, 0}}},
        {"Reverse Crash", "A cymbal swelling in backwards, hitting full right on the drop. No sample needed.",
         {{S::Source, S::Metal}, {S::Direction, S::Up}, {S::Curve, 1}, {S::LowHz, 1500}, {S::HighHz, 18000}, {S::Resonance, 0.1f},
          {S::Pitch, 0}, {S::Detune, 0.35f}, {S::Swell, 1}, {S::Width, 1}, {S::Rhythm, 0}, {S::Chord, 0}, {S::Air, 0.6f}}},
        {"Sub Drop", "A sine falling two octaves after the drop (play a low note).",
         {{S::Source, S::Sine}, {S::Direction, S::Down}, {S::Curve, -0.3f}, {S::LowHz, 15000}, {S::HighHz, 20000}, {S::Resonance, 0},
          {S::Pitch, 24}, {S::Detune, 0}, {S::Swell, 0.2f}, {S::Width, 0}, {S::Rhythm, 0}, {S::Chord, 0}, {S::Air, 0}}},
        {"Downlifter", "White noise closing down and fading out after a hit.",
         {{S::Source, S::Noise}, {S::Direction, S::Down}, {S::Curve, -0.35f}, {S::LowHz, 200}, {S::HighHz, 14000}, {S::Resonance, 0.3f},
          {S::Pitch, 12}, {S::Detune, 0.35f}, {S::Swell, 1}, {S::Width, 0.8f}, {S::Rhythm, 0}, {S::Chord, 0}, {S::Air, 0}}},
        {"Laser Down", "A bright saw diving three octaves.",
         {{S::Source, S::Tone}, {S::Direction, S::Down}, {S::Curve, -0.6f}, {S::LowHz, 8000}, {S::HighHz, 18000}, {S::Resonance, 0.2f},
          {S::Pitch, 36}, {S::Detune, 0.1f}, {S::Swell, 0.6f}, {S::Width, 0.3f}, {S::Rhythm, 0}, {S::Chord, 0}, {S::Air, 0}}},
        {"Reverse Sample", "Any sample reversed with its reverb tail, rising in pitch to land on its own attack.",
         {{S::Source, S::Sample}, {S::Direction, S::Up}, {S::Curve, 0.35f}, {S::LowHz, 300}, {S::HighHz, 18000}, {S::Resonance, 0.2f},
          {S::Pitch, 12}, {S::Detune, 0.35f}, {S::Swell, 0.8f}, {S::Width, 0.8f}, {S::Rhythm, 0}, {S::Reverse, 1}, {S::Tail, 0.6f},
          {S::Chord, 0}, {S::Air, 0}}},
    };
    return list;
}

int Sweep::matchingType() const
{
    const auto& list = types();
    for (size_t t = 0; t < list.size(); ++t)
    {
        bool same = true;
        for (const auto& [param, v] : list[t].values)
        {
            const float tolerance = std::max(1.0e-3f, std::abs(v) * 1.0e-3f);
            if (std::abs(value(param) - v) > tolerance) { same = false; break; }
        }
        if (same) return (int) t;
    }
    return -1;
}

void Sweep::applyType(int index)
{
    const auto& list = types();
    if (index < 0 || (size_t) index >= list.size()) return;
    for (const auto& [param, v] : list[(size_t) index].values) setParameter(param, v);
    refreshSample();
}

void Sweep::preview() { previewRequests_.fetch_add(1, std::memory_order_relaxed); }

// ---- audio

void Sweep::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    const int n = audio.getNumSamples();
    if (n <= 0) return;
    if (const auto* p = pending_.load(std::memory_order_acquire); p != current_)
    {
        current_ = p;
        inUse_.store(p, std::memory_order_release);
    }
    float* left = audio.getWritePointer(0);
    float* right = audio.getNumChannels() > 1 ? audio.getWritePointer(1) : nullptr;
    const int source = std::clamp((int) std::lround(value(Source)), 0, kSources - 1);
    const bool up = std::lround(value(Direction)) == Up;
    const float detune = value(Detune), width = value(Width), resonance = value(Resonance), air = value(Air);
    const float output = juce::Decibels::decibelsToGain(value(Output));
    const double beatsPerSecond = std::clamp(host_.tempo, 20.0, 999.0) / 60.0;
    const bool whoosh = source == Whoosh;
    const float k = whoosh ? 1.0f / (1.2f + resonance * 9.0f) : 1.0f / (0.6f + resonance * 7.0f);
    const float releaseStep = (float) (1.0 / (0.006 * sampleRate_));
    static constexpr int kChordNotes[kChords][4] = {{0, -1, -1, -1}, {0, 12, -1, -1}, {0, 7, 12, -1}, {0, 4, 7, 12}, {0, 3, 7, 12}};
    const int chord = std::clamp((int) std::lround(value(Chord)), 0, kChords - 1);
    int chordCount = 0;
    while (chordCount < 4 && kChordNotes[chord][chordCount] >= 0) ++chordCount;
    static constexpr double kMetalHz[6] = {205.3, 304.4, 369.6, 522.7, 540.0, 800.0};

    auto start = [&](int note) {
        if (active_) release_ = 1.0f; // fade the old one out quickly under the new one's start
        active_ = true;
        stopFade_ = 1.0f;
        stopping_ = false;
        elapsed_ = 0;
        duration_ = std::max(16.0, lengthSeconds() * sampleRate_);
        note_ = note;
        for (auto& ph : phases_) ph = 0;
        for (auto& ph : metal_) ph = 0;
        gatePhase_ = 0;
        readPos_ = 0;
        if (source == Sample && current_ != nullptr && current_->reversed)
        {
            // Start where the reversed audio ends exactly at the sweep's end, with the pitch rising to 0 st.
            double consumed = 0;
            constexpr int kSteps = 256;
            for (int st = 0; st < kSteps; ++st) consumed += std::pow(2.0, (double) pitchAt(((float) st + 0.5f) / kSteps) / 12.0);
            consumed *= duration_ / kSteps;
            readPos_ = (double) current_->audio.getNumSamples() - consumed;
        }
    };
    if (const int requests = previewRequests_.load(std::memory_order_relaxed); requests != previewSeen_)
    {
        previewSeen_ = requests;
        if (active_ && ! stopping_) stopping_ = true; // a second click stops the audition
        else start(previewNote(source));
    }

    auto midiIt = midi.cbegin();
    float a[2][3]{};
    float level = 0, cutoff = 1000, pitch = 0, gateRate = 0, widthNow = 0;
    for (int i = 0; i < n; ++i)
    {
        for (; midiIt != midi.cend() && (*midiIt).samplePosition <= i; ++midiIt)
        {
            const auto m = (*midiIt).getMessage();
            if (m.isNoteOn()) start(m.getNoteNumber());
        }
        if (! active_ || isBypassed()) continue;

        if ((i & 15) == 0 || i == 0)
        {
            const float p = (float) (elapsed_ / duration_);
            level = levelAt(p);
            cutoff = std::min(cutoffAt(p), (float) sampleRate_ * 0.45f);
            pitch = pitchAt(p);
            gateRate = gateRateAt(p);
            const float sh = shaped(p);
            widthNow = up ? width * sh : width * (1.0f - sh);
            for (int ch = 0; ch < 2; ++ch)
            {
                float hz = cutoff;
                if (whoosh) hz = std::min((float) sampleRate_ * 0.45f, hz * (1.0f + 0.22f * (float) std::sin(lfo_ + (ch == 0 ? 0.0 : 1.9))));
                const float g = std::tan((float) kPi * hz / (float) sampleRate_);
                a[ch][0] = 1.0f / (1.0f + g * (g + k));
                a[ch][1] = g * a[ch][0];
                a[ch][2] = g * a[ch][1];
            }
        }
        lfo_ += 2.0 * kPi * 0.35 / sampleRate_;
        if (lfo_ > 2.0 * kPi) lfo_ -= 2.0 * kPi;

        auto white = [&](int ch) {
            noise_[ch] = ch == 0 ? noise_[0] * 1664525u + 1013904223u : noise_[1] * 22695477u + 1u;
            return (float) (noise_[ch] >> 8) / 8388608.0f - 1.0f;
        };
        auto spread = [&](float a0, float b0, float& l, float& r) {
            const float mid = 0.5f * (a0 + b0), side = 0.5f * (a0 - b0);
            l = mid + widthNow * side;
            r = mid - widthNow * side;
        };
        float l = 0, r = 0;
        switch (source)
        {
            case Noise:
            case Whoosh: spread(white(0), white(1), l, r); break;
            case Pink:
            {
                float out[2];
                for (int ch = 0; ch < 2; ++ch)
                {
                    const float w = white(ch);
                    auto& b = pink_[ch];
                    b[0] = 0.99886f * b[0] + w * 0.0555179f;
                    b[1] = 0.99332f * b[1] + w * 0.0750759f;
                    b[2] = 0.96900f * b[2] + w * 0.1538520f;
                    b[3] = 0.86650f * b[3] + w * 0.3104856f;
                    b[4] = 0.55000f * b[4] + w * 0.5329522f;
                    b[5] = -0.7616f * b[5] - w * 0.0168980f;
                    out[ch] = (b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + w * 0.5362f) * 0.16f;
                    b[6] = w * 0.115926f;
                }
                spread(out[0], out[1], l, r);
                break;
            }
            case Tone:
            {
                const float norm = 0.25f / std::sqrt((float) chordCount);
                for (int c = 0; c < chordCount; ++c)
                {
                    const double base = 440.0 * std::pow(2.0, ((double) note_ - 69.0 + (double) pitch + kChordNotes[chord][c]) / 12.0);
                    for (int v = 0; v < 5; ++v)
                    {
                        auto& ph = phases_[c * 5 + v];
                        const double hz = base * std::pow(2.0, (double) (kDetuneOffsets[v] * detune * 0.6f) / 12.0);
                        const double dt = std::min(0.45, hz / sampleRate_);
                        const float saw = (float) (2.0 * ph - 1.0) - polyBlep(ph, dt);
                        ph += dt;
                        if (ph >= 1.0) ph -= 1.0;
                        const float pan = (float) (v - 2) * 0.5f * widthNow;
                        l += saw * norm * (1.0f - std::max(0.0f, pan));
                        r += saw * norm * (1.0f + std::min(0.0f, pan));
                    }
                }
                break;
            }
            case Sine:
            {
                float sum = 0;
                for (int c = 0; c < chordCount; ++c)
                {
                    auto& ph = phases_[c];
                    const double hz = 440.0 * std::pow(2.0, ((double) note_ - 69.0 + (double) pitch + kChordNotes[chord][c]) / 12.0);
                    sum += (float) std::sin(2.0 * kPi * ph);
                    ph += hz / sampleRate_;
                    if (ph >= 1.0) ph -= 1.0;
                }
                l = r = sum * 0.7f / std::sqrt((float) chordCount);
                break;
            }
            case Metal:
            {
                const double scale = std::pow(2.0, ((double) note_ - 60.0 + (double) pitch) / 12.0) * 1.7;
                float side[2] = {0, 0};
                for (int o = 0; o < 6; ++o)
                {
                    metal_[o] += kMetalHz[o] * scale / sampleRate_;
                    if (metal_[o] >= 1.0) metal_[o] -= 1.0;
                    side[o & 1] += metal_[o] < 0.5 ? 0.16f : -0.16f;
                }
                const float nl = white(0), nr = white(1);
                // Bright: a first-difference high-pass on the squares and the noise.
                const float ml = side[0] + 0.35f * nl, mr = side[1] + 0.35f * nr;
                const float hl = ml - metalHp_[0], hr = mr - metalHp_[1];
                metalHp_[0] = ml;
                metalHp_[1] = mr;
                spread(hl * 0.8f, hr * 0.8f, l, r);
                break;
            }
            case Sample:
            default:
                if (current_ != nullptr)
                {
                    const auto& buffer = current_->audio;
                    if (readPos_ >= 0 && readPos_ < (double) buffer.getNumSamples() - 1)
                    {
                        const int i0 = (int) readPos_;
                        const float frac = (float) (readPos_ - (double) i0);
                        const float* bl = buffer.getReadPointer(0);
                        const float* br = buffer.getReadPointer(1);
                        const float sl = bl[i0] + (bl[i0 + 1] - bl[i0]) * frac, sr = br[i0] + (br[i0 + 1] - br[i0]) * frac;
                        const float mid = 0.5f * (sl + sr),
                                    side = 0.5f * (sl - sr) * (up ? 0.3f + 0.7f * widthNow / std::max(0.01f, width) : 1.0f);
                        l = mid + side;
                        r = mid - side;
                    }
                    readPos_ += std::pow(2.0, (double) pitch / 12.0);
                }
                break;
        }
        if (air > 0.0f) // a bright noise layer under the source, through the same filter
        {
            const float nl = white(0), nr = white(1);
            airState_[0] += 0.45f * (nl - airState_[0]);
            airState_[1] += 0.45f * (nr - airState_[1]);
            l += air * 0.5f * (nl - airState_[0]);
            r += air * 0.5f * (nr - airState_[1]);
        }

        // Filter: low-pass (Up opens it, Down closes it); Whoosh takes the resonant band.
        float* io[2] = {&l, &r};
        for (int ch = 0; ch < 2; ++ch)
        {
            auto& st = svf_[ch];
            const float v3 = *io[ch] - st[1], v1 = a[ch][0] * st[0] + a[ch][1] * v3, v2 = st[1] + a[ch][1] * st[0] + a[ch][2] * v3;
            st[0] = 2 * v1 - st[0];
            st[1] = 2 * v2 - st[1];
            *io[ch] = whoosh ? v1 * 1.6f + v2 * 0.2f : v2;
        }
        // Accelerating gate.
        float gate = 1.0f;
        if (gateRate > 0)
        {
            gatePhase_ += gateRate * beatsPerSecond / sampleRate_;
            const double ph = gatePhase_ - std::floor(gatePhase_);
            gate = (float) std::clamp(std::min(ph, 0.55 - ph) * 60.0, 0.0, 1.0);
        }
        float env = level * gate;
        if (release_ > 0) { env *= 1.0f - release_; release_ = std::max(0.0f, release_ - releaseStep); }
        if (stopping_)
        {
            stopFade_ = std::max(0.0f, stopFade_ - releaseStep * 0.5f);
            env *= stopFade_;
        }
        const double remaining = duration_ - elapsed_;
        const double endFade = 0.004 * sampleRate_;
        if (remaining < endFade) env *= (float) std::max(0.0, remaining / endFade);
        left[i] += l * env * output;
        if (right != nullptr) right[i] += r * env * output;

        elapsed_ += 1.0;
        if (elapsed_ >= duration_ || (stopping_ && stopFade_ <= 0.0f)) { active_ = false; stopping_ = false; }
    }
    progress_.store(active_ ? (float) (elapsed_ / duration_) : -1.0f, std::memory_order_relaxed);
}

juce::AudioBuffer<float> Sweep::renderOffline(const std::vector<float>& parameterValues, const juce::AudioBuffer<float>* source,
                                              double sampleRate, double tempo, int note, double sourceRate)
{
    auto sweep = std::make_unique<Sweep>();
    for (int i = 0; i < (int) parameterValues.size() && i < NumParams; ++i) sweep->setParameter(i, parameterValues[(size_t) i]);
    sweep->prepare(sampleRate, 512);
    HostInfo host;
    host.tempo = tempo;
    host.sampleRate = sampleRate;
    sweep->setHostInfo(host);
    if (source != nullptr)
    {
        sweep->setParameter(Source, (float) Sample);
        sweep->setSampleAudio(*source, sourceRate > 0 ? sourceRate : sampleRate, "clip");
    }
    const int total = (int) std::ceil(sweep->lengthSeconds() * sampleRate);
    juce::AudioBuffer<float> out(2, total);
    out.clear();
    for (int start = 0; start < total; start += 512)
    {
        const int len = std::min(512, total - start);
        juce::AudioBuffer<float> view(out.getArrayOfWritePointers(), 2, start, len);
        juce::MidiBuffer midi;
        if (start == 0) midi.addEvent(juce::MidiMessage::noteOn(1, note, 1.0f), 0);
        sweep->process(view, midi);
    }
    return out;
}

std::vector<DeviceProcessor::UiGroup> Sweep::uiGroups() const
{
    return {{"Sweep", {Source, Direction, LengthParam, Curve}, 2}, {"Filter", {LowHz, HighHz, Resonance, Air}, 2},
            {"Motion", {Pitch, Chord, Swell, Width, Rhythm}, 3}, {"Sound", {Detune, Reverse, Tail, Output}, 2}};
}
} // namespace sonora::devices

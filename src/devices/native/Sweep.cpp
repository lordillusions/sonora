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
    p[S::Source] = makeParam("Source", "Source", 0, 2, 0, "", false, {"Noise", "Tone", "Sample"});
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
    for (auto& ch : svf_) ch[0] = ch[1] = 0;
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
    const int source = (int) std::lround(value(Source));
    const bool up = std::lround(value(Direction)) == Up;
    const float detune = value(Detune), width = value(Width), resonance = value(Resonance);
    const float output = juce::Decibels::decibelsToGain(value(Output));
    const double beatsPerSecond = std::clamp(host_.tempo, 20.0, 999.0) / 60.0;
    const float k = 1.0f / (0.6f + resonance * 7.0f);
    const float releaseStep = (float) (1.0 / (0.006 * sampleRate_));

    auto midiIt = midi.cbegin();
    float g = 0, a1 = 0, a2 = 0, a3 = 0;
    float level = 0, cutoff = 1000, pitch = 0, gateRate = 0, widthNow = 0;
    for (int i = 0; i < n; ++i)
    {
        for (; midiIt != midi.cend() && (*midiIt).samplePosition <= i; ++midiIt)
        {
            const auto m = (*midiIt).getMessage();
            if (! m.isNoteOn()) continue;
            if (active_) release_ = 1.0f; // fade the old one out quickly under the new one's start
            active_ = true;
            elapsed_ = 0;
            duration_ = std::max(16.0, lengthSeconds() * sampleRate_);
            note_ = m.getNoteNumber();
            for (auto& ph : phases_) ph = 0;
            gatePhase_ = 0;
            readPos_ = 0;
            if (source == Sample && current_ != nullptr && current_->reversed)
            {
                // Start where the reversed audio ends exactly at the sweep's end, with the pitch rising to 0 st.
                double consumed = 0;
                constexpr int kSteps = 256;
                for (int s = 0; s < kSteps; ++s)
                    consumed += std::pow(2.0, (double) pitchAt(((float) s + 0.5f) / kSteps) / 12.0);
                consumed *= duration_ / kSteps;
                readPos_ = (double) current_->audio.getNumSamples() - consumed;
            }
        }
        if (! active_ || isBypassed()) continue;

        if ((i & 15) == 0 || i == 0)
        {
            const float p = (float) (elapsed_ / duration_);
            level = levelAt(p);
            cutoff = std::min(cutoffAt(p), (float) sampleRate_ * 0.45f);
            pitch = pitchAt(p);
            gateRate = gateRateAt(p);
            const float s = shaped(p);
            widthNow = up ? width * s : width * (1.0f - s);
            g = std::tan((float) kPi * cutoff / (float) sampleRate_);
            a1 = 1.0f / (1.0f + g * (g + k));
            a2 = g * a1;
            a3 = g * a2;
        }

        float l = 0, r = 0;
        if (source == Noise)
        {
            noise_[0] = noise_[0] * 1664525u + 1013904223u;
            noise_[1] = noise_[1] * 22695477u + 1u;
            const float nl = (float) (noise_[0] >> 8) / 8388608.0f - 1.0f, nr = (float) (noise_[1] >> 8) / 8388608.0f - 1.0f;
            const float mid = 0.5f * (nl + nr), side = 0.5f * (nl - nr);
            l = mid + widthNow * side;
            r = mid - widthNow * side;
        }
        else if (source == Tone)
        {
            const double base = 440.0 * std::pow(2.0, ((double) note_ - 69.0 + (double) pitch) / 12.0);
            for (int v = 0; v < 5; ++v)
            {
                const double hz = base * std::pow(2.0, (double) (kDetuneOffsets[v] * detune * 0.6f) / 12.0);
                const double dt = std::min(0.45, hz / sampleRate_);
                const float saw = (float) (2.0 * phases_[v] - 1.0) - polyBlep(phases_[v], dt);
                phases_[v] += dt;
                if (phases_[v] >= 1.0) phases_[v] -= 1.0;
                const float pan = (float) (v - 2) * 0.5f * widthNow;
                l += saw * 0.25f * (1.0f - std::max(0.0f, pan));
                r += saw * 0.25f * (1.0f + std::min(0.0f, pan));
            }
        }
        else if (current_ != nullptr)
        {
            const auto& buffer = current_->audio;
            if (readPos_ >= 0 && readPos_ < (double) buffer.getNumSamples() - 1)
            {
                const int i0 = (int) readPos_;
                const float frac = (float) (readPos_ - (double) i0);
                const float* bl = buffer.getReadPointer(0);
                const float* br = buffer.getReadPointer(1);
                const float sl = bl[i0] + (bl[i0 + 1] - bl[i0]) * frac, sr = br[i0] + (br[i0 + 1] - br[i0]) * frac;
                const float mid = 0.5f * (sl + sr), side = 0.5f * (sl - sr) * (up ? 0.3f + 0.7f * widthNow / std::max(0.01f, width) : 1.0f);
                l = mid + side;
                r = mid - side;
            }
            readPos_ += std::pow(2.0, (double) pitch / 12.0);
        }

        // Filter (low-pass: Up opens it, Down closes it).
        float* io[2] = {&l, &r};
        for (int ch = 0; ch < 2; ++ch)
        {
            auto& st = svf_[ch];
            const float v3 = *io[ch] - st[1], v1 = a1 * st[0] + a2 * v3, v2 = st[1] + a2 * st[0] + a3 * v3;
            st[0] = 2 * v1 - st[0];
            st[1] = 2 * v2 - st[1];
            *io[ch] = v2;
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
        const double remaining = duration_ - elapsed_;
        const double endFade = 0.004 * sampleRate_;
        if (remaining < endFade) env *= (float) std::max(0.0, remaining / endFade);
        left[i] += l * env * output;
        if (right != nullptr) right[i] += r * env * output;

        elapsed_ += 1.0;
        if (elapsed_ >= duration_) active_ = false;
    }
    progress_.store(active_ ? (float) (elapsed_ / duration_) : -1.0f, std::memory_order_relaxed);
}

juce::AudioBuffer<float> Sweep::renderOffline(const std::vector<float>& parameterValues, const juce::AudioBuffer<float>* source,
                                              double sampleRate, double tempo, int note)
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
        sweep->setSampleAudio(*source, sampleRate, "clip");
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
    return {{"Sweep", {Source, Direction, LengthParam, Curve}, 2}, {"Filter", {LowHz, HighHz, Resonance}, 2},
            {"Motion", {Pitch, Swell, Width, Rhythm}, 2}, {"Sound", {Detune, Reverse, Tail, Output}, 2}};
}
} // namespace sonora::devices

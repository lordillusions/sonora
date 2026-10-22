#include "devices/native/CurveLfo.h"
#include <algorithm>
#include <cmath>

namespace sonora::devices
{
namespace
{
constexpr double kBases[] = {32, 16, 8, 4, 2, 1, 0.5, 0.25, 0.125}; // beats: 8 bars ... 1/32
constexpr const char* kBaseNames[] = {"8 bars", "4 bars", "2 bars", "1 bar", "1/2", "1/4", "1/8", "1/16", "1/32"};
constexpr int kControlInterval = 16, kMaxCcPerBlock = 32;
constexpr float kWarps[] = {0.125f, 0.25f, 0.5f, 1, 2, 4, 8};

std::vector<ParameterInfo> infos()
{
    using L = CurveLfo;
    std::vector<std::string> rates, sources, warps{"1/8x", "1/4x", "1/2x", "1x", "2x", "4x", "8x"};
    for (const char* base : kBaseNames)
        for (const char* suffix : {"", " dot", " trip"}) rates.push_back(std::string(base) + suffix);
    for (int g = 1; g <= L::kGraphs; ++g) sources.push_back(std::to_string(g));
    std::vector<ParameterInfo> p;
    p.push_back(makeParam("Sync", "Sync", 0, 1, 1, "", true));
    p.push_back(makeParam("Rate", "Rate", 0, L::kRates - 1, 15, "", false, rates));
    p.push_back(makeParam("Hertz", "Free Rate", 0.01f, 40, 2, "Hz"));
    p.push_back(makeParam("Anchor", "Anchor", 0, 1, 1, "", true));
    p.push_back(makeParam("Dotted", "Dotted", 0, 1, 0, "", true));
    p.push_back(makeParam("Triplet", "Triplet", 0, 1, 0, "", true));
    p.push_back(makeParam("Swing", "Swing", -0.5f, 0.5f, 0, "%"));
    p.push_back(makeParam("Phase", "Phase", 0, 1, 0, "%"));
    p.push_back(makeParam("Pwm", "PWM", -1, 1, 0, "%"));
    p.push_back(makeParam("Smooth", "Smooth", 0, 200, 3, "ms"));
    p.push_back(makeParam("Offset", "Offset", -4096, 4096, 0, "smp"));
    p.push_back(makeParam("Depth", "Depth", 0, 1, 1, "%"));
    for (int d = 0; d < L::kDestinations; ++d)
    {
        const std::string name = L::destinationName(d);
        p.push_back(makeParam(name + ".Source", name + " Graph", 0, L::kGraphs - 1, 0, "", false, sources));
        const bool volume = d == L::Vol;
        p.push_back(makeParam(name + ".Amount", name + " Amount", volume ? 0.0f : -1.0f, 1, volume ? 1.0f : 0.0f, "%"));
    }
    p.push_back(makeParam("Filter.On", "Filter", 0, 1, 0, "", true));
    p.push_back(makeParam("Filter.Type", "Filter Type", 0, CurveFilter::TypeCount - 1, CurveFilter::Low12, "", false,
                          CurveFilter::typeNames()));
    p.push_back(makeParam("Filter.Cutoff", "Cutoff", 0, 1, 0.6f, "%"));
    p.push_back(makeParam("Filter.Resonance", "Resonance", 0, 1, 0.2f, "%"));
    p.push_back(makeParam("Filter.Mix", "Filter Mix", 0, 1, 1, "%"));
    p.push_back(makeParam("Filter.Var", "Var", 0, 1, 0.5f, "%"));
    p.push_back(makeParam("Midi.Retrig", "Note Retrig", 0, 2, 0, "", false, {"Off", "Retrig", "Env"}));
    p.push_back(makeParam("Midi.Gate", "Note Gate", 0, 1, 0, "", true));
    p.push_back(makeParam("Midi.VelPwm", "Vel > PWM", 0, 1, 0, "", true));
    p.push_back(makeParam("Midi.NoteRate", "Note > Rate", 0, 1, 0, "", true));
    p.push_back(makeParam("Midi.NoteCutoff", "Note > Cutoff", 0, 1, 0, "", true));
    p.push_back(makeParam("Midi.CcOut", "CC Out", 0, 128, 0));
    p.push_back(makeParam("Midi.CcDepth", "CC Depth", 0, 127, 127));
    p.push_back(makeParam("SelectedGraph", "Graph", 0, L::kGraphs - 1, 0));
    const auto pump = dsp::CurveGraph::preset(0);
    for (int g = 0; g < L::kGraphs; ++g)
    {
        const std::string prefix = "G" + std::to_string(g + 1) + ".";
        p.push_back(makeParam(prefix + "Count", "Points", 1, L::kMaxPoints, (float) pump.count));
        p.push_back(makeParam(prefix + "Warp", "Warp", 0, 6, 3, "", false, warps));
        for (int i = 0; i < L::kMaxPoints; ++i)
        {
            const std::string point = prefix + std::to_string(i);
            const bool used = i < pump.count;
            p.push_back(makeParam(point + "X", "X", 0, 1, used ? pump.x[i] : 1));
            p.push_back(makeParam(point + "Y", "Y", 0, 1, used ? pump.y[i] : 1));
            p.push_back(makeParam(point + "C", "C", -1, 1, used ? pump.curve[i] : 0));
        }
    }
    return p;
}

double swingPhase(double cycles, double swing)
{
    const double split = std::clamp(0.5 + swing, 0.05, 0.95);
    double pair = cycles * 0.5;
    pair -= std::floor(pair);
    return pair < split ? pair / split : (pair - split) / (1 - split);
}
} // namespace

double CurveLfo::rateBeats(int index, bool dotted, bool triplet)
{
    index = std::clamp(index, 0, kRates - 1);
    const int variant = index % 3;
    const double base = kBases[index / 3];
    if (variant == 1 && dotted) return base * 1.5;
    if (variant == 2 && triplet) return base * 2.0 / 3.0;
    return base;
}
float CurveLfo::displayPosition(int index) const
{
    const float shaped = displayShaped_.load(std::memory_order_relaxed);
    if (shaped >= 1.0f) return 1.0f;
    const float position = shaped * warpValue((int) std::lround(value(warpParam(std::clamp(index, 0, kGraphs - 1)))));
    return position - std::floor(position);
}
float CurveLfo::warpValue(int index) { return kWarps[std::clamp(index, 0, 6)]; }
const char* CurveLfo::destinationName(int destination)
{
    static constexpr const char* names[kDestinations] = {"Cut", "Res", "Vol", "Pan", "Var"};
    return names[std::clamp(destination, 0, kDestinations - 1)];
}

CurveLfo::CurveLfo() : NativeDevice(infos()) { for (auto& o : displayOutput_) o.store(0); reset(); }
void CurveLfo::prepare(double sampleRate, int)
{
    sampleRate_ = std::max(8000.0, sampleRate);
    filter_.prepare(sampleRate_);
    followRelease_ = (float) std::exp(-1.0 / (0.015 * sampleRate_)); // scope peak follower: 15 ms release
    reset();
}
void CurveLfo::reset()
{
    freePhase_ = stoppedBeats_ = notePhase_ = 0;
    noteStarted_ = envDone_ = false; gateOpen_ = true;
    heldNotes_ = controlCounter_ = 0; lastCc_ = -1;
    for (auto& s : sourceOverride_) s = -1;
    for (auto& s : smoothed_) s = 0.5f;
    smoothed_[Vol] = 1; noteNumber_ = noteVelocity_ = -1; gateLevel_ = 1;
    filter_.reset();
    for (auto& b : scopeIn_) b.store(0, std::memory_order_relaxed);
    for (auto& b : scopeOut_) b.store(0, std::memory_order_relaxed);
    scopeBin_ = -1; binIn_ = binOut_ = followIn_ = followOut_ = 0;
    displayRevision_.fetch_add(1, std::memory_order_relaxed);
}
void CurveLfo::storeScopeBin(int bin)
{
    scopeIn_[(size_t) bin].store(binIn_, std::memory_order_relaxed);
    scopeOut_[(size_t) bin].store(binOut_, std::memory_order_relaxed);
}

std::vector<int> CurveLfo::displayParameters() const
{
    // Knobs page by twelve: timing + volume/pan routing, then the filter and its routing, then MIDI.
    std::vector<int> out{Rate, Hertz, Swing, Phase, Pwm, Smooth, Offset, Depth,
                         sourceParam(Vol), amountParam(Vol), sourceParam(Pan), amountParam(Pan),
                         FilterType, Cutoff, Resonance, FilterMix, FilterVar,
                         sourceParam(Cut), amountParam(Cut), sourceParam(Res), amountParam(Res), sourceParam(Var), amountParam(Var),
                         NoteRetrig, CcOut, CcDepth};
    for (int toggle : {Sync, Anchor, Dotted, Triplet, FilterOn, NoteGate, VelPwm, NoteRate, NoteCutoff}) out.push_back(toggle);
    return out;
}

dsp::CurveGraph CurveLfo::graph(int index) const
{
    index = std::clamp(index, 0, kGraphs - 1);
    dsp::CurveGraph g;
    g.count = std::clamp((int) std::lround(value(countParam(index))), 1, kMaxPoints);
    for (int i = 0; i < kMaxPoints; ++i)
    {
        g.x[i] = value(pointParam(index, i, X)); g.y[i] = value(pointParam(index, i, Y));
        g.curve[i] = value(pointParam(index, i, Curve));
    }
    return g;
}
void CurveLfo::setGraph(int index, const dsp::CurveGraph& g)
{
    index = std::clamp(index, 0, kGraphs - 1);
    const int count = std::clamp(g.count, 1, kMaxPoints);
    for (int i = 0; i < kMaxPoints; ++i)
    {
        const bool used = i < count;
        setParameter(pointParam(index, i, X), used ? g.x[i] : 1);
        setParameter(pointParam(index, i, Y), used ? g.y[i] : 1);
        setParameter(pointParam(index, i, Curve), used ? g.curve[i] : 0);
    }
    setParameter(countParam(index), (float) count); // published last so the audio thread never sees stale tails
}

void CurveLfo::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi)
{
    if (isBypassed()) return;
    const int numSamples = audio.getNumSamples(), channels = std::min(2, audio.getNumChannels());
    if (numSamples <= 0) return;
    const double tempo = std::isfinite(host_.tempo) && host_.tempo > 0 ? std::clamp(host_.tempo, 20.0, 999.0) : 120.0;
    const bool sync = value(Sync) >= 0.5f, anchor = value(Anchor) >= 0.5f;
    const bool dotted = value(Dotted) >= 0.5f, triplet = value(Triplet) >= 0.5f;
    const double swing = value(Swing), phaseOffset = value(Phase), offsetSamples = value(Offset);
    const float depth = value(Depth);
    const int retrig = (int) std::lround(value(NoteRetrig));
    const bool gateMode = value(NoteGate) >= 0.5f, velPwm = value(VelPwm) >= 0.5f;
    const bool noteRate = value(NoteRate) >= 0.5f, noteCutoff = value(NoteCutoff) >= 0.5f;
    const bool selectByNote = ! (velPwm || noteRate || noteCutoff);
    const bool filterOn = value(FilterOn) >= 0.5f;
    const int filterType = (int) std::lround(value(FilterType));
    const float baseRes = value(Resonance), mixAmount = value(FilterMix), baseVar = value(FilterVar);
    const int ccOut = (int) std::lround(value(CcOut));
    const float ccDepth = value(CcDepth);
    const float smoothMs = value(Smooth);
    const float smoothCoef = smoothMs <= 0 ? 1.0f : (float) (1 - std::exp(-1000.0 / (sampleRate_ * smoothMs)));
    const float gateCoef = (float) (1 - std::exp(-1000.0 / (sampleRate_ * std::max(2.0f, smoothMs))));
    float amount[kDestinations];
    for (int d = 0; d < kDestinations; ++d) amount[d] = std::clamp(value(amountParam(d)), -1.0f, 1.0f);
    std::array<dsp::CurveGraph, kGraphs> graphs;
    std::array<float, kGraphs> warps{};
    std::array<bool, kGraphs> loaded{};
    auto ensureGraph = [&](int g) {
        if (loaded[(size_t) g]) return;
        graphs[(size_t) g] = graph(g);
        warps[(size_t) g] = warpValue((int) std::lround(value(warpParam(g))));
        loaded[(size_t) g] = true;
    };

    double rateTime = 1, hertz = value(Hertz), stepCycles = 0, pwm = value(Pwm);
    float cutoffBase = value(Cutoff);
    const double beatsPerSample = tempo / (60.0 * sampleRate_);
    auto refresh = [&] {
        int rate = (int) std::lround(value(Rate));
        hertz = value(Hertz);
        if (noteRate && noteNumber_ >= 0)
        {
            rate = (int) std::lround(noteNumber_ / 127.0 * (kRates - 1));
            hertz = 0.05 * std::pow(800.0, noteNumber_ / 127.0);
        }
        rateTime = rateBeats(rate, dotted, triplet);
        stepCycles = sync ? beatsPerSample / rateTime : hertz / sampleRate_;
        pwm = velPwm && noteVelocity_ >= 0 ? noteVelocity_ / 127.0 * 2 - 1 : value(Pwm);
        cutoffBase = noteCutoff && noteNumber_ >= 0 ? noteNumber_ / 127.0f : value(Cutoff);
    };
    refresh();

    const int scopeGraph = std::clamp((int) std::lround(value(SelectedGraph)), 0, kGraphs - 1);
    const double scopeWarp = warpValue((int) std::lround(value(warpParam(scopeGraph))));
    bool ranThisBlock = false;

    struct CcEvent { int sample, value; };
    std::array<CcEvent, kMaxCcPerBlock> ccEvents;
    int ccCount = 0;
    float lastPhase = 0, lastShaped = 0;
    auto events = midi.begin();
    const auto end = midi.end();
    float* data[2] = {audio.getWritePointer(0), channels > 1 ? audio.getWritePointer(1) : nullptr};

    for (int s = 0; s < numSamples; ++s)
    {
        bool changed = false;
        while (events != end && (*events).samplePosition <= s)
        {
            const auto meta = *events;
            ++events;
            if (meta.numBytes < 3) continue;
            const int status = meta.data[0] & 0xF0, note = meta.data[1] & 0x7F, velocity = meta.data[2] & 0x7F;
            const bool on = status == 0x90 && velocity > 0, off = status == 0x80 || (status == 0x90 && velocity == 0);
            if (! on && ! off) continue;
            if (note < 60)
            {
                if (on && selectByNote) sourceOverride_[std::min(note / 12, kDestinations - 1)] = note % 12;
                continue;
            }
            if (on)
            {
                ++heldNotes_; gateOpen_ = true; noteNumber_ = (float) note; noteVelocity_ = (float) velocity;
                if (retrig != 0) { noteStarted_ = true; notePhase_ = 0; envDone_ = false; }
                changed = true;
            }
            else
            {
                heldNotes_ = std::max(0, heldNotes_ - 1);
                if (heldNotes_ == 0) gateOpen_ = false;
            }
        }
        if (changed) refresh();
        // The LFO only moves while the song plays, a note is held or a note envelope is still running: a stopped
        // transport freezes the playhead and the modulation where they are.
        const bool running = host_.playing || heldNotes_ > 0 || (retrig == 2 && noteStarted_ && ! envDone_);
        ranThisBlock = ranThisBlock || running;
        if (! running && std::isfinite(host_.beats))
            stoppedBeats_ = host_.beats; // parked where the song will start; held notes continue from there

        bool held = false;
        double phase = 0;
        if (retrig != 0 && noteStarted_) { held = envDone_; phase = envDone_ ? 1.0 : notePhase_; }
        else if (retrig == 2) { held = true; phase = 1.0; }
        else if (sync && anchor)
        {
            const double beatPos = host_.playing && std::isfinite(host_.beats) ? host_.beats + s * beatsPerSample : stoppedBeats_;
            const double cycles = beatPos / rateTime + offsetSamples * stepCycles;
            phase = std::abs(swing) > 1e-9 ? swingPhase(cycles, swing) : cycles - std::floor(cycles);
        }
        else
        {
            phase = freePhase_ + offsetSamples * stepCycles;
            phase -= std::floor(phase);
        }
        lastPhase = (float) phase;

        // Shape the cycle position: phase offset, PWM squeeze, then each graph's own warp.
        double shaped = held ? 1.0 : phase + phaseOffset;
        if (! held)
        {
            shaped -= std::floor(shaped);
            if (std::abs(pwm) > 1e-9)
            {
                const double mid = 0.5 + 0.45 * std::clamp(pwm, -1.0, 1.0);
                shaped = shaped < mid ? 0.5 * shaped / mid : 0.5 + 0.5 * (shaped - mid) / (1 - mid);
            }
        }
        lastShaped = (float) shaped;
        const float gateTarget = ! gateMode || gateOpen_ ? 1.0f : 0.0f;
        gateLevel_ += (gateTarget - gateLevel_) * gateCoef;
        for (int d = 0; d < kDestinations; ++d)
        {
            const int source = sourceOverride_[d] >= 0 ? sourceOverride_[d] : std::clamp((int) std::lround(value(sourceParam(d))), 0, kGraphs - 1);
            ensureGraph(source);
            double position = held ? 1.0 : shaped * warps[(size_t) source];
            if (! held) position -= std::floor(position);
            const float raw = graphs[(size_t) source].eval((float) position);
            smoothed_[d] += (raw - smoothed_[d]) * smoothCoef;
        }

        if (controlCounter_++ % kControlInterval == 0)
        {
            const float scale = depth * gateLevel_;
            cutFinal_ = std::clamp(cutoffBase + amount[Cut] * scale * (smoothed_[Cut] - 0.5f), 0.0f, 1.0f);
            resFinal_ = std::clamp(baseRes + amount[Res] * scale * (smoothed_[Res] - 0.5f), 0.0f, 1.0f);
            varFinal_ = std::clamp(baseVar + amount[Var] * scale * (smoothed_[Var] - 0.5f), 0.0f, 1.0f);
            if (filterOn) filter_.setup(filterType, CurveFilter::positionToHz(cutFinal_), resFinal_, varFinal_);
            if (ccOut > 0)
            {
                const int cc = (int) std::lround(std::clamp(cutFinal_, 0.0f, 1.0f) * ccDepth);
                if (cc != lastCc_ && ccCount < kMaxCcPerBlock) { ccEvents[(size_t) ccCount++] = {s, cc}; lastCc_ = cc; }
            }
            else lastCc_ = -1;
        }

        const float scale = depth * gateLevel_;
        const float gain = std::clamp(1.0f - amount[Vol] * scale * (1.0f - smoothed_[Vol]), 0.0f, 1.0f);
        const float position = std::clamp((smoothed_[Pan] - 0.5f) * 2 * amount[Pan] * scale, -1.0f, 1.0f);
        const float channelGain[2] = {gain * std::min(1.0f, 1 - position), gain * std::min(1.0f, 1 + position)};
        float peakIn = 0, peakOut = 0;
        for (int ch = 0; ch < channels; ++ch)
        {
            float x = data[ch][s];
            peakIn = std::max(peakIn, std::abs(x));
            if (filterOn) { const float wet = filter_.process(ch, x); x += mixAmount * (wet - x); }
            data[ch][s] = x * channelGain[ch];
            peakOut = std::max(peakOut, std::abs(data[ch][s]));
        }
        // Peak followers smooth the scope so it shows the level, not single waveform cycles.
        followIn_ = std::max(std::isfinite(peakIn) ? peakIn : 0.0f, followIn_ * followRelease_);
        followOut_ = std::max(std::isfinite(peakOut) ? peakOut : 0.0f, followOut_ * followRelease_);
        if (running && ! held)
        {
            double cyclePosition = shaped * scopeWarp;
            cyclePosition -= std::floor(cyclePosition);
            const int bin = std::clamp((int) (cyclePosition * kScopeBins), 0, kScopeBins - 1);
            if (bin != scopeBin_)
            {
                if (scopeBin_ >= 0)
                {
                    storeScopeBin(scopeBin_);
                    // Fast cycles skip slices: fill the ones passed over, but not across a jump (retrigger, relocation).
                    const int gap = (bin - scopeBin_ + kScopeBins) % kScopeBins;
                    if (gap > 1 && gap < kScopeBins / 8)
                        for (int k = 1; k < gap; ++k) storeScopeBin((scopeBin_ + k) % kScopeBins);
                }
                scopeBin_ = bin; binIn_ = binOut_ = 0;
            }
            binIn_ = std::max(binIn_, followIn_);
            binOut_ = std::max(binOut_, followOut_);
        }

        if (running)
            stoppedBeats_ = host_.playing && std::isfinite(host_.beats) ? host_.beats + (s + 1) * beatsPerSample : stoppedBeats_ + beatsPerSample;
        if (running && retrig != 0 && noteStarted_)
        {
            if (! envDone_)
            {
                notePhase_ += stepCycles;
                if (notePhase_ >= 1) { if (retrig == 2) { envDone_ = true; notePhase_ = 1; } else notePhase_ -= std::floor(notePhase_); }
            }
        }
        else if (running)
        {
            freePhase_ += stepCycles;
            freePhase_ -= std::floor(freePhase_);
        }
    }
    for (int i = 0; i < ccCount; ++i)
        midi.addEvent(juce::MidiMessage::controllerEvent(1, std::clamp(ccOut - 1, 0, 127), ccEvents[(size_t) i].value), ccEvents[(size_t) i].sample);
    if (scopeBin_ >= 0 && ranThisBlock) storeScopeBin(scopeBin_);
    if (ranThisBlock || std::abs(lastPhase - displayPhase_.load(std::memory_order_relaxed)) > 1e-7f
        || std::abs(lastShaped - displayShaped_.load(std::memory_order_relaxed)) > 1e-7f)
        displayRevision_.fetch_add(1, std::memory_order_relaxed);
    displayRunning_.store(ranThisBlock, std::memory_order_relaxed);
    displayPhase_.store(lastPhase, std::memory_order_relaxed);
    displayShaped_.store(lastShaped, std::memory_order_relaxed);
    for (int d = 0; d < kDestinations; ++d) displayOutput_[(size_t) d].store(smoothed_[d], std::memory_order_relaxed);
}

} // namespace sonora::devices

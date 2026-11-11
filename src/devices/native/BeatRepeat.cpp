#include "devices/native/BeatRepeat.h"

#include <algorithm>
#include <cmath>

namespace sonora::devices
{
namespace
{
constexpr float kPi = 3.14159265f;

std::vector<ParameterInfo> beatRepeatParameters()
{
    using B = BeatRepeat;
    std::vector<ParameterInfo> p(B::NumParams);
    p[B::Chance] = makeParam("Chance", "Chance", 0, 1, 1, "%");
    p[B::Interval] = makeParam("Interval", "Interval", 0, B::kIntervals - 1, 5, "", false,
                               {"1/32", "1/16", "1/8", "1/4", "1/2", "1 Bar", "2 Bars", "4 Bars"});
    p[B::Offset] = makeParam("Offset", "Offset", 0, 15, 8, "/16");
    p[B::Grid] = makeParam("Grid", "Grid", 0, B::kGrids - 1, 6, "", false,
                           {"1/128", "1/96", "1/64", "1/48", "1/32", "1/24", "1/16", "1/12", "1/8", "1/6", "1/4", "1/3", "1/2",
                            "1 Bar", "2 Bars", "4 Bars"});
    p[B::BlockTriplets] = makeParam("BlockTripplets", "No Triplets", 0, 1, 0, "", true);
    p[B::GridChance] = makeParam("GridChance", "Variation", 0, 10, 0);
    p[B::GridChanceType] = makeParam("GridChanceType", "Variation Type", 0, 4, 0, "", false, {"Trigger", "1/4", "1/8", "1/16", "Auto"});
    std::vector<std::string> gates;
    for (int i = 0; i < B::kGates; ++i)
        gates.push_back(i < 16 ? std::to_string(i + 1) + "/16" : std::to_string(i - 14) + " Bars"); // .. 16/16, 2-4 Bars
    p[B::Gate] = makeParam("Gate", "Gate", 0, B::kGates - 1, 7, "", false, gates);
    p[B::DampVolume] = makeParam("DampVolume", "Decay", 0, 1, 0, "%");
    p[B::DampPitch] = makeParam("DampPitch", "Pitch Decay", 0, 1, 0, "%");
    p[B::BasePitch] = makeParam("BasePitch", "Pitch", 0, 12, 0, "st");
    p[B::MixType] = makeParam("MixType", "Mix Mode", 0, 2, 0, "", false, {"Mix", "Insert", "Gate"});
    p[B::WetLevel] = makeParam("WetLevel", "Volume", 0.00031622799f, 1.99526203f, 1.0f);
    p[B::FilterOn] = makeParam("FilterOn", "Filter", 0, 1, 1, "", true);
    p[B::MidFreq] = makeParam("MidFreq", "Frequency", 50, 18000, 1000, "Hz");
    p[B::BandWidth] = makeParam("BandWidth", "Width", 0.5f, 9, 4);
    p[B::InstantRepeat] = makeParam("InstantRepeat", "Repeat", 0, 1, 0, "", true);
    return p;
}

constexpr double kIntervalBeats[BeatRepeat::kIntervals] = {0.125, 0.25, 0.5, 1, 2, 4, 8, 16};
// Grid in beats (quarter notes): 1/128 .. 4 Bars, straight and triplet values alternating up to 1/3.
constexpr double kGridBeats[BeatRepeat::kGrids] = {4.0 / 128, 4.0 / 96, 4.0 / 64, 4.0 / 48, 4.0 / 32, 4.0 / 24, 4.0 / 16, 4.0 / 12,
                                                   4.0 / 8, 4.0 / 6, 1.0, 4.0 / 3, 2.0, 4.0, 8.0, 16.0};
} // namespace

BeatRepeat::BeatRepeat() : NativeDevice(beatRepeatParameters()) {}

double BeatRepeat::intervalBeats(int index) { return kIntervalBeats[std::clamp(index, 0, kIntervals - 1)]; }

double BeatRepeat::gridBeats(int index, bool noTriplets)
{
    index = std::clamp(index, 0, kGrids - 1);
    if (noTriplets && index < 12 && index % 2 == 1) --index; // a triplet value: the straight one below it
    return kGridBeats[index];
}

double BeatRepeat::gateBeats(int index)
{
    index = std::clamp(index, 0, kGates - 1);
    return index < 16 ? (index + 1) * 0.25 : (double) (index - 14) * 4.0; // 1/16 .. 16/16, then 2, 3, 4 bars
}

void BeatRepeat::prepare(double sampleRate, int)
{
    sampleRate_ = sampleRate > 0 ? sampleRate : 44100.0;
    int size = 1;
    while (size < (int) (20.0 * sampleRate_)) size <<= 1; // the longest gate (4 bars at 60 BPM = 16 s)
    for (auto& h : history_) h.assign((size_t) size, 0.0f);
    historyMask_ = size - 1;
    reset();
}

void BeatRepeat::reset()
{
    for (auto& h : history_) std::fill(h.begin(), h.end(), 0.0f);
    written_ = 0;
    active_ = instantWas_ = false;
    lastTriggerSlot_ = -1;
    fade_ = 0;
    for (auto& ch : filter_) std::fill(std::begin(ch), std::end(ch), 0.0f);
}

void BeatRepeat::updateFilter()
{
    const float mid = value(MidFreq), octaves = value(BandWidth);
    const float lo = std::clamp(mid / std::pow(2.0f, octaves * 0.5f), 20.0f, (float) sampleRate_ * 0.45f);
    const float hi = std::clamp(mid * std::pow(2.0f, octaves * 0.5f), 20.0f, (float) sampleRate_ * 0.45f);
    const auto gain = [&](float hz) { const float g = std::tan(kPi * hz / (float) sampleRate_); return g / (1.0f + g); };
    hpG_ = gain(lo);
    lpG_ = gain(hi);
}

void BeatRepeat::start()
{
    const double samplesPerBeat = 60.0 / std::clamp(host_.tempo, 20.0, 999.0) * sampleRate_;
    double grid = gridBeats((int) std::lround(value(Grid)), value(BlockTriplets) > 0.5f);
    // Variation: a finer grid chosen at random for this repeat (up to Variation halvings, 10 = about 5).
    const int variation = (int) std::lround(value(GridChance));
    if (variation > 0)
    {
        noise_ = noise_ * 1664525u + 1013904223u;
        const double halvings = std::floor((double) (noise_ >> 8) / 16777216.0 * ((double) variation * 0.5 + 1.0));
        grid = std::max(grid / std::pow(2.0, halvings), 4.0 / 128);
    }
    grid_ = std::max(16.0, grid * samplesPerBeat);
    gate_ = std::max(grid_, gateBeats((int) std::lround(value(Gate))) * samplesPerBeat);
    start_ = written_;
    pos_ = gridPos_ = 0;
    repeat_ = 0;
    active_ = true;
}

void BeatRepeat::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    const int n = audio.getNumSamples();
    if (n <= 0 || history_[0].empty()) return;
    float* left = audio.getWritePointer(0);
    float* right = audio.getNumChannels() > 1 ? audio.getWritePointer(1) : left;
    const bool bypass = isBypassed();
    updateFilter();
    const double beatsPerSample = std::clamp(host_.tempo, 20.0, 999.0) / 60.0 / sampleRate_;
    const double interval = intervalBeats((int) std::lround(value(Interval)));
    const double offset = std::fmod(value(Offset) * 0.25, interval);
    const bool instant = value(InstantRepeat) > 0.5f;
    const int mix = (int) std::lround(value(MixType));
    const float volume = value(WetLevel), decay = value(DampVolume), chance = value(Chance);
    const float basePitch = value(BasePitch), pitchDecay = value(DampPitch);
    const bool filterOn = value(FilterOn) > 0.5f;
    const float fadeStep = (float) (1.0 / (0.002 * sampleRate_));

    for (int i = 0; i < n; ++i)
    {
        const double beat = host_.beats + (double) i * beatsPerSample;
        if (instant && ! instantWas_) start(); // Repeat pressed: now, regardless of Interval and Chance
        else if (! instant && instantWas_ && active_) active_ = false;
        instantWas_ = instant;
        if (host_.playing && ! instant)
        {
            const double slot = std::floor((beat - offset) / interval + 1e-9);
            if (slot >= 0 && std::abs(slot - lastTriggerSlot_) > 0.5 && beat - offset - slot * interval < beatsPerSample * 1.5 + 1e-9)
            {
                noise_ = noise_ * 1664525u + 1013904223u;
                if (chance >= 1.0f || (float) (noise_ >> 8) / 16777216.0f < chance) start();
            }
            lastTriggerSlot_ = slot;
        }

        const float inL = left[i], inR = right[i];
        const auto w = (size_t) (written_ & historyMask_);
        history_[0][w] = inL;
        history_[1][w] = inR;
        ++written_;

        float wetL = 0, wetR = 0;
        const bool inRepeat = active_ && repeat_ > 0;
        if (active_)
        {
            const double t = pos_ / gate_;
            if (repeat_ == 0) { wetL = inL; wetR = inR; } // the first grid is the input itself
            else
            {
                const double semis = (double) basePitch + (double) pitchDecay * 12.0 * t;
                const double read = std::fmod(gridPos_ * std::pow(2.0, -semis / 12.0), grid_);
                const double index = (double) start_ + read;
                const auto i0 = (int64_t) index;
                const float frac = (float) (index - (double) i0);
                const auto a = (size_t) (i0 & historyMask_), b = (size_t) ((i0 + 1) & historyMask_);
                const float edge = (float) std::min({1.0, (gridPos_ + 1.0) / 48.0, (grid_ - gridPos_) / 48.0});
                const float gain = std::max(0.0f, 1.0f - decay * (float) t) * edge;
                wetL = (history_[0][a] + (history_[0][b] - history_[0][a]) * frac) * gain;
                wetR = (history_[1][a] + (history_[1][b] - history_[1][a]) * frac) * gain;
            }
            if (filterOn && repeat_ > 0)
            {
                float* io[2] = {&wetL, &wetR};
                for (int ch = 0; ch < 2; ++ch)
                {
                    auto& f = filter_[ch];
                    float x = *io[ch];
                    for (int s = 0; s < 2; ++s) // two high-pass stages
                    {
                        const float v = (x - f[s]) * hpG_;
                        const float low = v + f[s];
                        f[s] = low + v;
                        x -= low;
                    }
                    for (int s = 2; s < 4; ++s) // two low-pass stages
                    {
                        const float v = (x - f[s]) * lpG_;
                        const float low = v + f[s];
                        f[s] = low + v;
                        x = low;
                    }
                    *io[ch] = x;
                }
            }
            pos_ += 1.0;
            gridPos_ += 1.0;
            if (gridPos_ >= grid_) { gridPos_ -= grid_; ++repeat_; }
            if (! instant && pos_ >= gate_) active_ = false;
        }
        fade_ = inRepeat ? std::min(1.0f, fade_ + fadeStep) : std::max(0.0f, fade_ - fadeStep);
        if (bypass) continue;
        // Mix: input + repeats. Insert: the input is muted while repeating. Gate: only the repeats are heard.
        const float dry = mix == MixGate ? 0.0f : mix == MixInsert ? 1.0f - fade_ : 1.0f;
        const float wetGain = (repeat_ > 0 || ! active_) ? volume * fade_ : 0.0f;
        left[i] = inL * dry + wetL * wetGain;
        if (right != left) right[i] = inR * dry + wetR * wetGain;
    }
}

std::vector<DeviceProcessor::UiGroup> BeatRepeat::uiGroups() const
{
    return {{"Repeat", {Interval, Offset, Chance, Gate, InstantRepeat}, 3},
            {"Grid", {Grid, BlockTriplets, GridChance, GridChanceType}, 2},
            {"Mix", {MixType, WetLevel, DampVolume}, 2},
            {"Pitch", {BasePitch, DampPitch}, 1},
            {"Filter", {FilterOn, MidFreq, BandWidth}, 2}};
}
} // namespace sonora::devices

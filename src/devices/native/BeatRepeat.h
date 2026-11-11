#pragma once
// Beat Repeat (the reference's `BeatRepeat`, PLG-18b): every Interval (offset by Offset sixteenths) it may capture (Chance)
// one Grid of the input and repeat it until Gate sixteenths have passed. Built on the same idea as Sonora Stutter: the
// input is always recorded into a ring, the first grid of a repeat is the live input, later grids read the ring.
//
// Parameters keep the set's names and values (`.als` Manual values): Interval 0..7 = 1/32 .. 4 Bars, Offset 0..15
// sixteenths, Grid 0..15 = 1/128 .. 4 Bars (odd indices are triplets; No Triplets rounds them to the straight neighbour),
// Gate 0..18, GridChance (Variation) 0..10 with GridChanceType (Trigger, 1/4, 1/8, 1/16, Auto), DampVolume (Decay),
// BasePitch 0..12 semitones down, DampPitch (Pitch Decay), MixType (Mix, Insert, Gate), WetLevel (Volume, linear),
// FilterOn / MidFreq / BandWidth (octaves), InstantRepeat (Repeat).
// TODO(PLG-18b): Grid and Gate index -> length tables are read from the reference's presets, not measured against its
// output; check them with parity_probe stems (docs/PARITY.md) and fix the tables if a repeat lands off.

#include "devices/NativeDevice.h"

#include <cstdint>
#include <vector>

namespace sonora::devices
{
class BeatRepeat final : public NativeDevice
{
public:
    enum Param
    {
        Chance, Interval, Offset, Grid, BlockTriplets, GridChance, GridChanceType, Gate, DampVolume, DampPitch, BasePitch,
        MixType, WetLevel, FilterOn, MidFreq, BandWidth, InstantRepeat, NumParams
    };
    enum Mix { MixMix, MixInsert, MixGate };
    static constexpr int kIntervals = 8, kGrids = 16, kGates = 19;

    BeatRepeat();
    std::string name() const override { return "Beat Repeat"; }
    void prepare(double sampleRate, int maxBlockSize) override;
    void reset() override;
    void process(juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    void setHostInfo(const HostInfo& info) override { host_ = info; }
    std::vector<UiGroup> uiGroups() const override;

    // Lengths in beats for the index tables (exposed for tests).
    static double intervalBeats(int index);
    static double gridBeats(int index, bool noTriplets);
    static double gateBeats(int index);
    bool repeating() const { return active_; }

private:
    void start();
    void updateFilter();

    double sampleRate_ = 44100.0;
    HostInfo host_;
    std::vector<float> history_[2];
    int64_t written_ = 0;
    int historyMask_ = 0;
    bool active_ = false, instantWas_ = false;
    int64_t start_ = 0;       // where the capture began (absolute sample)
    double grid_ = 1, gate_ = 1, pos_ = 0, gridPos_ = 0;
    int repeat_ = 0;
    double lastTriggerSlot_ = -1;
    uint32_t noise_ = 0x2545f491u;
    float fade_ = 0;
    // Band filter (high-pass then low-pass, TPT one-poles x2 per side).
    float hpG_ = 0, lpG_ = 1, filter_[2][4]{};
};
} // namespace sonora::devices

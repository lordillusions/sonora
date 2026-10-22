#include "devices/native/WavetableEffects.h"
#include "devices/native/Biquad.h"
#include "devices/native/SerumDistortion.h"
#include "devices/native/SerumHall.h"
#include "devices/native/SerumPlate.h"
#include "dsp/Dsp.h"
#include "dsp/LadderFilter.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <functional>
#include <limits>
#include <new>
#include <set>
#include <stdexcept>

namespace sonora::devices
{
namespace
{
using E = WavetableEffects;
using Type = E::Type;
using PD = E::ParameterDescriptor;
constexpr double tau = 2 * dsp::kPi;
constexpr int controlPeriod = 16;
constexpr int partition = E::convolutionPartition;
constexpr PD descriptor(std::string_view id, std::string_view label, std::string_view unit,
                        float minimum, float maximum, float initial, bool discrete = false,
                        std::span<const std::string_view> choices = {})
{
    return {id, label, unit, minimum, maximum, initial, discrete, choices};
}
constexpr PD mix = descriptor("Mix", "Mix", "%", 0, 1, 1), output = descriptor("Output", "Output", "dB", -24, 24, 0);
constexpr PD linearOutput = descriptor("OutputGain", "Output Scale", "x", 0, 4, 1);
constexpr std::array<std::string_view, 2> compressorStyles{"Standard", "Instant Peak"};
constexpr std::array<std::string_view, 2> distortionStyles{"Standard", "Tabulated"};
constexpr std::array<std::string_view, 18> distortionModes{"Tube", "Soft Clip", "Hard Clip", "Diode 1", "Diode 2", "Linear Fold", "Sine Fold", "Zero Square", "Downsample",
                                                           "Asym", "Rectify", "X Shaper", "X Shaper Asym", "Sine Shaper", "Stomp Box", "Tape Sat", "Overdrive", "Soft Sat"};
constexpr std::array<std::string_view, 3> delayModes{"Normal", "Ping Pong", "Mono"};
constexpr std::array<std::string_view, 3> distortionFilterPlacement{"Off", "Pre", "Post"};
constexpr std::array<std::string_view, 6> distortionShapes{"Soft", "Clip", "Fold", "Asymmetric", "Rectify", "Crush"};
constexpr std::array<std::string_view, 5> eqShapes{"Low Shelf", "Peak", "High Shelf", "High Pass", "Low Pass"};
constexpr std::array<std::string_view, 8> filterShapes{"Low Pass", "High Pass", "Band Pass", "Notch", "MG Low 6", "MG Low 12", "MG Low 18", "MG Low 24"};
constexpr std::array<std::string_view, 2> slopes{"12 dB", "24 dB"};
constexpr std::array distortion{
    mix, output, descriptor("Drive", "Drive", "dB", 0, 36, 6), descriptor("Shape", "Shape", "", 0, 5, 0, true, distortionShapes),
    descriptor("Tone", "Tone", "Hz", 20, 20000, 16000), descriptor("Bias", "Bias", "", -1, 1, 0),
    descriptor("Bits", "Bits", "int", 4, 24, 12, true), descriptor("Downsample", "Hold", "int", 1, 64, 1, true), linearOutput,
    descriptor("Style", "Style", "", 0, 1, 0, true, distortionStyles), descriptor("Mode", "Mode", "", 0, 17, 0, true, distortionModes),
    descriptor("Amount", "Drive Amount", "%", 0, 100, 0), descriptor("Filter", "Filter", "", 0, 2, 0, true, distortionFilterPlacement),
    descriptor("FilterFreq", "Filter Freq", "", 0, 1, 0.5f), descriptor("FilterWidth", "Filter Bandwidth", "", 0, 10, 2),
    descriptor("FilterMorph", "Filter LP-HP", "%", 0, 100, 0), descriptor("Stages", "Overdrive Stages", "int", 1, 16, 1, true)};
constexpr std::array flanger{
    mix, output, descriptor("Rate", "Rate", "Hz", 0.01f, 20, 0.2f), descriptor("Depth", "Depth", "ms", 0, 15, 3),
    descriptor("Time", "Time", "ms", 0.05f, 20, 2), descriptor("Feedback", "Feedback", "", -0.95f, 0.95f, 0.4f),
    descriptor("Phase", "Stereo Phase", "turn", 0, 1, 0.25f), linearOutput};
constexpr std::array phaser{
    mix, output, descriptor("Rate", "Rate", "Hz", 0, 20, 0.3f), descriptor("Center", "Center", "Hz", 20, 20000, 2000),
    descriptor("Depth", "Depth", "oct", 0, 4, 2), descriptor("Feedback", "Feedback", "", -0.95f, 0.95f, 0.3f),
    descriptor("Stages", "Stages", "int", 2, 12, 6, true), descriptor("Phase", "Stereo Phase", "turn", 0, 1, 0.25f), linearOutput,
    descriptor("Style", "Style", "", 0, 1, 0, true, distortionStyles), descriptor("Freq", "Pole Freq", "Hz", 20, 18000, 600),
    descriptor("Mod", "Mod Depth", "%", 0, 1, 0.5f), descriptor("Spread", "Pole Spread", "%", 0, 1, 0.5f), descriptor("Width", "Width", "%", 0, 1, 0),
    descriptor("Poles", "Poles", "int", 1, 18, 4, true), descriptor("LfoPhase", "LFO Phase", "turn", 0, 1, 0)};
constexpr std::array chorus{
    mix, output, descriptor("Rate", "Rate", "Hz", 0.01f, 10, 0.6f), descriptor("Depth", "Depth", "ms", 0, 12, 3),
    descriptor("Time", "Time", "ms", 1, 40, 12), descriptor("Feedback", "Feedback", "", -0.8f, 0.8f, 0.1f),
    descriptor("Voices", "Voices", "int", 2, 6, 3, true), descriptor("Width", "Width", "%", 0, 1, 1), linearOutput};
constexpr std::array delay{
    mix, output, descriptor("Time", "Left Time", "s", 0.001f, 2, 0.25f), descriptor("RightTime", "Right Time", "s", 0.001f, 2, 0.375f),
    descriptor("Feedback", "Feedback", "%", 0, 1.0f, 0.35f), descriptor("LowCut", "Low Cut", "Hz", 20, 20000, 20),
    descriptor("HighCut", "High Cut", "Hz", 20, 20000, 15000), descriptor("PingPong", "Pingpong", "", 0, 1, 0, true),
    descriptor("Sync", "Tempo", "", 0, 1, 0, true), descriptor("Beats", "Left Beats", "beats", 0.03125f, 16, 1),
    descriptor("RightBeats", "Right Beats", "beats", 0.03125f, 16, 1.5f), linearOutput,
    descriptor("Style", "Style", "", 0, 1, 0, true, distortionStyles), descriptor("Mode", "Mode", "", 0, 2, 0, true, delayModes),
    descriptor("FilterFreq", "Filter Freq", "Hz", 20, 20000, 849), descriptor("FilterWidth", "Filter Bandwidth", "oct", 0.75f, 9.5f, 6.75f),
    descriptor("OffsetLeft", "Left Offset", "x", 0.5f, 1.5f, 1), descriptor("OffsetRight", "Right Offset", "x", 0.5f, 1.5f, 1),
    descriptor("Link", "Link", "", 0, 1, 0, true)};
constexpr std::array compressor{
    mix, output, descriptor("Threshold", "Threshold", "dB", -120, 0, -18), descriptor("Ratio", "Ratio", ":1", 1, 100, 4),
    descriptor("Attack", "Attack", "s", 0.0001f, 1, 0.01f), descriptor("Release", "Release", "s", 0.001f, 5, 0.1f),
    descriptor("Knee", "Knee", "dB", 0, 24, 6), descriptor("Makeup", "Makeup", "dB", -12, 31, 0),
    descriptor("Multiband", "Multiband", "", 0, 1, 0, true), descriptor("LowSplit", "Low Split", "Hz", 40, 5000, 250),
    descriptor("HighSplit", "High Split", "Hz", 300, 16000, 2500), descriptor("Upward", "Upward", "%", 0, 1, 0), linearOutput,
    descriptor("Style", "Style", "", 0, 1, 0, true, compressorStyles)};
constexpr std::array<std::string_view, 3> reverbStyles{"Standard", "Hall", "Plate"};
constexpr std::array reverb{
    mix, output, descriptor("Size", "Size", "%", 0.1f, 1, 0.6f), descriptor("Decay", "Decay", "s", 0.1f, 20, 3),
    descriptor("Predelay", "Predelay", "s", 0, 2.5f, 0.02f), descriptor("Damping", "Damping", "Hz", 20, 20000, 8000),
    descriptor("Diffusion", "Diffusion", "%", 0, 1, 0.7f), descriptor("Width", "Width", "%", 0, 1, 1), linearOutput,
    descriptor("Style", "Style", "", 0, 2, 0, true, reverbStyles),
    // Normalised controls of the measured reference engines (Hall, Plate), named after the reference: Size, Delay (decay time, Hall), Freq,
    // Feedback, FreqB and Width keep one meaning per engine.
    descriptor("RvSize", "Size", "%", 0, 1, 0.35f), descriptor("RvDelay", "Delay", "%", 0, 1, 0.35f), descriptor("RvFreq", "Freq", "%", 0, 1, 0),
    descriptor("RvFeedback", "Feedback", "%", 0, 1, 0.25f), descriptor("RvFreqB", "Freq B", "%", 0, 1, 0.35f), descriptor("RvWidth", "Width", "%", 0, 1, 0.2f)};
constexpr std::array eq{
    mix, output, descriptor("Band1Type", "Band 1 Type", "", 0, 4, 0, true, eqShapes), descriptor("Band1Freq", "Band 1 Freq", "Hz", 20, 20000, 209.617f),
    descriptor("Band1Gain", "Band 1 Gain", "dB", -24, 24, 0), descriptor("Band1Q", "Band 1 Q", "Q", 0.1f, 20, 0.776f),
    descriptor("Band2Type", "Band 2 Type", "", 0, 4, 2, true, eqShapes), descriptor("Band2Freq", "Band 2 Freq", "Hz", 20, 20000, 2040.536f),
    descriptor("Band2Gain", "Band 2 Gain", "dB", -24, 24, 0), descriptor("Band2Q", "Band 2 Q", "Q", 0.1f, 20, 0.776f), linearOutput};
constexpr std::array filter{
    mix, output, descriptor("Mode", "Mode", "", 0, 7, 0, true, filterShapes), descriptor("Cutoff", "Cutoff", "Hz", 20, 20000, 10000),
    descriptor("Resonance", "Resonance", "%", 0, 1, 0.1f), descriptor("Drive", "Drive", "dB", 0, 24, 0),
    descriptor("Slope", "Slope", "", 0, 1, 0, true, slopes), linearOutput};
constexpr std::array hyper{
    mix, output, descriptor("Rate", "Rate", "Hz", 0.01f, 20, 0.3f), descriptor("Detune", "Detune", "%", 0, 1, 0.2f),
    descriptor("Voices", "Voices", "int", 1, 7, 4, true), descriptor("DimensionSize", "Dimension Size", "%", 0, 1, 0.25f),
    descriptor("DimensionMix", "Dimension Mix", "%", 0, 1, 0.25f), descriptor("Width", "Width", "%", 0, 1, 1), linearOutput,
    descriptor("Style", "Style", "", 0, 1, 0, true, distortionStyles), descriptor("Unison", "Unison", "int", 0, 7, 4, true),
    descriptor("HyperRate", "Hyper Rate", "%", 0, 1, 0.4f), descriptor("HyperDetune", "Hyper Detune", "%", 0, 1, 0.5f),
    descriptor("DimeSize", "Dim Size", "%", 0, 1, 0.5f), descriptor("DimeWet", "Dim Wet", "%", 0, 1, 0), descriptor("DimeLevel", "Dim Level", "", 0, 1, 0.5f),
    descriptor("PhaseL0", "Phase L1", "", 0, 1, 0), descriptor("PhaseL1", "Phase L2", "", 0, 1, 0.125f), descriptor("PhaseL2", "Phase L3", "", 0, 1, 0.25f),
    descriptor("PhaseL3", "Phase L4", "", 0, 1, 0.375f), descriptor("PhaseL4", "Phase L5", "", 0, 1, 0.5f), descriptor("PhaseL5", "Phase L6", "", 0, 1, 0.625f),
    descriptor("PhaseL6", "Phase L7", "", 0, 1, 0.75f), descriptor("PhaseL7", "Phase L8", "", 0, 1, 0.875f),
    descriptor("PhaseR0", "Phase R1", "", 0, 1, 0.0625f), descriptor("PhaseR1", "Phase R2", "", 0, 1, 0.1875f), descriptor("PhaseR2", "Phase R3", "", 0, 1, 0.3125f),
    descriptor("PhaseR3", "Phase R4", "", 0, 1, 0.4375f), descriptor("PhaseR4", "Phase R5", "", 0, 1, 0.5625f), descriptor("PhaseR5", "Phase R6", "", 0, 1, 0.6875f),
    descriptor("PhaseR6", "Phase R7", "", 0, 1, 0.8125f), descriptor("PhaseR7", "Phase R8", "", 0, 1, 0.9375f)};
constexpr std::array shift{
    mix, output, descriptor("Shift", "Shift", "Hz", -1000, 1000, 0), descriptor("Fine", "Fine", "Hz", -1, 1, 0),
    descriptor("Feedback", "Feedback", "", -0.9f, 0.9f, 0), descriptor("Width", "Width", "%", 0, 1, 1), linearOutput};
constexpr std::array convolution{mix, output, linearOutput};
constexpr std::array utility{
    mix, output, descriptor("Width", "Width", "%", 0, 2, 1), descriptor("Pan", "Pan", "", -1, 1, 0),
    descriptor("Mono", "Mono", "", 0, 1, 0, true), descriptor("Swap", "Swap", "", 0, 1, 0, true),
    descriptor("InvertLeft", "Invert Left", "", 0, 1, 0, true), descriptor("InvertRight", "Invert Right", "", 0, 1, 0, true), linearOutput,
    descriptor("HighPass", "High Pass", "Hz", 0, 400, 0), descriptor("LowPass", "Low Pass", "Hz", 50, 24000, 24000),
    descriptor("LowMono", "Low Mono", "", 0, 1, 0, true), descriptor("LowMonoFreq", "Low Mono Freq", "Hz", 20, 400, 90),
    descriptor("DcBlock", "DC Block", "", 0, 1, 0, true)};
constexpr std::array split{
    mix, output, descriptor("LowSplit", "Low Split", "Hz", 40, 5000, 250), descriptor("HighSplit", "High Split", "Hz", 300, 16000, 2500),
    descriptor("LowGain", "Low Gain", "dB", -24, 24, 0), descriptor("MidGain", "Mid Gain", "dB", -24, 24, 0),
    descriptor("HighGain", "High Gain", "dB", -24, 24, 0), linearOutput};

float finiteSample(float x) { return std::isfinite(x) ? std::clamp(x, -128.0f, 128.0f) : 0; }
float routingMaximum(size_t index) { return index == 2 || index == 3 || index == 5 ? 4.0f : 2.0f; }
// Serum 2 Hyper (measured): voice sawtooth rates are Rate^2 times these ratios (Hz), left and right voice sets differ.
constexpr std::array<double, 8> hyperRatioLeft{0.98047643, 1.06216538, 0.88997686, 1.2968395546510096, 0.7071067811865475, 1.5422108254079407, 0.5946035575013605, 1.8340080864093424};
constexpr std::array<double, 8> hyperRatioRight{1.01991221, 0.9371156, 1.10745242, 0.773782496771195, 1.4142135623730951, 0.6484197773255048, 1.681792830507429, 0.5452538663326288};
// Serum 2 Phaser (measured): pole-pair spread multipliers s_k; coefficient_k = 2 sin(pi (1 + s_k * spread) * F / fs) with F = 20 * 900^(u * 0.98268...) .
constexpr std::array<float, 18> phaserSpread{3.0f, 1.0f, -0.5f, -0.75f, 0.75f, 0.5f, -1.0f / 3, 2.0f / 3, 0.2f, -0.4f, 0.717f, -0.123f, 1.3f, -0.1f, 0.1f, -0.717f, 0.8f, -0.9f};
// Serum 2 Delay feedback filter (measured): unity-peak band-pass designed at a fixed 44.1 kHz and run at any rate, Q from the bandwidth control.
double delayFilterQ(float bandwidth) noexcept
{
    static constexpr std::array<double, 14> bw{0.75, 1.0, 1.25, 1.5, 2.0, 2.5, 3.0, 3.5, 4.0, 5.0, 6.0, 7.0, 8.0, 9.5};
    static constexpr std::array<double, 14> q{1.0606, 0.7769, 0.6026, 0.4834, 0.3289, 0.2328, 0.168, 0.1225, 0.0899, 0.0489, 0.0266, 0.0145, 0.0079, 0.0030};
    const double b = std::clamp<double>(bandwidth, 0.75, 9.5);
    size_t i = 1;
    while (i + 1 < bw.size() && b > bw[i]) ++i;
    const double t = (b - bw[i - 1]) / (bw[i] - bw[i - 1]);
    return std::exp(std::log(q[i - 1]) + (std::log(q[i]) - std::log(q[i - 1])) * t);
}
// Serum 2 Delay tempo sync: the stored time value selects a power-of-two note length (in beats, 1/16 .. 8) at fixed thresholds (measured).
double delaySyncBeats(float stored) noexcept
{
    static constexpr std::array<double, 7> threshold{0.00612, 0.01501, 0.03225, 0.06197, 0.10902, 0.17926, 0.27914};
    double beats = 1.0 / 16.0;
    for (const double limit : threshold)
        if (stored > limit) beats *= 2.0;
    return beats;
}
BiquadCoeffs delayFilter(float freq, float bandwidth) noexcept
{
    const double w = std::tan(dsp::kPi * std::clamp<double>(freq, 20.0, 20000.0) / 44100.0), k = 1.0 / delayFilterQ(bandwidth);
    const double a0 = 1 + w * w + w * k, b0 = w * k / a0;
    return {b0, 0.0, -b0, (2 * w * w - 2) / a0, (1 + w * w - w * k) / a0};
}
float balanceLeft(float pan) { return std::min(1.0f, 1.0f - pan); }
float balanceRight(float pan) { return std::min(1.0f, 1.0f + pan); }
float coefficient(float hz, double sampleRate) { return 1 - std::exp(static_cast<float>(-tau * std::min<double>(hz, sampleRate * 0.45) / sampleRate)); }
double wrap(double phase) { return phase - std::floor(phase); }
int selection(float x, int count) { return std::clamp(static_cast<int>(std::lround(x)), 0, count - 1); }
const std::array<float, 2049> sineTable = [] {
    std::array<float, 2049> result{};
    for (size_t i = 0; i < result.size(); ++i) result[i] = static_cast<float>(std::sin(tau * static_cast<double>(i) / 2048));
    return result;
}();
float sineAt(double phase)
{
    const double position = wrap(phase) * 2048;
    const auto first = static_cast<size_t>(position);
    return sineTable[first] + (sineTable[first + 1] - sineTable[first]) * static_cast<float>(position - static_cast<double>(first));
}
struct Stereo { float left = 0, right = 0; };
// Reset is also called from MIDI/bypass on the audio thread. Track valid history instead of
// clearing potentially hundreds of MiB of prepared delay storage in one callback.
class PreparedDelay
{
public:
    void prepare(int length) { line_.prepare(length); valid_ = 0; }
    void reset() { valid_ = 0; }
    int capacity() const { return line_.capacity(); }
    void write(float value) { line_.write(value); valid_ = std::min(valid_ + 1, capacity()); }
    float read(float delaySamples) const
    {
        if (valid_ == 0) return 0;
        const float bounded = std::clamp(delaySamples, 0.0f, static_cast<float>(capacity() - 2));
        const int first = static_cast<int>(bounded);
        if (first >= valid_) return 0;
        if (first + 1 >= valid_) return line_.read(static_cast<float>(first)) * (1 - (bounded - static_cast<float>(first)));
        return line_.read(bounded);
    }
private:
    dsp::FractionalDelay line_;
    int valid_ = 0;
};
float distortionTransfer(float value, int shape)
{
    if (shape == 0) return std::tanh(value);
    if (shape == 1) return std::clamp(value, -1.0f, 1.0f);
    if (shape == 2) return 1 - 4 * std::abs(static_cast<float>(wrap(value * 0.25 + 0.25)) - 0.5f);
    if (shape == 3) return value >= 0 ? std::tanh(value) : std::tanh(value * 0.5f) * 0.5f;
    return std::abs(std::tanh(value)) * 2;
}
Stereo alignSignal(std::array<PreparedDelay, 2>& line, int latency, Stereo source)
{
    if (latency <= 0) return source;
    const Stereo result{line[0].read(static_cast<float>(latency - 1)), line[1].read(static_cast<float>(latency - 1))};
    line[0].write(source.left); line[1].write(source.right);
    return result;
}
using Complex = std::complex<float>;

template <int size>
struct FftPlan
{
    std::array<int, size> reverse{};
    std::array<Complex, size / 2> roots{};
    FftPlan()
    {
        for (int i = 0; i < size; ++i)
        {
            int value = i, reversed = 0;
            for (int bit = 1; bit < size; bit <<= 1) { reversed = (reversed << 1) | (value & 1); value >>= 1; }
            reverse[static_cast<size_t>(i)] = reversed;
        }
        for (int i = 0; i < size / 2; ++i)
        {
            const double angle = -tau * i / size;
            roots[static_cast<size_t>(i)] = {static_cast<float>(std::cos(angle)), static_cast<float>(std::sin(angle))};
        }
    }
    void transform(std::array<Complex, size>& data, bool inverse) const
    {
        for (int i = 0; i < size; ++i) if (reverse[static_cast<size_t>(i)] > i)
            std::swap(data[static_cast<size_t>(i)], data[static_cast<size_t>(reverse[static_cast<size_t>(i)])]);
        for (int length = 2; length <= size; length <<= 1)
            for (int start = 0; start < size; start += length)
                for (int i = 0; i < length / 2; ++i)
                {
                    Complex root = roots[static_cast<size_t>(i * size / length)];
                    if (inverse) root = std::conj(root);
                    const auto even = static_cast<size_t>(start + i), odd = static_cast<size_t>(start + i + length / 2);
                    const Complex product = data[odd] * root, original = data[even];
                    data[even] = original + product; data[odd] = original - product;
                }
        if (inverse) for (auto& value : data) value *= 1.0f / size;
    }
};
template <int blockSize>
struct ConvolutionStage
{
    static constexpr int transformSize = blockSize * 2, bins = blockSize + 1;
    FftPlan<transformSize> fft;
    std::vector<Complex> kernel, history;
    std::array<std::array<float, blockSize>, 2> input{}, queue{}, overlap{};
    int partitions = 0, channels = 1, cursor = 0, head = 0, validPartitions = 0;
    void reset()
    {
        input = {}; queue = {}; overlap = {}; cursor = 0; head = 0; validPartitions = 0;
    }
    Stereo next(Stereo source)
    {
        if (partitions == 0) return {};
        const auto sample = static_cast<size_t>(cursor);
        const Stereo result{queue[0][sample], queue[1][sample]};
        input[0][sample] = source.left; input[1][sample] = source.right;
        if (++cursor == blockSize)
        {
            cursor = 0;
            validPartitions = std::min(partitions, validPartitions + 1);
            for (int channel = 0; channel < 2; ++channel)
            {
                std::array<Complex, transformSize> spectrum{};
                for (int i = 0; i < blockSize; ++i) spectrum[static_cast<size_t>(i)] = input[static_cast<size_t>(channel)][static_cast<size_t>(i)];
                fft.transform(spectrum, false);
                const auto slot = static_cast<size_t>((channel * partitions + head) * bins);
                for (int bin = 0; bin < bins; ++bin) history[slot + static_cast<size_t>(bin)] = spectrum[static_cast<size_t>(bin)];
                spectrum = {};
                for (int part = 0; part < validPartitions; ++part)
                {
                    const int past = (head + partitions - part) % partitions;
                    const auto h = static_cast<size_t>((channel * partitions + past) * bins);
                    const auto k = static_cast<size_t>((std::min(channel, channels - 1) * partitions + part) * bins);
                    for (int bin = 0; bin < bins; ++bin) spectrum[static_cast<size_t>(bin)] += history[h + static_cast<size_t>(bin)] * kernel[k + static_cast<size_t>(bin)];
                }
                for (int bin = 1; bin < transformSize / 2; ++bin) spectrum[static_cast<size_t>(transformSize - bin)] = std::conj(spectrum[static_cast<size_t>(bin)]);
                fft.transform(spectrum, true);
                for (int i = 0; i < blockSize; ++i)
                {
                    const auto at = static_cast<size_t>(i), ch = static_cast<size_t>(channel);
                    queue[ch][at] = spectrum[at].real() + overlap[ch][at];
                    overlap[ch][at] = spectrum[at + blockSize].real();
                }
            }
            head = (head + 1) % partitions;
        }
        return result;
    }
};

struct ConvolutionState
{
    ConvolutionStage<E::convolutionPartition> early;
    std::unique_ptr<ConvolutionStage<E::convolutionTailPartition>> tail;
    std::array<PreparedDelay, 2> tailAlignment;
    static constexpr int tailDelay = E::convolutionHeadFrames + E::convolutionPartition - E::convolutionTailPartition;
    void reset()
    {
        early.reset();
        if (tail) tail->reset();
        for (auto& line : tailAlignment) line.reset();
    }
    Stereo next(Stereo source)
    {
        Stereo result = early.next(source);
        if (tail)
        {
            const Stereo late = alignSignal(tailAlignment, tailDelay, tail->next(source));
            result.left += late.left; result.right += late.right;
        }
        return result;
    }
};

struct ModuleState
{
    E::ModuleId id = 0;
    Type type = Type::Utility;
    std::atomic<bool> enabled{true};
    std::array<std::atomic<float>, E::maxParameters> parameter{};
    std::array<float, E::maxParameters> p{};
    bool active = true;
    int outputGainParameter = -1;
    std::array<PreparedDelay, 14> delay;
    std::array<PreparedDelay, 2> dryAlignment;
    std::array<std::array<PreparedDelay, 2>, 3> bandAlignment;
    std::array<int, 3> bandLatency{};
    int latency = 0;
    std::array<dsp::Svf, 4> svf;
    std::array<dsp::LadderFilter, 2> ladder;
    std::array<dsp::Crossover, 6> crossover;
    // The RBJ filters below run in state-variable form: modulated or automated frequencies must not click (DEV-24).
    std::array<SvfCoeffs, 2> eqCoefficients{};
    std::array<std::array<SvfState, 2>, 2> eqState{};
    std::array<std::array<float, 12>, 2> allpassX{}, allpassY{};
    std::array<std::array<float, 64>, 2> hilbertInput{};
    std::array<float, 63> hilbert{};
    int hilbertCursor = 0, holdCountdown = 0;
    std::array<float, 2> previous{}, held{}, tone{}, low{};
    std::array<float, 8> damping{}, reverbTime{}, reverbFeedback{};
    std::array<float, 3> envelope{}, compressionGain{1, 1, 1}, bandGain{1, 1, 1};
    std::array<std::array<int, E::maxBandModules>, 3> child{};
    std::array<int, 3> childCount{};
    std::unique_ptr<ConvolutionState> convolution;
    double phase = 0, increment = 0;
    bool shapedMix = false;
    float wet = 1, dry = 0, outputGain = 1, drive = 1, toneCoefficient = 1, distortionBaseline = 0;
    float lowCoefficient = 1, attackCoefficient = 1, releaseCoefficient = 1, makeup = 1;
    // "Instant Peak" compressor (measured Serum 2 behaviour): 12 Hz input DC blocker, per-sample peak level, reduction smoothed in dB.
    // Utility filters (measured Serum 2 Utils): Linkwitz-Riley 4th-order high-pass, Butterworth 2nd-order low-pass, side-channel LR4 for low mono.
    SvfCoeffs highPassCoefficient, lowPassCoefficient, lowMonoCoefficient;
    std::array<std::array<SvfState, 2>, 2> highPassState{};
    std::array<SvfState, 2> lowPassState{};
    std::array<SvfState, 2> lowMonoState{};
    bool highPassActive = false, lowPassActive = false, dcBlock = false;
    float dcCoefficient = 0;
    std::array<float, 2> dcInput{}, dcOutput{};
    bool serumHyper = false;
    int hyperVoices = 4;
    double hyperDepth = 0;
    std::array<std::array<double, 8>, 2> hyperPhase{}, hyperIncrement{};
    float hyperWet = 0, hyperDry = 1, hyperFilterG = 0, hyperFilterK = 1.3675f;
    std::array<float, 2> hyperLow{}, hyperBand{};
    bool dimeActive = false;
    float dimeWet = 0, dimeGain = 1, dimePhase = 0, dimeIncrement = 0;
    std::array<double, 4> dimeDelay{};
    bool serumPhaser = false;
    int phaserPoles = 4;
    std::array<float, 18> phaserCoefficient{};
    float phaserModulation = 0, phaserWidth = 0, phaserQ = 0, phaserFeedback = 0;
    double phaserPhaseIncrement = 0, phaserPhase = 0;
    std::array<std::array<std::array<float, 2>, 18>, 2> phaserState{};
    std::array<float, 2> phaserPrevious{};
    bool serumDelay = false;
    int delayMode = 0;
    std::array<float, 2> delayFeedback{};
    std::array<float, 2> feedbackPipe{}; // one sample of extra delay in the feedback path
    SvfCoeffs delayFilterCoefficient;
    std::array<SvfState, 2> delayFilterState;
    bool serumDistortion = false;
    int serumMode = 0, serumFilter = 0, holdCounter = 0, holdCursor = 0;
    std::array<float, 2> serumHeld{};
    std::array<std::array<float, 8>, 2> holdPipe{}; // Downsample delays its wet path by 7 samples (an 8-slot ring)
    float serumDrive = 0;
    std::array<serum_distortion::Oversampler, 2> oversampler;
    std::array<serum_distortion::MorphFilter, 2> morph;
    serum_distortion::Overdrive overdrive;
    int overdriveStages = 1, holdScale = 96;
    double overdriveRate = 0;
    bool serumHall = false, serumPlate = false;
    std::unique_ptr<serum_hall::Hall> hall;
    std::unique_ptr<serum_plate::Plate> plate;
    bool instantPeak = false;
    float reductionDb = 0, thresholdLog = 0, reductionSlope = 0, blockerCoefficient = 0;
    std::array<float, 2> blockerInput{}, blockerOutput{};
    std::array<float, 2> delayTime{}, allpassCoefficient{};
    void reset()
    {
        for (auto& line : delay) if (line.capacity() > 0) line.reset();
        for (auto& line : dryAlignment) if (line.capacity() > 0) line.reset();
        for (auto& band : bandAlignment) for (auto& line : band) if (line.capacity() > 0) line.reset();
        for (auto& f : svf) f.reset();
        for (auto& f : ladder) f.reset();
        for (auto& f : crossover) f.reset();
        for (auto& channel : eqState) for (auto& band : channel) band.reset();
        allpassX = {}; allpassY = {}; hilbertInput = {}; previous = {}; held = {}; tone = {}; low = {};
        damping = {}; envelope = {}; compressionGain = {1, 1, 1};
        reductionDb = 0; blockerInput = {}; blockerOutput = {};
        for (auto& o : oversampler) o.reset();
        for (auto& f : morph) f.reset();
        holdCounter = 0; holdCursor = 0; serumHeld = {}; holdPipe = {};
        overdrive.reset();
        if (hall) hall->reset();
        if (plate) plate->reset();
        for (auto& c : highPassState) for (auto& b : c) b.reset();
        for (auto& b : lowPassState) b.reset();
        for (auto& b : lowMonoState) b.reset();
        dcInput = {}; dcOutput = {};
        for (auto& b : delayFilterState) b.reset();
        feedbackPipe = {};
        phaserState = {}; phaserPrevious = {}; phaserPhase = p[15];
        hyperLow = {}; hyperBand = {}; dimePhase = 0;
        for (size_t c = 0; c < 2; ++c) for (size_t j = 0; j < 8; ++j) hyperPhase[c][j] = p[16 + c * 8 + j];
        phase = 0; hilbertCursor = 0; holdCountdown = 0;
        if (convolution) convolution->reset();
    }
};

std::array<Stereo, 3> splitBands(ModuleState& state, Stereo source)
{
    std::array<Stereo, 3> bandsOut{};
    for (int channel = 0; channel < 2; ++channel)
    {
        const auto ch = static_cast<size_t>(channel);
        float lowBand = 0, remainder = 0, midBand = 0, highBand = 0, alignLow = 0, alignHigh = 0;
        state.crossover[ch].process(channel == 0 ? source.left : source.right, lowBand, remainder);
        state.crossover[ch + 2].process(remainder, midBand, highBand);
        state.crossover[ch + 4].process(lowBand, alignLow, alignHigh);
        if (channel == 0) { bandsOut[0].left = alignLow + alignHigh; bandsOut[1].left = midBand; bandsOut[2].left = highBand; }
        else { bandsOut[0].right = alignLow + alignHigh; bandsOut[1].right = midBand; bandsOut[2].right = highBand; }
    }
    return bandsOut;
}
float compressorTarget(float level, const ModuleState& state)
{
    const float db = dsp::gainToDb(level), over = db - state.p[2], knee = state.p[6];
    float reduction = 0;
    if (over > knee * 0.5f) reduction = -(1 - 1 / state.p[3]) * over;
    else if (knee > 0 && over > -knee * 0.5f)
        reduction = -(1 - 1 / state.p[3]) * (over + knee * 0.5f) * (over + knee * 0.5f) / (2 * knee);
    // Own bounded upward option, applied below threshold without raising true silence.
    if (db < state.p[2] && level > 1e-5f) reduction += std::min(24.0f, (state.p[2] - db) * state.p[11] * 0.5f);
    return dsp::dbToGain(reduction) * state.makeup;
}
Stereo compressInstantPeak(ModuleState& state, Stereo source)
{
    // 12 Hz DC blocker on the input; the blocked signal is both the dry path and the compressor input.
    const float a = state.blockerCoefficient;
    const float left = a * (state.blockerOutput[0] + source.left - state.blockerInput[0]);
    const float right = a * (state.blockerOutput[1] + source.right - state.blockerInput[1]);
    state.blockerInput = {source.left, source.right}; state.blockerOutput = {left, right};
    const float level = std::max(std::abs(left), std::abs(right));
    // Hard-knee reduction of the instantaneous peak, smoothed in dB (attack when the reduction deepens).
    const float thresholdAmplitude = dsp::dbToGain(state.thresholdLog);
    const float target = level > thresholdAmplitude ? state.reductionSlope * dsp::gainToDb(level / thresholdAmplitude) : 0.0f;
    state.reductionDb += (target - state.reductionDb) * (target > state.reductionDb ? state.attackCoefficient : state.releaseCoefficient);
    const float gain = dsp::dbToGain(-state.reductionDb) * state.makeup;
    const float wet = state.wet, dry = 1.0f - state.wet;
    return {left * dry + left * gain * wet, right * dry + right * gain * wet};
}
Stereo compress(ModuleState& state, Stereo source, int band)
{
    const auto at = static_cast<size_t>(band);
    const float level = std::max(std::abs(source.left), std::abs(source.right));
    const float target = compressorTarget(level, state);
    state.compressionGain[at] += (target - state.compressionGain[at]) * (target < state.compressionGain[at] ? state.attackCoefficient : state.releaseCoefficient);
    return {source.left * state.compressionGain[at], source.right * state.compressionGain[at]};
}

void derive(ModuleState& state, double sampleRate, double tempo)
{
    const auto& p = state.p;
    const float sine = p[0] <= 0 ? 0 : p[0] >= 1 ? 1 : static_cast<float>(std::sin(p[0] * dsp::kPi * 0.5));
    state.wet = state.shapedMix ? sine * sine : sine;
    state.dry = state.shapedMix ? 1 - state.wet * state.wet
        : p[0] <= 0 ? 1 : p[0] >= 1 ? 0 : static_cast<float>(std::cos(p[0] * dsp::kPi * 0.5));
    state.outputGain = dsp::dbToGain(p[1]) * p[static_cast<size_t>(state.outputGainParameter)];
    switch (state.type)
    {
        case Type::Distortion:
            state.drive = dsp::dbToGain(p[2]); state.toneCoefficient = coefficient(p[4], sampleRate);
            state.distortionBaseline = distortionTransfer(p[5], selection(p[3], 6));
            state.serumDistortion = p[9] >= 0.5f;
            if (state.serumDistortion)
            {
                state.serumMode = selection(p[10], 18); state.serumDrive = p[11] * 0.01f; state.serumFilter = selection(p[12], 3);
                for (auto& f : state.morph) f.setup(sampleRate, p[13], p[14], p[15] * 0.01f);
                state.overdriveStages = static_cast<int>(std::lround(p[16]));
                state.holdScale = static_cast<int>(sampleRate * 0.002 + 0.5);
                if (std::abs(state.overdriveRate - sampleRate) > 1e-9) { state.overdrive.prepare(sampleRate); state.overdriveRate = sampleRate; }
            }
            break;
        case Type::HyperDimension:
            state.serumHyper = p[9] >= 0.5f;
            if (state.serumHyper)
            {
                const double rate = static_cast<double>(p[11]) * p[11];
                state.hyperVoices = std::clamp(static_cast<int>(std::lround(p[10])), 0, 7);
                for (size_t j = 0; j < 8; ++j)
                {
                    state.hyperIncrement[0][j] = rate * hyperRatioLeft[j] / sampleRate;
                    state.hyperIncrement[1][j] = rate * hyperRatioRight[j] / sampleRate;
                }
                state.hyperDepth = (static_cast<double>(p[12]) * 0.04 + 0.0004) * sampleRate;
                const double q = 0.5 - 0.5 * std::cos(dsp::kPi * std::clamp<double>(p[0], 0.0, 1.0));
                state.hyperWet = static_cast<float>(q); state.hyperDry = static_cast<float>(0.99785 - 0.55366 * q);
                if (state.hyperVoices == 0) { state.hyperWet = 0; state.hyperDry = 0.99785f; } // no voices: only the fixed dry gain remains
                const double s = std::sin(dsp::kPi * std::min(120.0 / sampleRate, 0.25));
                state.hyperFilterG = static_cast<float>(2 * s);
                state.hyperFilterK = static_cast<float>(std::min({1.0 / s - s, 2.0, 1.3675}));
                state.dimeActive = p[14] > 0;
                state.dimeWet = p[14] * 0.25f; state.dimeGain = 4.0f * p[15] * p[15];
                const double d = std::max((static_cast<double>(p[13]) * 0.1 + 0.015) * sampleRate, 0.015001 * sampleRate);
                state.dimeDelay = {d, d * 1.3924, d * 1.643032, d * 1.18};
                state.dimeIncrement = static_cast<float>(0.45454547 / sampleRate);
                break;
            }
            state.increment = p[2] / sampleRate; break;
        case Type::Flanger: case Type::Chorus: state.increment = p[2] / sampleRate; break;
        case Type::Phaser:
            state.serumPhaser = p[9] >= 0.5f;
            if (state.serumPhaser)
            {
                const double frequency = 20.0 * std::pow(std::clamp<double>(p[10], 20.0, 18000.0) / 20.0, 6.6846 / std::log(900.0));
                state.phaserPoles = std::clamp(static_cast<int>(std::lround(p[14])), 1, 18);
                for (size_t k = 0; k < 18; ++k)
                    state.phaserCoefficient[k] = static_cast<float>(2.0 * std::sin(dsp::kPi * (1.0 + phaserSpread[k] * p[12]) * std::min(frequency, 10000.0) / sampleRate));
                state.phaserModulation = static_cast<float>(16.0 * dsp::kPi * std::pow(p[11], 3.0));
                state.phaserWidth = p[13];
                state.phaserFeedback = 0.99f * p[5];
                const double q = 0.5 - 0.5 * std::cos(dsp::kPi * 0.5 * std::clamp<double>(p[0], 0.0, 1.0));
                state.phaserQ = static_cast<float>(q);
                state.phaserPhaseIncrement = p[2] / sampleRate;
                break;
            }
            state.increment = p[2] / sampleRate;
            for (int channel = 0; channel < 2; ++channel)
            {
                const float hz = static_cast<float>(std::min<double>(sampleRate * 0.45, p[3] * std::exp2(p[4] * sineAt(state.phase + static_cast<float>(channel) * p[7]))));
                const float tangent = std::tan(static_cast<float>(dsp::kPi * hz / sampleRate));
                state.allpassCoefficient[static_cast<size_t>(channel)] = (1 - tangent) / (1 + tangent);
            }
            break;
        case Type::Delay:
            state.delayTime[0] = static_cast<float>(std::clamp(p[8] >= 0.5f ? p[9] * 60 / tempo : static_cast<double>(p[2]), 1 / sampleRate, 2.0) * sampleRate);
            state.delayTime[1] = static_cast<float>(std::clamp(p[8] >= 0.5f ? p[10] * 60 / tempo : static_cast<double>(p[3]), 1 / sampleRate, 2.0) * sampleRate);
            state.toneCoefficient = coefficient(p[6], sampleRate); state.lowCoefficient = coefficient(p[5], sampleRate);
            state.serumDelay = p[12] >= 0.5f;
            if (state.serumDelay)
            {
                // Measured: delay = offset * (time * rate - 50 samples), at most about half a second; Link makes the left delay follow the right time.
                const auto samples = [&](float time, float rawOffset) {
                    // The reference does not interpolate: the delay is rounded up to a whole number of samples. Free-running time is capped at
                    // half a second before the offset (0.5 .. 1.5) multiplies it; synced timing is a power-of-two number of beats minus two samples.
                    const double offset = std::clamp<double>(rawOffset, 0.5, 1.5);
                    const double exact = p[8] >= 0.5f ? offset * (delaySyncBeats(time) * 60.0 / tempo * sampleRate - 2.0)
                                                      : offset * std::min(static_cast<double>(time) * sampleRate - 50.0, 0.5 * sampleRate - 2.0);
                    return std::clamp(std::ceil(exact - 1e-3), 1.0, 6.0 * sampleRate);
                };
                const float right = p[3];
                state.delayTime[1] = static_cast<float>(samples(right, p[17]));
                state.delayTime[0] = static_cast<float>(samples(p[18] >= 0.5f ? right : p[2], p[16]));
                state.delayMode = selection(p[13], 3);
                // Measured: the longer delay is fed back less so both decay at the same rate per second: gain = feedback ^ (delay / shortest delay).
                // Ping-pong closes its loop through the left delay (same exponent as the left channel); the mono mode uses the plain feedback.
                const double shortest = std::min(state.delayTime[0], state.delayTime[1]);
                const double base = std::clamp<double>(p[4], 0.0, 1.0);
                state.delayFeedback[0] = static_cast<float>(state.delayMode == 2 ? base : std::pow(base, state.delayTime[0] / shortest));
                state.delayFeedback[1] = static_cast<float>(state.delayMode == 2 ? base : std::pow(base, state.delayTime[1] / shortest));
                state.delayFilterCoefficient = SvfCoeffs::fromBiquad(delayFilter(p[14], p[15]));
            }
            break;
        case Type::Compressor:
            state.attackCoefficient = dsp::timeToCoef(p[4] * 1000, sampleRate);
            state.releaseCoefficient = dsp::timeToCoef(p[5] * 1000, sampleRate); state.makeup = dsp::dbToGain(p[7]);
            state.instantPeak = p[13] >= 0.5f;
            if (state.instantPeak)
            {
                // Ratios from ~53:1 select the reference's brickwall regime: the attack is instant and the full overshoot is removed.
                constexpr float brickwallRatio = 53.25f;
                state.thresholdLog = p[2];
                state.reductionSlope = p[3] >= brickwallRatio ? 1.0f : 1.0f - 1.0f / std::max(1.0f, p[3]);
                if (p[3] >= brickwallRatio) state.attackCoefficient = 1.0f;
                state.blockerCoefficient = static_cast<float>(std::exp(-2.0 * dsp::kPi * 12.0 / sampleRate));
                state.wet = p[0]; state.dry = 0; // linear mix, applied inside the module
            }
            [[fallthrough]];
        case Type::Split3:
        {
            const float lower = p[state.type == Type::Split3 ? 2 : 9];
            const float upper = std::max(lower * 1.05f, p[state.type == Type::Split3 ? 3 : 10]);
            for (int channel = 0; channel < 2; ++channel)
            {
                state.crossover[static_cast<size_t>(channel)].setup(sampleRate, lower);
                state.crossover[static_cast<size_t>(channel + 2)].setup(sampleRate, upper);
                state.crossover[static_cast<size_t>(channel + 4)].setup(sampleRate, upper);
            }
            if (state.type == Type::Split3) for (int band = 0; band < 3; ++band) state.bandGain[static_cast<size_t>(band)] = dsp::dbToGain(p[static_cast<size_t>(band + 4)]);
            break;
        }
        case Type::Reverb:
        {
            constexpr std::array<float, 8> seconds{0.0299f, 0.0373f, 0.0419f, 0.0473f, 0.0539f, 0.0613f, 0.0679f, 0.0797f};
            const float scale = 0.35f + p[2] * 1.65f;
            for (size_t line = 0; line < seconds.size(); ++line)
            {
                state.reverbTime[line] = static_cast<float>(seconds[line] * scale * sampleRate);
                state.reverbFeedback[line] = std::pow(10.0f, -3 * seconds[line] * scale / p[3]);
            }
            state.toneCoefficient = coefficient(p[5], sampleRate); state.delayTime[0] = static_cast<float>(p[4] * sampleRate);
            state.serumHall = state.hall != nullptr && selection(p[9], 3) == 1;
            state.serumPlate = state.plate != nullptr && selection(p[9], 3) == 2;
            if (state.serumHall)
            {
                // Measured Serum 2 Hall engine: the whole module output (dry + wet) is scaled by Level Out.
                serum_hall::Parameters hall;
                hall.size = p[10]; hall.decay = p[11]; hall.low = p[12]; hall.high = p[14]; hall.rate = p[13]; hall.depth = p[15];
                hall.preDelaySeconds = p[4];
                state.hall->setParameters(hall);
                state.hall->setMix(p[0], state.outputGain);
            }
            if (state.serumPlate)
            {
                serum_plate::Parameters plate;
                plate.size = p[10]; plate.highPass = p[12]; plate.damping = p[13]; plate.lowPass = p[14]; plate.width = p[15];
                plate.preDelaySeconds = p[4];
                state.plate->setParameters(plate);
                state.plate->setMix(p[0], state.outputGain);
            }
            break;
        }
        case Type::Eq:
            for (int band = 0; band < 2; ++band)
            {
                const auto start = static_cast<size_t>(2 + band * 4);
                const int shape = selection(p[start], 5);
                const auto type = shape == 0 ? BiquadCoeffs::Type::LowShelf : shape == 1 ? BiquadCoeffs::Type::Peak
                                  : shape == 2 ? BiquadCoeffs::Type::HighShelf : shape == 3 ? BiquadCoeffs::Type::HighPass : BiquadCoeffs::Type::LowPass;
                state.eqCoefficients[static_cast<size_t>(band)] = SvfCoeffs::fromBiquad(BiquadCoeffs::make(type, sampleRate, p[start + 1], p[start + 3], p[start + 2]));
            }
            break;
        case Type::Filter:
        {
            state.drive = dsp::dbToGain(p[5]);
            const int mode = selection(p[2], 8);
            if (mode >= 4) // MG Low ladder (measured on the reference FX filter): knee in plugin level units, poles 0.1 % above the display
            {
                for (auto& ladder : state.ladder)
                {
                    ladder.setKnee(dsp::LadderFilter::kEffectKnee);
                    ladder.setup(sampleRate, p[3], p[4], mode - 3, 1.001f);
                }
            }
            else
                for (auto& f : state.svf) f.setup(sampleRate, p[3], 0.5f + p[4] * 15);
            break;
        }
        case Type::FrequencyShift: state.increment = (p[2] + p[3]) / sampleRate; break;
        case Type::Utility:
        {
            const double limit = sampleRate * 0.49;
            state.highPassActive = p[9] > 1.0f;
            if (state.highPassActive) state.highPassCoefficient = SvfCoeffs::fromBiquad(BiquadCoeffs::make(BiquadCoeffs::Type::HighPass, sampleRate, std::min<double>(p[9], limit), 0.70710678, 0));
            state.lowPassActive = p[10] < limit;
            if (state.lowPassActive) state.lowPassCoefficient = SvfCoeffs::fromBiquad(BiquadCoeffs::make(BiquadCoeffs::Type::LowPass, sampleRate, p[10], 0.70710678, 0));
            state.lowMonoCoefficient = SvfCoeffs::fromBiquad(BiquadCoeffs::make(BiquadCoeffs::Type::HighPass, sampleRate, std::min<double>(p[12], limit), 0.70710678, 0));
            state.dcBlock = p[13] >= 0.5f; // measured: the reference has a first-order high-pass near 2.85 Hz in front of the module
            state.dcCoefficient = static_cast<float>(std::exp(-2.0 * dsp::kPi * 2.85 / sampleRate));
            break;
        }
        case Type::Convolution: case Type::Unknown: break;
    }
}
} // namespace

std::optional<E::Type> E::typeFromCode(int code)
{
    if ((code >= 0 && code <= 12) || code == 14) return static_cast<Type>(code);
    return std::nullopt;
}
std::string_view E::typeName(Type type)
{
    switch (type)
    {
        case Type::Distortion: return "Distortion";
        case Type::Flanger: return "Flanger";
        case Type::Phaser: return "Phaser";
        case Type::Chorus: return "Chorus";
        case Type::Delay: return "Delay";
        case Type::Compressor: return "Compressor";
        case Type::Reverb: return "Reverb";
        case Type::Eq: return "EQ";
        case Type::Filter: return "Filter";
        case Type::HyperDimension: return "Hyper / Dimension";
        case Type::FrequencyShift: return "Frequency Shift";
        case Type::Convolution: return "Convolution";
        case Type::Utility: return "Utility";
        case Type::Split3: return "Split 3";
        case Type::Unknown: return "Unknown";
    }
    return "Unknown";
}
std::span<const PD> E::parameterDescriptors(Type type)
{
    switch (type)
    {
        case Type::Distortion: return distortion;
        case Type::Flanger: return flanger;
        case Type::Phaser: return phaser;
        case Type::Chorus: return chorus;
        case Type::Delay: return delay;
        case Type::Compressor: return compressor;
        case Type::Reverb: return reverb;
        case Type::Eq: return eq;
        case Type::Filter: return filter;
        case Type::HyperDimension: return hyper;
        case Type::FrequencyShift: return shift;
        case Type::Convolution: return convolution;
        case Type::Utility: return utility;
        case Type::Split3: return split;
        case Type::Unknown: return {};
    }
    return {};
}
int E::parameterIndex(Type type, std::string_view id)
{
    const auto descriptors = parameterDescriptors(type);
    for (size_t i = 0; i < descriptors.size(); ++i) if (descriptors[i].id == id) return static_cast<int>(i);
    return -1;
}

ConvolutionIRResult ConvolutionIR::fromSamples(std::span<const float> interleaved, int channels, double sampleRate, std::string name)
{
    if (channels < 1 || channels > 2 || interleaved.empty() || interleaved.size() % static_cast<size_t>(channels) != 0
        || interleaved.size() / static_cast<size_t>(channels) > maxFrames)
        return {nullptr, "Impulse response requires 1-2 aligned channels and at most 1048576 frames"};
    if (! std::isfinite(sampleRate) || sampleRate < 1 || sampleRate > 768000) return {nullptr, "Invalid impulse-response sample rate"};
    for (float sample : interleaved) if (! std::isfinite(sample) || std::abs(sample) > 32) return {nullptr, "Invalid impulse-response sample value"};
    try
    {
        auto result = std::shared_ptr<ConvolutionIR>(new ConvolutionIR);
        result->channels_ = channels; result->sampleRate_ = sampleRate;
        result->samples_.assign(interleaved.begin(), interleaved.end());
        result->name_ = name.empty() ? "Embedded Impulse" : name.substr(0, 1024);
        return {std::move(result), {}};
    }
    catch (const std::bad_alloc&) { return {nullptr, "Unable to allocate impulse response"}; }
    catch (const std::length_error&) { return {nullptr, "Impulse-response allocation length exceeded"}; }
}
ConvolutionIRResult ConvolutionIR::fromFile(const juce::File& file)
{
    if (! file.existsAsFile()) return {nullptr, "Impulse-response file does not exist"};
    try
    {
        juce::AudioFormatManager formats; formats.registerBasicFormats();
        const std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if (! reader || reader->numChannels < 1 || reader->numChannels > 2 || reader->lengthInSamples <= 0 || reader->lengthInSamples > maxFrames)
            return {nullptr, "Unsupported, malformed or oversized impulse-response file"};
        const int channels = static_cast<int>(reader->numChannels), frames = static_cast<int>(reader->lengthInSamples);
        juce::AudioBuffer<float> buffer(channels, frames);
        if (! reader->read(&buffer, 0, frames, 0, true, channels == 2)) return {nullptr, "Unable to read impulse response"};
        std::vector<float> pcm(static_cast<size_t>(frames) * static_cast<size_t>(channels));
        for (int frame = 0; frame < frames; ++frame) for (int channel = 0; channel < channels; ++channel)
            pcm[static_cast<size_t>(frame) * static_cast<size_t>(channels) + static_cast<size_t>(channel)] = buffer.getSample(channel, frame);
        return fromSamples(pcm, channels, reader->sampleRate, file.getFileNameWithoutExtension().toStdString());
    }
    catch (const std::bad_alloc&) { return {nullptr, "Unable to allocate impulse-response file buffer"}; }
    catch (const std::length_error&) { return {nullptr, "Impulse-response file allocation length exceeded"}; }
}

struct E::Impl
{
    Configuration configuration;
    std::vector<std::unique_ptr<ModuleState>> modules;
    std::vector<ModuleId> ids;
    std::array<std::array<int, maxBusModules>, busCount> bus{};
    std::array<int, busCount> busModules{};
    std::array<std::array<PreparedDelay, 2>, busCount> busAlignment;
    std::array<PreparedDelay, 2> mainInputAlignment;
    std::array<int, busCount> busLatency{}, busDelay{};
    std::array<bool, 2> returnBeforeMain{false, false};
    bool alignment = true;
    MixLaw mixLaw = MixLaw::EqualPower;
    std::array<std::atomic<float>, 6> route{0, 0, 1, 1, 1, 1};
    std::atomic<double> tempo{120};
    std::array<float, 6> routeBlock{0, 0, 1, 1, 1, 1};
    double sampleRate = 44100, tempoBlock = 120;
    size_t bytes = 0;
    uint64_t clock = 0;
    int latency = 0, preReturnLatency = 0;
    ModuleState* find(ModuleId id) const
    {
        for (const auto& module : modules) if (module->id == id) return module.get();
        return nullptr;
    }
    bool budget(size_t amount, std::string& error)
    {
        if (amount > maxPreparedBytes - bytes) { error = "Effect graph exceeds the 256 MiB prepared-storage limit"; return false; }
        bytes += amount; return true;
    }
    bool prepareLine(PreparedDelay& line, int length, std::string& error)
    {
        size_t capacity = 1;
        while (capacity < static_cast<size_t>(length) + 4) capacity *= 2;
        if (! budget(capacity * sizeof(float), error)) return false;
        line.prepare(length); return true;
    }
    bool prepareDelay(ModuleState& state, int index, double seconds, std::string& error)
    {
        return prepareLine(state.delay[static_cast<size_t>(index)], static_cast<int>(std::ceil(seconds * sampleRate)), error);
    }
    template <int blockSize>
    bool prepareConvolutionStage(ConvolutionStage<blockSize>& stage, const ConvolutionIR& ir,
                                 double ratio, int offsetFrames, int frames, std::string& error)
    {
        constexpr int transformSize = blockSize * 2, binCount = blockSize + 1;
        const int parts = (frames + blockSize - 1) / blockSize;
        if (! budget(static_cast<size_t>((ir.channels() + 2) * parts * binCount) * sizeof(Complex), error)) return false;
        stage.partitions = parts; stage.channels = ir.channels();
        stage.kernel.resize(static_cast<size_t>(stage.channels * parts * binCount));
        stage.history.resize(static_cast<size_t>(2 * parts * binCount));
        for (int channel = 0; channel < stage.channels; ++channel) for (int part = 0; part < parts; ++part)
        {
            std::array<Complex, transformSize> data{};
            for (int i = 0; i < blockSize; ++i)
            {
                const int frame = part * blockSize + i;
                if (frame >= frames) continue;
                const double position = (offsetFrames + frame) * ratio;
                if (position >= ir.frames()) continue;
                const auto first = static_cast<size_t>(position), second = std::min(first + 1, static_cast<size_t>(ir.frames() - 1));
                const auto channels = static_cast<size_t>(ir.channels()), ch = static_cast<size_t>(channel);
                const float a = ir.originalSamples()[first * channels + ch], b = ir.originalSamples()[second * channels + ch];
                data[static_cast<size_t>(i)] = static_cast<float>((a + (b - a) * (position - static_cast<double>(first))) * ratio);
            }
            stage.fft.transform(data, false);
            const auto offset = static_cast<size_t>((channel * parts + part) * binCount);
            for (int bin = 0; bin < binCount; ++bin) stage.kernel[offset + static_cast<size_t>(bin)] = data[static_cast<size_t>(bin)];
        }
        return true;
    }
    bool prepareConvolution(ModuleState& state, const ConvolutionIR& ir, std::string& error)
    {
        const double ratio = ir.sampleRate() / sampleRate;
        const double targetFrames = std::ceil(ir.frames() / ratio);
        if (! std::isfinite(targetFrames) || targetFrames < 1 || targetFrames > maxResampledConvolutionFrames)
        { error = "Resampled impulse response exceeds the supported frame count"; return false; }
        const int frames = static_cast<int>(targetFrames);
        if (! budget(sizeof(ConvolutionState), error)) return false;
        state.convolution = std::make_unique<ConvolutionState>();
        auto& conv = *state.convolution;
        if (! prepareConvolutionStage(conv.early, ir, ratio, 0, std::min(frames, convolutionHeadFrames), error)) return false;
        if (frames > convolutionHeadFrames)
        {
            using Tail = ConvolutionStage<convolutionTailPartition>;
            if (! budget(sizeof(Tail), error)) return false;
            conv.tail = std::make_unique<Tail>();
            if (! prepareConvolutionStage(*conv.tail, ir, ratio, convolutionHeadFrames, frames - convolutionHeadFrames, error)) return false;
            for (auto& line : conv.tailAlignment) if (! prepareLine(line, ConvolutionState::tailDelay, error)) return false;
        }
        return true;
    }
    int add(const Module& module, int depth, std::set<ModuleId>& unique, std::string& error)
    {
        if (module.id == 0 || ! unique.insert(module.id).second) { error = "Effect module identities must be nonzero and unique"; return -1; }
        const auto descriptors = parameterDescriptors(module.type);
        if (descriptors.empty()) { error = "Unknown effect type (code 13 is not interpreted)"; return -1; }
        if (modules.size() >= maxModules || depth > maxDepth) { error = "Effect graph module count/depth limit exceeded"; return -1; }
        if (module.values.size() > descriptors.size()) { error = "Effect module has excess plain parameters"; return -1; }
        if (module.type != Type::Convolution && module.impulse) { error = "An impulse response is only valid for Convolution"; return -1; }
        if (! budget(sizeof(ModuleState), error)) return -1;
        auto state = std::make_unique<ModuleState>();
        state->id = module.id; state->type = module.type; state->enabled.store(module.enabled);
        state->shapedMix = mixLaw == MixLaw::Shaped;
        state->outputGainParameter = parameterIndex(module.type, "OutputGain");
        for (size_t i = 0; i < descriptors.size(); ++i)
        {
            const float value = i < module.values.size() ? module.values[i] : descriptors[i].initial;
            if (! std::isfinite(value)) { error = "Effect module has a nonfinite plain parameter"; return -1; }
            const float bounded = std::clamp(value, descriptors[i].minimum, descriptors[i].maximum);
            state->parameter[i].store(bounded); state->p[i] = bounded;
        }
        int delayLines = 0; double delaySeconds = 0;
        switch (module.type)
        {
            case Type::Flanger: delayLines = 2; delaySeconds = 0.05; break;
            case Type::Chorus: delayLines = 2; delaySeconds = 0.08; break;
            case Type::HyperDimension: delayLines = 6; delaySeconds = module.values.size() > 9 && module.values[9] >= 0.5f ? 0.35 : 0.1; break;
            case Type::Delay: delayLines = 2; delaySeconds = 6.2; break;
            case Type::Reverb: delayLines = 8; delaySeconds = 0.17; break;
            case Type::Distortion: case Type::Phaser: case Type::Compressor: case Type::Eq: case Type::Filter:
            case Type::FrequencyShift: case Type::Convolution: case Type::Utility: case Type::Unknown: case Type::Split3: break;
        }
        for (int line = 0; line < delayLines; ++line) if (! prepareDelay(*state, line, delaySeconds, error)) return -1;
        if (module.type == Type::Reverb)
        {
            for (int line = 8; line < 12; ++line) if (! prepareDelay(*state, line, line < 10 ? 0.5 : 0.012, error)) return -1;
            // Reference engine buffers: Hall 10 lines of about 1.05 s each, Plate two pre-delay lines and about 0.7 s of combs / all-passes.
            if (! budget(static_cast<size_t>(13.0 * 1.06 * sampleRate) * sizeof(float) + sizeof(serum_hall::Hall) + sizeof(serum_plate::Plate), error)) return -1;
            state->hall = std::make_unique<serum_hall::Hall>();
            state->hall->prepare(sampleRate);
            state->plate = std::make_unique<serum_plate::Plate>();
            state->plate->prepare(sampleRate);
        }
        if (module.type == Type::FrequencyShift)
            for (int i = 0; i < 63; ++i)
            {
                const int centered = i - 31;
                const double window = 0.5 - 0.5 * std::cos(tau * i / 62);
                state->hilbert[static_cast<size_t>(i)] = centered % 2 != 0 ? static_cast<float>(2 * window / (dsp::kPi * centered)) : 0;
            }
        if (module.type == Type::Convolution && module.impulse && ! prepareConvolution(*state, *module.impulse, error)) return -1;
        const int index = static_cast<int>(modules.size());
        modules.push_back(std::move(state)); ids.push_back(module.id);
        for (int band = 0; band < 3; ++band)
        {
            const auto b = static_cast<size_t>(band);
            if ((! module.bands[b].empty() && module.type != Type::Split3) || module.bands[b].size() > maxBandModules)
            { error = "Nested band effects require Split3 and at most eight modules per band"; return -1; }
            for (const auto& child : module.bands[b])
            {
                const int childIndex = add(child, depth + 1, unique, error);
                if (childIndex < 0) return -1;
                auto& parent = *modules[static_cast<size_t>(index)];
                parent.child[b][static_cast<size_t>(parent.childCount[b]++)] = childIndex;
            }
        }
        derive(*modules[static_cast<size_t>(index)], sampleRate, tempoBlock);
        return index;
    }
    int moduleLatency(int index) const
    {
        const auto& state = *modules[static_cast<size_t>(index)];
        int own = state.type == Type::FrequencyShift ? 31 : state.convolution ? partition : (state.type == Type::Distortion && state.p[9] >= 0.5f) ? 7 : 0;
        int nested = 0;
        for (int band = 0; band < 3; ++band)
        {
            int total = 0;
            for (int i = 0; i < state.childCount[static_cast<size_t>(band)]; ++i) total += moduleLatency(state.child[static_cast<size_t>(band)][static_cast<size_t>(i)]);
            nested = std::max(nested, total);
        }
        return own + nested;
    }
    bool prepareAlignment(std::string& error)
    {
        for (size_t index = 0; index < modules.size(); ++index)
        {
            auto& module = *modules[index];
            module.latency = moduleLatency(static_cast<int>(index));
            std::array<int, 3> chainLatency{};
            int maximum = 0;
            for (size_t band = 0; band < chainLatency.size(); ++band)
            {
                for (int i = 0; i < module.childCount[band]; ++i)
                    chainLatency[band] += moduleLatency(module.child[band][static_cast<size_t>(i)]);
                maximum = std::max(maximum, chainLatency[band]);
            }
            for (size_t band = 0; band < chainLatency.size(); ++band)
            {
                module.bandLatency[band] = maximum - chainLatency[band];
                if (alignment && module.type == Type::Split3 && module.bandLatency[band] > 0)
                    for (auto& line : module.bandAlignment[band]) if (! prepareLine(line, module.bandLatency[band], error)) return false;
            }
            if (alignment && module.latency > 0)
                for (auto& line : module.dryAlignment) if (! prepareLine(line, module.latency, error)) return false;
        }
        for (size_t b = 0; b < busDelay.size(); ++b)
        {
            const int mergeLatency = b > 0 && returnBeforeMain[b - 1] ? preReturnLatency : latency;
            busDelay[b] = mergeLatency - busLatency[b] - (b == 0 ? preReturnLatency : 0);
            if (alignment && busDelay[b] > 0)
                for (auto& line : busAlignment[b]) if (! prepareLine(line, busDelay[b], error)) return false;
        }
        if (alignment && preReturnLatency > 0)
            for (auto& line : mainInputAlignment) if (! prepareLine(line, preReturnLatency, error)) return false;
        return true;
    }
    void snapshot()
    {
        tempoBlock = tempo.load(std::memory_order_relaxed);
        for (size_t i = 0; i < routeBlock.size(); ++i) routeBlock[i] = route[i].load(std::memory_order_relaxed);
        for (auto& state : modules)
        {
            state->active = state->enabled.load(std::memory_order_relaxed);
            const size_t count = parameterDescriptors(state->type).size();
            for (size_t i = 0; i < count; ++i) state->p[i] = state->parameter[i].load(std::memory_order_relaxed);
        }
    }
    Stereo processModule(int index, Stereo source);
};

Stereo E::Impl::processModule(int index, Stereo source)
{
    auto& s = *modules[static_cast<size_t>(index)];
    const Stereo drySource = alignment ? alignSignal(s.dryAlignment, s.latency, source) : source;
    if (! s.active) return drySource;
    const auto& p = s.p;
    Stereo result = source;
    switch (s.type)
    {
        case Type::Distortion:
        {
            if (s.serumDistortion)
            {
                if (s.serumMode == 8)
                {
                    // Downsample: sample and hold at the base rate. The held value is renewed after int(Drive^2 * round(0.002 fs) + 0.5) held samples;
                    // the wet path is delayed by 7 samples like the dry one (an 8-slot ring), then the optional post filter follows.
                    const int steps = static_cast<int>(s.serumDrive * s.serumDrive * static_cast<float>(s.holdScale) + 0.5f);
                    const std::array<float, 2> input{s.serumFilter == 1 ? s.morph[0].process(source.left) : source.left,
                                                     s.serumFilter == 1 ? s.morph[1].process(source.right) : source.right};
                    if (s.holdCounter < steps) ++s.holdCounter;
                    else { s.holdCounter = 0; s.serumHeld = input; }
                    const auto held = [&](size_t ch) {
                        float& slot = s.holdPipe[ch][static_cast<size_t>(s.holdCursor)];
                        slot = std::clamp(s.serumHeld[ch], -64.0f, 64.0f);
                        float y = s.holdPipe[ch][static_cast<size_t>((s.holdCursor + 1) & 7)];
                        if (s.serumFilter == 2) y = 0.25f * s.morph[ch].process(4.0f * y); // the post filter runs before the wet gain's 0.25
                        return y;
                    };
                    result = {held(0), held(1)};
                    s.holdCursor = (s.holdCursor + 1) & 7;
                    break;
                }
                if (s.serumMode == 16)
                {
                    // Overdrive: base rate, no oversampling, no wet delay; the optional filters wrap it and the wet gain's 0.25 follows the post filter.
                    float left = s.serumFilter == 1 ? s.morph[0].process(source.left) : source.left;
                    float right = s.serumFilter == 1 ? s.morph[1].process(source.right) : source.right;
                    s.overdrive.process(left, right, s.serumDrive, s.overdriveStages);
                    if (s.serumFilter == 2) { left = s.morph[0].process(left); right = s.morph[1].process(right); }
                    result = {0.25f * left, 0.25f * right};
                    break;
                }
                const auto shaped = [&](int channel, float input) {
                    const auto ch = static_cast<size_t>(channel);
                    float x = input;
                    if (s.serumFilter == 1) x = s.morph[ch].process(x);
                    float y = s.oversampler[ch].process(x, [&](float v) { return 0.25f * serum_distortion::shaper(s.serumMode, s.serumDrive, v); });
                    if (s.serumFilter == 2) y = 0.25f * s.morph[ch].process(4.0f * y); // the post filter runs before the wet gain's 0.25
                    return y;
                };
                result = {shaped(0, source.left), shaped(1, source.right)};
                break;
            }
            const int shape = selection(p[3], 6);
            if (shape == 5 && s.holdCountdown-- <= 0)
            {
                const float steps = std::exp2(std::round(p[6]) - 1);
                s.held = {std::round(source.left * s.drive * steps) / steps, std::round(source.right * s.drive * steps) / steps};
                s.holdCountdown = std::max(1, static_cast<int>(std::lround(p[7]))) - 1;
            }
            for (int channel = 0; channel < 2; ++channel)
            {
                const auto ch = static_cast<size_t>(channel);
                float value = (channel == 0 ? source.left : source.right) * s.drive + p[5];
                value = shape == 5 ? s.held[ch] : distortionTransfer(value, shape) - s.distortionBaseline;
                s.tone[ch] += (value - s.tone[ch]) * s.toneCoefficient;
                if (channel == 0) result.left = s.tone[ch]; else result.right = s.tone[ch];
            }
            break;
        }
        case Type::Flanger: case Type::Chorus: case Type::HyperDimension:
        {
            if (s.serumHyper)
            {
                // Hyper (measured): per-channel high-pass (2-pass cubic state-variable filter, 120 Hz) feeds a delay line read by N sawtooth-driven
                // taps at D * |phase - 0.5| samples; Dimension: four decorrelating taps of the mono sum, crossfaded by a slow sine/cosine.
                for (size_t channel = 0; channel < 2; ++channel)
                {
                    auto& phases = s.hyperPhase[channel];
                    for (int voice = 0; voice < s.hyperVoices; ++voice)
                    {
                        const auto j = static_cast<size_t>(voice);
                        phases[j] += s.hyperIncrement[channel][j];
                        if (phases[j] >= 1.0) phases[j] -= 1.0;
                    }
                }
                std::array<float, 2> wet{};
                for (size_t channel = 0; channel < 2; ++channel)
                {
                    const float input = channel == 0 ? source.left : source.right;
                    const float g = s.hyperFilterG, k = s.hyperFilterK;
                    float low = s.hyperLow[channel], band = s.hyperBand[channel];
                    low += g * band;
                    band = band - 0.15f * band * band * band + g * (input - (low + band * k));
                    low += g * band;
                    const float highpassed = input - (low + k * band);
                    band = band - 0.15f * band * band * band + g * highpassed;
                    s.hyperLow[channel] = finiteSample(low); s.hyperBand[channel] = finiteSample(band);
                    float sum = 0;
                    for (int voice = 0; voice < s.hyperVoices; ++voice)
                        sum += s.delay[channel].read(static_cast<float>(s.hyperDepth * std::abs(s.hyperPhase[channel][static_cast<size_t>(voice)] - 0.5)));
                    s.delay[channel].write(finiteSample(highpassed));
                    wet[channel] = sum;
                }
                result = {s.hyperWet * wet[0] + s.hyperDry * source.left, s.hyperWet * wet[1] + s.hyperDry * source.right};
                if (s.dimeActive)
                {
                    s.dimePhase += s.dimeIncrement; s.dimePhase -= std::floor(s.dimePhase);
                    const float mono = result.left + result.right;
                    std::array<float, 4> tap{};
                    for (size_t line = 0; line < 4; ++line) tap[line] = s.delay[line + 2].read(static_cast<float>(s.dimeDelay[line]));
                    for (size_t line = 0; line < 4; ++line) s.delay[line + 2].write(finiteSample(mono));
                    const float angle = 6.2831855f * s.dimePhase;
                    const float v = ((tap[0] - tap[1]) * std::sin(angle) + (tap[2] - tap[3]) * std::cos(angle)) * s.dimeWet;
                    result = {(v + result.left) * s.dimeGain, (result.right - v) * s.dimeGain};
                }
                break;
            }
            const bool flange = s.type == Type::Flanger, dimension = s.type == Type::HyperDimension;
            const int voices = flange ? 1 : selection(p[dimension ? 4 : 6] - 1, dimension ? 7 : 6) + 1;
            const float feedback = flange || ! dimension ? p[5] : 0;
            std::array<float, 2> channelOut{};
            for (int channel = 0; channel < 2; ++channel)
            {
                const auto ch = static_cast<size_t>(channel);
                float sum = 0;
                for (int voice = 0; voice < voices; ++voice)
                {
                    const double phase = s.phase + static_cast<double>(voice) / voices + channel * (flange ? p[6] : 0.25);
                    const float base = dimension ? 8 : p[4], depth = dimension ? 0.1f + p[3] * 6 : p[3];
                    const float milliseconds = std::max(0.05f, base + depth * sineAt(phase));
                    sum += s.delay[ch].read(static_cast<float>(milliseconds * 0.001 * sampleRate) - 1);
                }
                sum /= static_cast<float>(voices);
                if (dimension)
                {
                    float decorrelated = 0;
                    for (int tap = 0; tap < 4; ++tap)
                        decorrelated += s.delay[ch].read(static_cast<float>((0.006 + tap * 0.004 + channel * 0.0013) * (0.5 + p[5]) * sampleRate) - 1) * (tap % 2 == 0 ? 0.25f : -0.25f);
                    sum += (decorrelated - sum) * p[6];
                }
                channelOut[ch] = sum;
                s.delay[ch].write(finiteSample((channel == 0 ? source.left : source.right) + sum * feedback));
            }
            result = flange ? Stereo{(source.left + channelOut[0]) * 0.5f, (source.right + channelOut[1]) * 0.5f}
                            : Stereo{channelOut[0], channelOut[1]};
            if (! flange)
            {
                const float width = p[7], mid = (result.left + result.right) * 0.5f, side = (result.left - result.right) * 0.5f * width;
                result = {mid + side, mid - side};
            }
            s.phase = wrap(s.phase + s.increment); break;
        }
        case Type::Phaser:
        {
            if (s.serumPhaser)
            {
                // Measured: 2*poles first-order all-passes (pairs share one coefficient), coefficient modulated by a triangle LFO,
                // output = (1 - q^2) dry + q all-pass with q = sin^2(pi w / 4).
                s.phaserPhase += s.phaserPhaseIncrement; s.phaserPhase -= std::floor(s.phaserPhase); // the reference advances before use (double precision)
                const float triangleLeft = static_cast<float>(std::min(s.phaserPhase, 1.0 - s.phaserPhase));
                double shifted = s.phaserPhase + s.phaserWidth; shifted -= std::floor(shifted);
                const float triangleRight = static_cast<float>(std::min(shifted, 1.0 - shifted));
                const float dryGain = 1.0f - s.phaserQ * s.phaserQ;
                for (size_t channel = 0; channel < 2; ++channel)
                {
                    const float input = channel == 0 ? source.left : source.right;
                    float value = finiteSample(input + s.phaserPrevious[channel] * s.phaserFeedback);
                    const float triangle = channel == 0 ? triangleLeft : triangleRight;
                    for (int k = 0; k < s.phaserPoles; ++k)
                    {
                        const auto at = static_cast<size_t>(k);
                        const float c = s.phaserCoefficient[at] * (triangle * s.phaserModulation + 1.0f);
                        const float a = (1.0f - c) / (c + 1.0f);
                        auto& state = s.phaserState[channel][at];
                        const float t = state[0] - value * a, u = t * a;
                        state[0] = value + u;
                        const float second = state[1] - u;
                        state[1] = t + a * second;
                        value = finiteSample(second);
                    }
                    s.phaserPrevious[channel] = value;
                    const float mixed = s.phaserQ * value + dryGain * input;
                    if (channel == 0) result.left = mixed; else result.right = mixed;
                }
                break;
            }
            const int stages = selection(p[6] - 2, 11) + 2;
            for (int channel = 0; channel < 2; ++channel)
            {
                const auto ch = static_cast<size_t>(channel);
                const float input = channel == 0 ? source.left : source.right;
                float value = finiteSample(input + s.previous[ch] * p[5]);
                for (int stage = 0; stage < stages; ++stage)
                {
                    const auto at = static_cast<size_t>(stage);
                    const float next = -s.allpassCoefficient[ch] * value + s.allpassX[ch][at] + s.allpassCoefficient[ch] * s.allpassY[ch][at];
                    s.allpassX[ch][at] = value; s.allpassY[ch][at] = finiteSample(next); value = s.allpassY[ch][at];
                }
                s.previous[ch] = value;
                if (channel == 0) result.left = (input + value) * 0.5f; else result.right = (input + value) * 0.5f;
            }
            s.phase = wrap(s.phase + s.increment); break;
        }
        case Type::Delay:
        {
            if (s.serumDelay)
            {
                const auto bandpass = [&](size_t slot, float value) { return s.delayFilterState[slot].process(s.delayFilterCoefficient, value); };
                // The feedback signal is delayed by one extra sample before it re-enters a delay line (measured loop = delay + 1).
                const auto feed = [&](size_t slot, float value) {
                    const float old = s.feedbackPipe[slot];
                    s.feedbackPipe[slot] = value;
                    return old;
                };
                if (s.delayMode == 0)
                {
                    const float left = bandpass(0, s.delay[0].read(s.delayTime[0] - 1)), right = bandpass(1, s.delay[1].read(s.delayTime[1] - 1));
                    s.delay[0].write(finiteSample(source.left + feed(0, left * s.delayFeedback[0])));
                    s.delay[1].write(finiteSample(source.right + feed(1, right * s.delayFeedback[1])));
                    result = {left, right};
                }
                else if (s.delayMode == 1) // ping-pong: mono input, the unfiltered left delay output feeds the right delay, the right tap feeds back to the left
                {
                    const float rawLeft = s.delay[0].read(s.delayTime[0] - 1), rawRight = s.delay[1].read(s.delayTime[1] - 1);
                    const float left = bandpass(0, rawLeft), right = bandpass(1, rawRight);
                    s.delay[0].write(finiteSample((source.left + source.right) * 0.5f + feed(0, right * s.delayFeedback[0])));
                    s.delay[1].write(finiteSample(rawLeft));
                    result = {left, right};
                }
                else // mono: input sum goes through both delays in series (one filter), feedback closes around the second delay
                {
                    const float rawLeft = s.delay[0].read(s.delayTime[0] - 1), second = bandpass(1, s.delay[1].read(s.delayTime[1] - 1));
                    s.delay[0].write(finiteSample(source.left + source.right));
                    s.delay[1].write(finiteSample(rawLeft + feed(1, second * s.delayFeedback[1])));
                    result = {second, second};
                }
                break;
            }
            std::array<float, 2> tap{};
            for (size_t channel = 0; channel < 2; ++channel)
            {
                const float value = s.delay[channel].read(s.delayTime[channel] - 1);
                s.tone[channel] += (value - s.tone[channel]) * s.toneCoefficient;
                s.low[channel] += (s.tone[channel] - s.low[channel]) * s.lowCoefficient;
                tap[channel] = s.tone[channel] - s.low[channel];
            }
            s.delay[0].write(finiteSample(source.left + tap[p[7] >= 0.5f ? 1 : 0] * p[4]));
            s.delay[1].write(finiteSample(source.right + tap[p[7] >= 0.5f ? 0 : 1] * p[4]));
            result = {tap[0], tap[1]}; break;
        }
        case Type::Compressor:
            if (s.instantPeak) { result = compressInstantPeak(s, source); break; }
            if (p[8] < 0.5f) result = compress(s, source, 0);
            else
            {
                const auto bandsIn = splitBands(s, source); result = {};
                for (int band = 0; band < 3; ++band)
                {
                    const auto bandOut = compress(s, bandsIn[static_cast<size_t>(band)], band);
                    result.left += bandOut.left; result.right += bandOut.right;
                }
            }
            break;
        case Type::Reverb:
        {
            if (s.serumHall || s.serumPlate)
            {
                float left = 0, right = 0;
                if (s.serumHall) s.hall->process(source.left, source.right, left, right);
                else s.plate->process(source.left, source.right, left, right);
                return {finiteSample(left), finiteSample(right)};
            }
            std::array<float, 2> input{source.left, source.right};
            for (size_t channel = 0; channel < 2; ++channel)
            {
                if (s.delayTime[0] >= 1) { input[channel] = s.delay[channel + 8].read(s.delayTime[0] - 1); }
                s.delay[channel + 8].write(channel == 0 ? source.left : source.right);
                const float delayed = s.delay[channel + 10].read(static_cast<float>((channel == 0 ? 0.0037 : 0.0053) * sampleRate) - 1);
                const float allpass = delayed - input[channel] * p[6] * 0.75f;
                s.delay[channel + 10].write(finiteSample(input[channel] + allpass * p[6] * 0.75f));
                input[channel] = allpass;
            }
            std::array<float, 8> tap{};
            float sum = 0;
            for (size_t line = 0; line < 8; ++line)
            {
                const float value = s.delay[line].read(s.reverbTime[line] - 1);
                s.damping[line] += (value - s.damping[line]) * s.toneCoefficient;
                tap[line] = s.damping[line]; sum += tap[line];
            }
            result = {};
            for (size_t line = 0; line < 8; ++line)
            {
                const float feedback = (tap[line] - sum * 0.25f) * s.reverbFeedback[line];
                s.delay[line].write(finiteSample(feedback + (line % 2 == 0 ? input[0] : input[1]) * 0.25f));
                result.left += tap[line] * (line % 3 == 0 ? -0.25f : 0.25f);
                result.right += tap[line] * (line % 3 == 1 ? -0.25f : 0.25f);
            }
            const float mid = (result.left + result.right) * 0.5f, side = (result.left - result.right) * 0.5f * p[7];
            result = {mid + side, mid - side}; break;
        }
        case Type::Eq:
            for (int band = 0; band < 2; ++band)
            {
                result.left = s.eqState[0][static_cast<size_t>(band)].process(s.eqCoefficients[static_cast<size_t>(band)], result.left);
                result.right = s.eqState[1][static_cast<size_t>(band)].process(s.eqCoefficients[static_cast<size_t>(band)], result.right);
            }
            break;
        case Type::Filter:
        {
            if (selection(p[2], 8) >= 4)
            {
                result = {s.ladder[0].process(source.left * s.drive), s.ladder[1].process(source.right * s.drive)};
                break;
            }
            const auto mode = static_cast<dsp::Svf::Mode>(selection(p[2], 4));
            result = {s.svf[0].process(std::tanh(source.left * s.drive), mode), s.svf[1].process(std::tanh(source.right * s.drive), mode)};
            if (p[6] >= 0.5f) result = {s.svf[2].process(result.left, mode), s.svf[3].process(result.right, mode)};
            break;
        }
        case Type::FrequencyShift:
        {
            const float cosine = sineAt(s.phase + 0.25), sine = sineAt(s.phase);
            for (int channel = 0; channel < 2; ++channel)
            {
                const auto ch = static_cast<size_t>(channel), head = static_cast<size_t>(s.hilbertCursor);
                s.hilbertInput[ch][head] = finiteSample((channel == 0 ? source.left : source.right) + s.previous[ch] * p[4]);
                float imaginary = 0;
                for (int tap = 0; tap < 63; ++tap) imaginary += s.hilbert[static_cast<size_t>(tap)] * s.hilbertInput[ch][static_cast<size_t>((s.hilbertCursor - tap) & 63)];
                const float real = s.hilbertInput[ch][static_cast<size_t>((s.hilbertCursor - 31) & 63)];
                const float shifted = real * cosine - imaginary * sine;
                s.previous[ch] = finiteSample(shifted);
                if (channel == 0) result.left = shifted; else result.right = shifted;
            }
            const float mid = (result.left + result.right) * 0.5f, side = (result.left - result.right) * 0.5f * p[5];
            result = {mid + side, mid - side};
            s.hilbertCursor = (s.hilbertCursor + 1) & 63; s.phase = wrap(s.phase + s.increment); break;
        }
        case Type::Convolution: if (s.convolution) result = s.convolution->next(source); break;
        case Type::Utility:
        {
            if (s.dcBlock)
            {
                const float left = s.dcCoefficient * (s.dcOutput[0] + result.left - s.dcInput[0]);
                const float right = s.dcCoefficient * (s.dcOutput[1] + result.right - s.dcInput[1]);
                s.dcInput = {result.left, result.right}; s.dcOutput = {left, right};
                result = {left, right};
            }
            if (s.highPassActive || s.lowPassActive)
            {
                // Measured: HPF is a Linkwitz-Riley 4th-order high-pass (two equal Butterworth sections), LPF a Butterworth 2nd-order low-pass.
                for (size_t channel = 0; channel < 2; ++channel)
                {
                    float v = channel == 0 ? result.left : result.right;
                    if (s.highPassActive) { v = s.highPassState[channel][0].process(s.highPassCoefficient, v); v = s.highPassState[channel][1].process(s.highPassCoefficient, v); }
                    if (s.lowPassActive) v = s.lowPassState[channel].process(s.lowPassCoefficient, v);
                    (channel == 0 ? result.left : result.right) = v;
                }
            }
            if (p[5] >= 0.5f) std::swap(result.left, result.right);
            // Measured order: polarity and balance act on the channels first, then the width matrix (mid/side) follows.
            result.left *= balanceLeft(p[3]) * (p[6] >= 0.5f ? -1 : 1);
            result.right *= balanceRight(p[3]) * (p[7] >= 0.5f ? -1 : 1);
            const float mid = (result.left + result.right) * 0.5f;
            float side = p[4] >= 0.5f ? 0 : (result.left - result.right) * 0.5f * p[2];
            if (p[11] >= 0.5f) // low-frequency mono: the side signal loses everything below the crossover (Linkwitz-Riley 4th order)
                side = s.lowMonoState[1].process(s.lowMonoCoefficient, s.lowMonoState[0].process(s.lowMonoCoefficient, side));
            result = {mid + side, mid - side};
            break;
        }
        case Type::Split3:
        {
            auto bandsOut = splitBands(s, source); result = {};
            for (int band = 0; band < 3; ++band)
            {
                const auto b = static_cast<size_t>(band);
                for (int child = 0; child < s.childCount[b]; ++child) bandsOut[b] = processModule(s.child[b][static_cast<size_t>(child)], bandsOut[b]);
                if (alignment) bandsOut[b] = alignSignal(s.bandAlignment[b], s.bandLatency[b], bandsOut[b]);
                result.left += bandsOut[b].left * s.bandGain[b]; result.right += bandsOut[b].right * s.bandGain[b];
            }
            break;
        }
        case Type::Unknown: return source;
    }
    if (s.instantPeak) return {finiteSample(result.left * s.outputGain), finiteSample(result.right * s.outputGain)};
    if (s.serumHyper) return {finiteSample(result.left * s.outputGain), finiteSample(result.right * s.outputGain)};
    if (s.serumPhaser) return {finiteSample(result.left * s.outputGain), finiteSample(result.right * s.outputGain)};
    if (s.serumDelay) // Level Out scales dry and wet together
        return {finiteSample((drySource.left * s.dry + result.left * s.wet) * s.outputGain),
                finiteSample((drySource.right * s.dry + result.right * s.wet) * s.outputGain)};
    if (s.serumDistortion) // the reference scales the whole module output (dry and wet) by Level Out
        return {finiteSample((drySource.left * s.dry + result.left * s.wet) * s.outputGain),
                finiteSample((drySource.right * s.dry + result.right * s.wet) * s.outputGain)};
    if (p[0] <= 0) return drySource;
    return {finiteSample(drySource.left * s.dry + result.left * s.wet * s.outputGain),
            finiteSample(drySource.right * s.dry + result.right * s.wet * s.outputGain)};
}

E::WavetableEffects() : impl_(std::make_unique<Impl>()) {}
E::~WavetableEffects() = default;
bool E::prepare(const Configuration& configuration, double sampleRate, std::string& error)
{
    error.clear();
    if (! std::isfinite(sampleRate) || sampleRate < 8000 || sampleRate > 384000) { error = "Invalid effect-graph sample rate"; return false; }
    const auto validRouting = [](const Routing& routing) {
        const std::array<float, 6> values{routing.mainToBus1, routing.mainToBus2, routing.bus1ToMain, routing.bus2ToMain, routing.mainGain, routing.directGain};
        for (size_t i = 0; i < values.size(); ++i)
            if (! std::isfinite(values[i]) || values[i] < 0 || values[i] > routingMaximum(i)) return false;
        return true;
    };
    if (! validRouting(configuration.routing)) { error = "Invalid effect-bus routing gain"; return false; }
    if (configuration.mixLaw != MixLaw::EqualPower && configuration.mixLaw != MixLaw::Shaped)
    { error = "Invalid effect mix law"; return false; }
    try
    {
        auto next = std::make_unique<Impl>();
        next->sampleRate = sampleRate;
        next->alignment = configuration.alignLatency;
        next->returnBeforeMain = configuration.returnBeforeMain;
        next->mixLaw = configuration.mixLaw;
        next->tempo.store(impl_->tempo.load(std::memory_order_relaxed)); next->tempoBlock = next->tempo.load();
        std::set<ModuleId> unique;
        for (int bus = 0; bus < busCount; ++bus)
        {
            const auto b = static_cast<size_t>(bus);
            if (configuration.buses[b].size() > maxBusModules) { error = "Effect bus has more than 16 modules"; return false; }
            int latency = 0;
            for (const auto& module : configuration.buses[b])
            {
                const int index = next->add(module, 0, unique, error);
                if (index < 0) return false;
                next->bus[b][static_cast<size_t>(next->busModules[b]++)] = index;
                latency += next->moduleLatency(index);
            }
            next->busLatency[b] = latency;
        }
        for (size_t b = 1; b < busCount; ++b)
            if (next->returnBeforeMain[b - 1]) next->preReturnLatency = std::max(next->preReturnLatency, next->busLatency[b]);
        next->latency = next->preReturnLatency + next->busLatency[0];
        for (size_t b = 1; b < busCount; ++b)
            if (! next->returnBeforeMain[b - 1]) next->latency = std::max(next->latency, next->busLatency[b]);
        if (! next->prepareAlignment(error)) return false;
        next->configuration = configuration;
        const auto& r = configuration.routing;
        const std::array<float, 6> routes{r.mainToBus1, r.mainToBus2, r.bus1ToMain, r.bus2ToMain, r.mainGain, r.directGain};
        for (size_t i = 0; i < routes.size(); ++i) next->route[i].store(routes[i]);
        impl_ = std::move(next); reset(); return true;
    }
    catch (const std::bad_alloc&) { error = "Unable to allocate effect-graph storage"; return false; }
    catch (const std::length_error&) { error = "Effect-graph allocation length exceeded"; return false; }
}
void E::reset()
{
    for (auto& module : impl_->modules) module->reset();
    for (auto& bus : impl_->busAlignment) for (auto& line : bus) if (line.capacity() > 0) line.reset();
    for (auto& line : impl_->mainInputAlignment) if (line.capacity() > 0) line.reset();
    impl_->clock = 0; impl_->snapshot();
    for (auto& module : impl_->modules) derive(*module, impl_->sampleRate, impl_->tempoBlock);
}
bool E::setParameter(ModuleId id, int parameter, float plainValue)
{
    auto* module = impl_->find(id);
    if (! module || parameter < 0 || ! std::isfinite(plainValue)) return false;
    const auto descriptors = parameterDescriptors(module->type);
    if (static_cast<size_t>(parameter) >= descriptors.size()) return false;
    const auto& info = descriptors[static_cast<size_t>(parameter)];
    module->parameter[static_cast<size_t>(parameter)].store(std::clamp(plainValue, info.minimum, info.maximum), std::memory_order_relaxed);
    return true;
}
float E::getParameter(ModuleId id, int parameter) const
{
    auto* module = impl_->find(id);
    return module && parameter >= 0 && static_cast<size_t>(parameter) < parameterDescriptors(module->type).size()
        ? module->parameter[static_cast<size_t>(parameter)].load(std::memory_order_relaxed) : 0;
}
bool E::setEnabled(ModuleId id, bool state)
{
    if (auto* module = impl_->find(id)) { module->enabled.store(state, std::memory_order_relaxed); return true; }
    return false;
}
bool E::enabled(ModuleId id) const { const auto* module = impl_->find(id); return module && module->enabled.load(std::memory_order_relaxed); }
std::optional<Type> E::moduleType(ModuleId id) const { if (const auto* module = impl_->find(id)) return module->type; return std::nullopt; }
void E::setTempo(double bpm) { if (std::isfinite(bpm)) impl_->tempo.store(std::clamp(bpm, 1.0, 1000.0), std::memory_order_relaxed); }
bool E::setRouting(const Routing& routing)
{
    const std::array<float, 6> values{routing.mainToBus1, routing.mainToBus2, routing.bus1ToMain, routing.bus2ToMain, routing.mainGain, routing.directGain};
    for (size_t i = 0; i < values.size(); ++i)
        if (! std::isfinite(values[i]) || values[i] < 0 || values[i] > routingMaximum(i)) return false;
    for (size_t i = 0; i < values.size(); ++i) impl_->route[i].store(values[i], std::memory_order_relaxed);
    return true;
}
E::Routing E::routing() const
{
    return {impl_->route[0].load(), impl_->route[1].load(), impl_->route[2].load(), impl_->route[3].load(), impl_->route[4].load(), impl_->route[5].load()};
}
bool E::setRoutingParameter(int parameter, float plainValue)
{
    if (parameter < 0 || parameter >= 6 || ! std::isfinite(plainValue)) return false;
    const auto index = static_cast<size_t>(parameter);
    impl_->route[index].store(std::clamp(plainValue, 0.0f, routingMaximum(index)), std::memory_order_relaxed);
    return true;
}
float E::getRoutingParameter(int parameter) const
{
    return parameter >= 0 && parameter < 6 ? impl_->route[static_cast<size_t>(parameter)].load(std::memory_order_relaxed) : 0;
}
E::Configuration E::snapshotConfiguration() const
{
    Configuration result = impl_->configuration;
    result.routing = routing();
    const std::function<void(Module&)> capture = [&](Module& module) {
        module.enabled = enabled(module.id);
        module.values.resize(parameterDescriptors(module.type).size());
        for (size_t i = 0; i < module.values.size(); ++i) module.values[i] = getParameter(module.id, static_cast<int>(i));
        for (auto& band : module.bands) for (auto& child : band) capture(child);
    };
    for (auto& bus : result.buses) for (auto& module : bus) capture(module);
    return result;
}
std::span<const E::ModuleId> E::moduleIds() const { return impl_->ids; }
int E::latencySamples() const { return impl_->latency; }
size_t E::preparedBytes() const { return impl_->bytes; }
void E::process(const std::array<StereoBuffer, busCount>& buses, int frames)
{
    if (frames <= 0) return;
    for (const auto& bus : buses) if (bus.left == nullptr || bus.right == nullptr) return;
    impl_->snapshot();
    for (int sample = 0; sample < frames; ++sample)
    {
        if (impl_->clock % controlPeriod == 0)
            for (auto& module : impl_->modules) derive(*module, impl_->sampleRate, impl_->tempoBlock);
        const Stereo main{finiteSample(buses[0].left[sample]), finiteSample(buses[0].right[sample])};
        std::array<Stereo, busCount> value{main,
            Stereo{finiteSample(buses[1].left[sample] + main.left * impl_->routeBlock[0]), finiteSample(buses[1].right[sample] + main.right * impl_->routeBlock[0])},
            Stereo{finiteSample(buses[2].left[sample] + main.left * impl_->routeBlock[1]), finiteSample(buses[2].right[sample] + main.right * impl_->routeBlock[1])}};
        for (int bus : {1, 2})
        {
            const auto b = static_cast<size_t>(bus);
            for (int module = 0; module < impl_->busModules[b]; ++module)
                value[b] = impl_->processModule(impl_->bus[b][static_cast<size_t>(module)], value[b]);
            if (impl_->alignment) value[b] = alignSignal(impl_->busAlignment[b], impl_->busDelay[b], value[b]);
        }
        if (impl_->returnBeforeMain[0] || impl_->returnBeforeMain[1])
        {
            if (impl_->alignment) value[0] = alignSignal(impl_->mainInputAlignment, impl_->preReturnLatency, value[0]);
            for (size_t b = 1; b < busCount; ++b)
                if (impl_->returnBeforeMain[b - 1])
                {
                    value[0].left = finiteSample(value[0].left + value[b].left * impl_->routeBlock[b + 1]);
                    value[0].right = finiteSample(value[0].right + value[b].right * impl_->routeBlock[b + 1]);
                }
        }
        for (int module = 0; module < impl_->busModules[0]; ++module)
            value[0] = impl_->processModule(impl_->bus[0][static_cast<size_t>(module)], value[0]);
        if (impl_->alignment) value[0] = alignSignal(impl_->busAlignment[0], impl_->busDelay[0], value[0]);
        const float return1 = impl_->returnBeforeMain[0] ? 0 : impl_->routeBlock[2];
        const float return2 = impl_->returnBeforeMain[1] ? 0 : impl_->routeBlock[3];
        const float directGain = impl_->routeBlock[5];
        if (directGain < 1 || directGain > 1)
        {
            value[0].left = finiteSample((value[0].left + (value[1].left * return1 + value[2].left * return2) * directGain) * impl_->routeBlock[4]);
            value[0].right = finiteSample((value[0].right + (value[1].right * return1 + value[2].right * return2) * directGain) * impl_->routeBlock[4]);
        }
        else
        {
            // Exact unity keeps the historical floating-point evaluation order for legacy graphs.
            value[0].left = finiteSample((value[0].left + value[1].left * return1 + value[2].left * return2) * impl_->routeBlock[4]);
            value[0].right = finiteSample((value[0].right + value[1].right * return1 + value[2].right * return2) * impl_->routeBlock[4]);
        }
        for (size_t bus = 0; bus < value.size(); ++bus) { buses[bus].left[sample] = value[bus].left; buses[bus].right[sample] = value[bus].right; }
        ++impl_->clock;
    }
}

} // namespace sonora::devices

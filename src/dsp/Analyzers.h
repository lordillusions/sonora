#pragma once
// Metering building blocks behind the Monitor view: stereo correlation / balance / goniometer, a log-frequency
// spectrum (bars and a smooth curve) and an oscilloscope trace. JUCE free; run on an analysis thread, never
// on the audio thread (see engine/MasterMonitor).

#include "dsp/Fft.h"

#include <array>
#include <complex>
#include <vector>

namespace sonora::dsp
{

// Phase correlation (-1 out of phase .. +1 mono), balance (-1 left .. +1 right) and stereo width over a
// sliding window of about 300 ms, plus the latest mid/side points for a goniometer.
class StereoMeter
{
public:
    static constexpr int kGoniometerPoints = 512;

    explicit StereoMeter(double sampleRate = 48000.0) { prepare(sampleRate); }
    void prepare(double sampleRate);
    void reset();
    void process(const float* left, const float* right, int numSamples);

    float correlation() const; // 0 while silent
    float balance() const;     // (R - L) / (R + L) on RMS
    float width() const;       // side / (mid + side) energy, 0 mono .. 1 wide / out of phase
    // The latest (side, mid) points, oldest first: x = (L - R) / sqrt 2, y = (L + R) / sqrt 2 (full scale mono sits at y = 1.41).
    void goniometer(std::array<std::pair<float, float>, kGoniometerPoints>& out) const;

private:
    double decay_ = 0.999;
    double ll_ = 0, rr_ = 0, lr_ = 0, mm_ = 0, ss_ = 0;
    std::array<std::pair<float, float>, kGoniometerPoints> points_{};
    int pointPos_ = 0, decimate_ = 4, decimateCount_ = 0;
};

// Log-frequency magnitude spectrum of the mono sum. Full scale sine == 0 dB.
class SpectrumAnalyzer
{
public:
    static constexpr int kFftLog2 = 12;                 // 4096 points
    static constexpr int kFftSize = 1 << kFftLog2;
    static constexpr int kBars = 64;
    static constexpr int kCurvePoints = 256;
    static constexpr float kFloorDb = -90.0f;

    explicit SpectrumAnalyzer(double sampleRate = 48000.0);
    void prepare(double sampleRate);
    void reset();
    void push(const float* left, const float* right, int numSamples); // mono sum into the window
    // Analyses the newest window. `seconds` = time since the previous call (drives the release smoothing).
    void analyze(double seconds);

    const std::array<float, kBars>& bars() const { return bars_; }          // dB, smoothed (fast attack, slow release)
    const std::array<float, kBars>& barPeaks() const { return barPeaks_; }  // dB, peak hold
    const std::array<float, kCurvePoints>& curve() const { return curve_; } // dB, 1/6 octave smoothed
    static float barFrequency(int bar);       // centre of a bar in Hz
    static float curveFrequency(int point);   // frequency of a curve point in Hz
    double sampleRate() const { return sampleRate_; }

private:
    float magnitudeAt(double hz) const; // linear interpolation of the dB spectrum
    double sampleRate_;
    Fft fft_;
    std::vector<float> window_, ring_;
    int ringPos_ = 0;
    std::vector<std::complex<float>> work_;
    std::vector<float> db_; // kFftSize / 2 + 1 bins
    std::array<float, kBars> bars_{}, barPeaks_{};
    std::array<float, kCurvePoints> curve_{};
    std::array<double, kBars> peakAge_{};
};

// Keeps the last half second of stereo audio and extracts a min / max trace for a time window, started on a
// rising zero crossing so periodic signals hold still.
class Scope
{
public:
    explicit Scope(double sampleRate = 48000.0) { prepare(sampleRate); }
    void prepare(double sampleRate);
    void reset();
    void push(const float* left, const float* right, int numSamples);

    struct Column { float minL, maxL, minR, maxR; };
    // `columns` columns spanning `windowMs` (1..400); `triggered` aligns to a rising zero crossing of the left channel.
    void trace(int columns, double windowMs, bool triggered, std::vector<Column>& out) const;

private:
    double sampleRate_ = 48000.0;
    std::vector<float> l_, r_;
    int pos_ = 0;
};

} // namespace sonora::dsp

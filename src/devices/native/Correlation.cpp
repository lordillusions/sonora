#include "devices/native/Correlation.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sonora::devices
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kSilentEnergy = 1.0e-7;   // kSilenceDb as mean energy
constexpr double kLowEnergy = 1.0e-6;      // -60 dBFS: the low end has something to measure
constexpr double kTiny = 1.0e-20;
constexpr float kRelease = 1.0f;           // held extremes move back this much per second once their hold ends
constexpr float kNone = 2.0f;              // no held value yet (outside -1..1)
bool none(float held) { return std::abs(held) > 1.5f; }
// Q of each of a band's two band-passes: together they are -3 dB at the third-octave edges (2^(+-1/6)).
constexpr double kBandQ = 2.77;
const char* const kBandLabels[Correlation::kBands] = {"20",  "25",  "31.5", "40",   "50",   "63",   "80",  "100", "125", "160", "200",
                                                      "250", "315", "400",  "500",  "630",  "800",  "1k",  "1.25k", "1.6k", "2k", "2.5k",
                                                      "3.15k", "4k", "5k",  "6.3k", "8k",   "10k",  "12.5k", "16k", "20k"};

std::vector<ParameterInfo> correlationParameters()
{
    using C = Correlation;
    std::vector<ParameterInfo> p(C::NumParams);
    p[C::Integration] = makeParam("Integration", "Integration", 30, 3000, 300, "ms");
    p[C::Hold] = makeParam("Hold", "Hold", 0, 3, 1, "", false, {"1 s", "3 s", "10 s", "Infinite"});
    p[C::ScopeMode] = makeParam("ScopeMode", "Scope", 0, 1, 0, "", false, {"Lissajous", "Polar"});
    p[C::ScopeAuto] = makeParam("ScopeAuto", "Auto Zoom", 0, 1, 1, "", true);
    p[C::ScopeZoom] = makeParam("ScopeZoom", "Zoom", 0, 30, 0, "dB");
    p[C::Listen] = makeParam("Listen", "Listen", 0, 4, 0, "", false, {"Stereo", "Mono", "Side", "Left", "Right"});
    return p;
}

float holdSeconds(int hold)
{
    switch (hold)
    {
        case Correlation::Hold1s: return 1.0f;
        case Correlation::Hold3s: return 3.0f;
        case Correlation::Hold10s: return 10.0f;
        default: return std::numeric_limits<float>::infinity();
    }
}

float decibels(double energy, float floor) { return energy > kTiny ? std::max(floor, (float) (10.0 * std::log10(energy))) : floor; }

float correlationOf(double lr, double ll, double rr)
{
    const double d = std::sqrt(ll * rr);
    return d > kTiny ? (float) std::clamp(lr / d, -1.0, 1.0) : 0.0f;
}

// Moves a held minimum (or, with sign -1, maximum) after a new reading.
void holdExtreme(float& held, float& left, float now, float seconds, float dt, float sign)
{
    if (none(held) || sign * now <= sign * held)
    {
        held = now;
        left = seconds;
        return;
    }
    if (std::isinf(seconds)) return;
    left -= dt;
    if (left > 0) return;
    held = sign > 0 ? std::min(now, held + kRelease * dt) : std::max(now, held - kRelease * dt);
}

void listenTo(int mode, float l, float r, float& outL, float& outR)
{
    switch (mode)
    {
        case Correlation::ListenMono: outL = outR = 0.5f * (l + r); return;
        case Correlation::ListenSide: outL = outR = 0.5f * (l - r); return;
        case Correlation::ListenLeft: outL = outR = l; return;
        case Correlation::ListenRight: outL = outR = r; return;
        default: outL = l; outR = r; return;
    }
}
} // namespace

float Correlation::bandCentre(int band) { return (float) (1000.0 * std::pow(2.0, (double) (band - 17) / 3.0)); }

std::string Correlation::bandLabel(int band) { return band >= 0 && band < kBands ? kBandLabels[band] : std::string(); }

Correlation::Correlation() : NativeDevice(correlationParameters()) {}

double Correlation::run(const Biquad& c, State& s, double x)
{
    const double y = c.b0 * x + s.z1;
    s.z1 = -c.a1 * y + s.z2;
    s.z2 = c.b2 * x - c.a2 * y;
    return y;
}

void Correlation::prepare(double sampleRate, int)
{
    sampleRate_ = sampleRate > 0 ? sampleRate : 44100.0;
    readingInterval_ = std::max(16, (int) std::lround(sampleRate_ / kHistoryRate));
    integrationMs_ = -1;
    for (int b = 0; b < kBands; ++b)
    {
        auto& band = bands_[(size_t) b];
        const double f = bandCentre(b);
        band.active = f * std::pow(2.0, 1.0 / 6.0) < 0.48 * sampleRate_;
        const double w = 2.0 * kPi * std::min(f, 0.49 * sampleRate_) / sampleRate_;
        const double alpha = std::sin(w) / (2.0 * kBandQ), a0 = 1.0 + alpha;
        band.c.b0 = alpha / a0;
        band.c.b2 = -alpha / a0;
        band.c.a1 = -2.0 * std::cos(w) / a0;
        band.c.a2 = (1.0 - alpha) / a0;
        bandActive_[(size_t) b].store(band.active, std::memory_order_relaxed);
    }
    reset();
}

void Correlation::setCoefficients()
{
    integrationMs_ = value(Integration);
    const double seconds = std::max(0.001, (double) integrationMs_ * 0.001);
    keep_ = std::exp(-1.0 / (seconds * sampleRate_));
    take_ = 1.0 - keep_;
    for (int b = 0; b < kBands; ++b)
    {
        auto& band = bands_[(size_t) b];
        const double bandSeconds = std::max(seconds, 8.0 / bandCentre(b)); // at least 8 periods: 400 ms at 20 Hz
        band.keep = std::exp(-1.0 / (bandSeconds * sampleRate_));
        band.take = 1.0 - band.keep;
    }
}

void Correlation::clearHolds()
{
    minHeld_ = kNone;
    maxHeld_ = -kNone;
    minHold_ = maxHold_ = 0;
    for (auto& band : bands_)
    {
        band.minHeld = kNone;
        band.holdLeft = 0;
    }
    historyNext_ = 0;
    historyHead_.store(0, std::memory_order_relaxed);
    historyCount_.store(0, std::memory_order_release);
}

void Correlation::reset()
{
    ll_ = rr_ = lr_ = 0;
    peakL_ = peakR_ = 0;
    sinceReading_ = 0;
    for (auto& band : bands_)
    {
        band.l[0] = band.l[1] = band.r[0] = band.r[1] = {};
        band.ll = band.rr = band.lr = 0;
    }
    listenFrom_ = listenTo_ = (int) std::lround(value(Listen));
    fadeLeft_ = 0;
    clearHolds();
    signal_.store(false, std::memory_order_relaxed);
    lowSignal_.store(false, std::memory_order_relaxed);
    correlation_.store(0, std::memory_order_relaxed);
    for (int b = 0; b < kBands; ++b)
    {
        bandCorrelation_[(size_t) b].store(0, std::memory_order_relaxed);
        bandMin_[(size_t) b].store(0, std::memory_order_relaxed);
        bandLevel_[(size_t) b].store(-120, std::memory_order_relaxed);
    }
}

void Correlation::publish()
{
    const float dt = (float) readingInterval_ / (float) sampleRate_;
    const float hold = holdSeconds((int) std::lround(value(Hold)));
    const double mean = 0.5 * (ll_ + rr_);
    const bool signal = mean > kSilentEnergy;
    const float now = signal ? correlationOf(lr_, ll_, rr_) : 0.0f;
    if (signal)
    {
        holdExtreme(minHeld_, minHold_, now, hold, dt, 1.0f);
        holdExtreme(maxHeld_, maxHold_, now, hold, dt, -1.0f);
    }
    double lowLr = 0, lowLl = 0, lowRr = 0;
    const double bandFloor = std::pow(10.0, (double) kBandFloorDb / 10.0);
    for (int b = 0; b < kBands; ++b)
    {
        auto& band = bands_[(size_t) b];
        if (! band.active) continue;
        const double energy = 0.5 * (band.ll + band.rr);
        const bool bandSignal = energy > bandFloor;
        const float c = bandSignal ? correlationOf(band.lr, band.ll, band.rr) : 0.0f;
        if (bandSignal) holdExtreme(band.minHeld, band.holdLeft, c, hold, dt, 1.0f);
        bandCorrelation_[(size_t) b].store(c, std::memory_order_relaxed);
        bandMin_[(size_t) b].store(none(band.minHeld) ? c : band.minHeld, std::memory_order_relaxed);
        bandLevel_[(size_t) b].store(decibels(energy, -120.0f), std::memory_order_relaxed);
        if (bandCentre(b) <= kLowEndHz * 1.01f)
        {
            lowLr += band.lr;
            lowLl += band.ll;
            lowRr += band.rr;
        }
    }
    const bool lowSignal = 0.5 * (lowLl + lowRr) > kLowEnergy;
    const float low = lowSignal ? correlationOf(lowLr, lowLl, lowRr) : 0.0f;

    const double mid = 0.5 * (ll_ + rr_ + 2.0 * lr_), side = 0.5 * (ll_ + rr_ - 2.0 * lr_);
    const float width = mid > kTiny ? (float) std::min(10.0, std::sqrt(std::max(0.0, side) / mid)) : (side > kTiny ? 10.0f : 0.0f);
    const double monoRatio = ll_ + rr_ > kTiny ? (ll_ + rr_ + 2.0 * lr_) / (2.0 * (ll_ + rr_)) : 1.0;
    const float balance = ll_ > kTiny ? (rr_ > kTiny ? (float) std::clamp(10.0 * std::log10(rr_ / ll_), -60.0, 60.0) : -60.0f)
                                      : (rr_ > kTiny ? 60.0f : 0.0f);
    signal_.store(signal, std::memory_order_relaxed);
    correlation_.store(now, std::memory_order_relaxed);
    minShown_.store(none(minHeld_) ? now : minHeld_, std::memory_order_relaxed);
    maxShown_.store(none(maxHeld_) ? now : maxHeld_, std::memory_order_relaxed);
    lowSignal_.store(lowSignal, std::memory_order_relaxed);
    lowEnd_.store(low, std::memory_order_relaxed);
    width_.store(width, std::memory_order_relaxed);
    mono_.store(signal ? decibels(monoRatio, -60.0f) : 0.0f, std::memory_order_relaxed);
    balance_.store(signal ? balance : 0.0f, std::memory_order_relaxed);
    levelL_.store(decibels(ll_, -120.0f), std::memory_order_relaxed);
    levelR_.store(decibels(rr_, -120.0f), std::memory_order_relaxed);
    peakShownL_.store(peakL_ > 0 ? std::max(-120.0f, 20.0f * std::log10(peakL_)) : -120.0f, std::memory_order_relaxed);
    peakShownR_.store(peakR_ > 0 ? std::max(-120.0f, 20.0f * std::log10(peakR_)) : -120.0f, std::memory_order_relaxed);
    peakL_ = peakR_ = 0;

    const int slot = historyNext_;
    historyCorrelation_[(size_t) slot].store(now, std::memory_order_relaxed);
    historyLow_[(size_t) slot].store(low, std::memory_order_relaxed);
    historyFlags_[(size_t) slot].store((uint8_t) ((signal ? 1 : 0) | (lowSignal ? 2 : 0)), std::memory_order_relaxed);
    historyNext_ = (historyNext_ + 1) % kHistory;
    historyHead_.store(historyNext_, std::memory_order_relaxed);
    const uint32_t count = historyCount_.load(std::memory_order_relaxed);
    historyCount_.store(std::min<uint32_t>(count + 1, (uint32_t) kHistory), std::memory_order_release); // publishes the slot
}

void Correlation::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    const int n = audio.getNumSamples();
    if (n <= 0 || audio.getNumChannels() < 1) return;
    float* left = audio.getWritePointer(0);
    float* right = audio.getNumChannels() > 1 ? audio.getWritePointer(1) : left;
    if (resetRequested_.exchange(false, std::memory_order_relaxed)) clearHolds();
    if (std::abs(value(Integration) - integrationMs_) > 0.001f) setCoefficients();
    const bool frozen = frozen_.load(std::memory_order_relaxed);

    for (int i = 0; i < n; ++i)
    {
        double l = left[i], r = right[i];
        if (! std::isfinite(l)) l = 0; // a NaN would poison every running sum for good
        if (! std::isfinite(r)) r = 0;
        ll_ = keep_ * ll_ + take_ * l * l;
        rr_ = keep_ * rr_ + take_ * r * r;
        lr_ = keep_ * lr_ + take_ * l * r;
        peakL_ = std::max(peakL_, (float) std::abs(l));
        peakR_ = std::max(peakR_, (float) std::abs(r));
        for (auto& band : bands_)
        {
            if (! band.active) continue;
            const double bl = run(band.c, band.l[1], run(band.c, band.l[0], l));
            const double br = run(band.c, band.r[1], run(band.c, band.r[0], r));
            band.ll = band.keep * band.ll + band.take * bl * bl;
            band.rr = band.keep * band.rr + band.take * br * br;
            band.lr = band.keep * band.lr + band.take * bl * br;
        }
        if (! frozen)
        {
            const size_t at = (size_t) (writtenLocal_ % (uint64_t) kScopePoints);
            scopeX_[at].store((float) ((r - l) * 0.70710678118654752), std::memory_order_relaxed);
            scopeY_[at].store((float) ((l + r) * 0.70710678118654752), std::memory_order_relaxed);
            ++writtenLocal_;
        }
        if (++sinceReading_ >= readingInterval_)
        {
            sinceReading_ = 0;
            if (! frozen) publish();
            else peakL_ = peakR_ = 0;
        }
    }
    written_.store(writtenLocal_, std::memory_order_release);

    // Listen: the check modes replace the output; Stereo leaves it untouched. A switch crossfades over 10 ms.
    const int listen = (int) std::lround(value(Listen));
    if (listen != listenTo_)
    {
        listenFrom_ = listenTo_; // a switch during a fade starts from the mode it was fading to
        listenTo_ = listen;
        fadeLen_ = std::max(1, (int) (0.01 * sampleRate_));
        fadeLeft_ = fadeLen_;
    }
    if (listenTo_ == ListenStereo && fadeLeft_ <= 0) return;
    for (int i = 0; i < n; ++i)
    {
        float toL, toR;
        listenTo(listenTo_, left[i], right[i], toL, toR);
        if (fadeLeft_ > 0)
        {
            float fromL, fromR;
            listenTo(listenFrom_, left[i], right[i], fromL, fromR);
            const float g = (float) fadeLeft_ / (float) fadeLen_;
            toL = toL + (fromL - toL) * g;
            toR = toR + (fromR - toR) * g;
            --fadeLeft_;
        }
        left[i] = toL;
        if (right != left) right[i] = toR;
    }
}

Correlation::Reading Correlation::reading() const
{
    Reading r;
    r.signal = signal_.load(std::memory_order_relaxed);
    r.correlation = correlation_.load(std::memory_order_relaxed);
    r.minHeld = minShown_.load(std::memory_order_relaxed);
    r.maxHeld = maxShown_.load(std::memory_order_relaxed);
    r.lowSignal = lowSignal_.load(std::memory_order_relaxed);
    r.lowEnd = lowEnd_.load(std::memory_order_relaxed);
    r.balanceDb = balance_.load(std::memory_order_relaxed);
    r.width = width_.load(std::memory_order_relaxed);
    r.monoDb = mono_.load(std::memory_order_relaxed);
    r.levelL = levelL_.load(std::memory_order_relaxed);
    r.levelR = levelR_.load(std::memory_order_relaxed);
    r.peakL = peakShownL_.load(std::memory_order_relaxed);
    r.peakR = peakShownR_.load(std::memory_order_relaxed);
    r.frozen = frozen();
    return r;
}

Correlation::Band Correlation::band(int index) const
{
    Band b;
    if (index < 0 || index >= kBands) return b;
    b.active = bandActive_[(size_t) index].load(std::memory_order_relaxed);
    b.correlation = bandCorrelation_[(size_t) index].load(std::memory_order_relaxed);
    b.minHeld = bandMin_[(size_t) index].load(std::memory_order_relaxed);
    b.levelDb = bandLevel_[(size_t) index].load(std::memory_order_relaxed);
    b.signal = b.active && b.levelDb > kBandFloorDb;
    return b;
}

Correlation::HistoryPoint Correlation::history(int newestFirst) const
{
    const uint32_t count = historyCount_.load(std::memory_order_acquire);
    if (newestFirst < 0 || newestFirst >= kHistory || (uint32_t) newestFirst >= count) return {};
    const int slot = ((historyHead_.load(std::memory_order_relaxed) - 1 - newestFirst) % kHistory + kHistory) % kHistory;
    const auto flags = historyFlags_[(size_t) slot].load(std::memory_order_relaxed);
    return {(flags & 1) != 0, (flags & 2) != 0, historyCorrelation_[(size_t) slot].load(std::memory_order_relaxed),
            historyLow_[(size_t) slot].load(std::memory_order_relaxed)};
}

int Correlation::scope(float* x, float* y, int count) const
{
    const uint64_t total = written_.load(std::memory_order_acquire);
    const int n = (int) std::min<uint64_t>({total, (uint64_t) std::max(0, count), (uint64_t) kScopePoints});
    for (int i = 0; i < n; ++i)
    {
        const size_t at = (size_t) ((total - (uint64_t) n + (uint64_t) i) % (uint64_t) kScopePoints);
        x[i] = scopeX_[at].load(std::memory_order_relaxed);
        y[i] = scopeY_[at].load(std::memory_order_relaxed);
    }
    return n;
}

std::vector<DeviceProcessor::UiGroup> Correlation::uiGroups() const
{
    return {{"Meter", {Integration, Hold}, 2}, {"Scope", {ScopeMode, ScopeAuto, ScopeZoom}, 3}, {"Listen", {Listen}, 1}};
}
} // namespace sonora::devices

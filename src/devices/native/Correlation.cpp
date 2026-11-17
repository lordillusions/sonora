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
// Kick x Bass
constexpr double kPairRate = 3000.0;       // the low end is analysed at about this rate (decimated after the low-pass)
constexpr double kPairSilent = 1.0e-7;     // -70 dBFS: a low-passed kick or bass below this is "not playing"
constexpr double kPairFloor = 1.0e-10;     // -100 dBFS: below this an envelope gives no overlap weight
constexpr double kEnvelopeSeconds = 0.010; // the energy envelopes the overlap weight compares
constexpr double kLagMinSeconds = 0.16;
constexpr double kWeightFrom = 0.06, kWeightFull = 0.5; // the overlap weight's ramp over rho (see pairStep)    // the best-shift search averages at least 8 periods of 50 Hz
const char* const kNoteLabels[Correlation::kNotes] = {"1/64", "1/32", "1/16", "1/8", "1/4", "1/2", "1 bar", "2 bars"};
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
    p[C::Listen] = makeParam("Listen", "Listen", 0, 5, 0, "", false, {"Stereo", "Mono", "Side", "Left", "Right", "Bass Inverted"});
    // PLG-24c
    p[C::Mode] = makeParam("Mode", "Measure", 0, 1, 0, "", false, {"Stereo", "Kick \xC3\x97 Bass"});
    p[C::KickFrom] = makeParam("KickFrom", "Kick From", 0, 1, 0, "", false, {"Sidechain", "Left"});
    p[C::Sync] = makeParam("Sync", "Sync", 0, 1, 0, "", true);
    p[C::IntegrationNote] = makeParam("IntegrationNote", "Integration Note", 0, (float) (C::kNotes - 1), 4, "", false,
                                      std::vector<std::string>(kNoteLabels, kNoteLabels + C::kNotes));
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

// `bassRight`: Kick From Left (the kick left, the bass right), so Bass Inverted flips only the right channel.
void listenTo(int mode, float l, float r, bool bassRight, float& outL, float& outR)
{
    switch (mode)
    {
        case Correlation::ListenMono: outL = outR = 0.5f * (l + r); return;
        case Correlation::ListenSide: outL = outR = 0.5f * (l - r); return;
        case Correlation::ListenLeft: outL = outR = l; return;
        case Correlation::ListenRight: outL = outR = r; return;
        case Correlation::ListenBassInverted: outL = bassRight ? l : -l; outR = -r; return;
        default: outL = l; outR = r; return;
    }
}

// RBJ biquads for the Kick x Bass low end (Butterworth Q: two low-passes in a row are a Linkwitz-Riley).
void lowPass(double sampleRate, double hz, double& b0, double& b1, double& b2, double& a1, double& a2)
{
    const double w = 2.0 * kPi * std::min(hz, 0.45 * sampleRate) / sampleRate, alpha = std::sin(w) / (2.0 * 0.70710678118654752);
    const double a0 = 1.0 + alpha, c = std::cos(w);
    b0 = (1.0 - c) * 0.5 / a0;
    b1 = (1.0 - c) / a0;
    b2 = b0;
    a1 = -2.0 * c / a0;
    a2 = (1.0 - alpha) / a0;
}

void highPass(double sampleRate, double hz, double& b0, double& b1, double& b2, double& a1, double& a2)
{
    const double w = 2.0 * kPi * std::min(hz, 0.45 * sampleRate) / sampleRate, alpha = std::sin(w) / (2.0 * 0.70710678118654752);
    const double a0 = 1.0 + alpha, c = std::cos(w);
    b0 = (1.0 + c) * 0.5 / a0;
    b1 = -(1.0 + c) / a0;
    b2 = b0;
    a1 = -2.0 * c / a0;
    a2 = (1.0 - alpha) / a0;
}

double clean(double x) { return std::isfinite(x) ? x : 0.0; } // a NaN would poison every running sum for good
} // namespace

float Correlation::bandCentre(int band) { return (float) (1000.0 * std::pow(2.0, (double) (band - 17) / 3.0)); }

std::string Correlation::bandLabel(int band) { return band >= 0 && band < kBands ? kBandLabels[band] : std::string(); }

double Correlation::noteBeats(int note, int numerator, int denominator)
{
    static const double beats[kNotes] = {1.0 / 16.0, 1.0 / 8.0, 0.25, 0.5, 1.0, 2.0, 0.0, 0.0};
    note = std::clamp(note, 0, kNotes - 1);
    if (note < 6) return beats[note];
    const double bar = numerator > 0 && denominator > 0 ? (double) numerator * 4.0 / (double) denominator : 4.0;
    return note == 6 ? bar : 2.0 * bar;
}

std::string Correlation::noteLabel(int note) { return kNoteLabels[std::clamp(note, 0, kNotes - 1)]; }

float Correlation::wantedIntegrationMs() const
{
    if (value(Sync) < 0.5f) return value(Integration);
    const double tempo = host_.tempo > 1.0 ? host_.tempo : 120.0;
    const double beats = noteBeats((int) std::lround(value(IntegrationNote)), host_.timeSigNumerator, host_.timeSigDenominator);
    return (float) std::clamp(beats * 60000.0 / tempo, 5.0, 10000.0);
}

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
    // Kick x Bass: 25 Hz high-pass, Linkwitz-Riley low-pass at kLowEndHz, then every decimation_-th sample.
    for (auto* chain : {kickLow_, bassLow_})
    {
        highPass(sampleRate_, 25.0, chain[0].b0, chain[0].b1, chain[0].b2, chain[0].a1, chain[0].a2);
        for (int i = 1; i < 3; ++i) lowPass(sampleRate_, kLowEndHz, chain[i].b0, chain[i].b1, chain[i].b2, chain[i].a1, chain[i].a2);
    }
    decimation_ = std::max(1, (int) std::lround(sampleRate_ / kPairRate));
    pairRate_ = sampleRate_ / (double) decimation_;
    pairRateShown_.store((float) pairRate_, std::memory_order_relaxed);
    lags_ = std::clamp((int) std::lround((double) kMaxShiftMs * 0.001 * pairRate_), 1, kMaxLag);
    envKeep_ = std::exp(-1.0 / (kEnvelopeSeconds * pairRate_));
    envTake_ = 1.0 - envKeep_;
    onsetRelease_ = std::exp(-1.0 / (0.030 * pairRate_));     // the onset envelope's release (peak follower)
    onsetPeakRelease_ = std::exp(-1.0 / (1.5 * pairRate_));   // the loudest recent kick
    onsetBack_ = std::clamp((int) std::lround(0.008 * pairRate_), 1, kOnsetLook - 1);
    onsetGap_ = std::max(1, (int) std::lround(0.070 * pairRate_));
    reset();
}

void Correlation::setCoefficients(float ms)
{
    integrationMs_ = ms;
    integrationShown_.store(ms, std::memory_order_relaxed);
    const double seconds = std::max(0.001, (double) integrationMs_ * 0.001);
    keep_ = std::exp(-1.0 / (seconds * sampleRate_));
    take_ = 1.0 - keep_;
    pairKeep_ = std::exp(-1.0 / (seconds * pairRate_));
    pairTake_ = 1.0 - pairKeep_;
    lagKeep_ = std::exp(-1.0 / (std::max(seconds, kLagMinSeconds) * pairRate_));
    lagTake_ = 1.0 - lagKeep_;
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

void Correlation::clearMeasurements()
{
    ll_ = rr_ = lr_ = 0;
    peakL_ = peakR_ = 0;
    for (auto& band : bands_)
    {
        band.l[0] = band.l[1] = band.r[0] = band.r[1] = {};
        band.ll = band.rr = band.lr = band.wll = band.wrr = band.wlr = 0;
    }
    for (auto* chain : {kickLow_, bassLow_})
        for (int i = 0; i < 3; ++i) chain[i].z1 = chain[i].z2 = 0;
    decimationPhase_ = 0;
    envKick_ = envBass_ = weight_ = 0;
    akk_ = abb_ = wkk_ = wbb_ = wkb_ = 0;
    lagKb_.fill(0);
    lagBb_.fill(0);
    lagKk_ = 0;
    ringKick_.fill(0);
    ringBass_.fill(0);
    ringWeight_.fill(0);
    ringAt_ = 0;
    onsetEnv_ = onsetPeak_ = 0;
    onsetLook_.fill(0);
    anyOnset_ = false;
    onsets_.store(0, std::memory_order_relaxed);
    clearHolds();
}

void Correlation::reset()
{
    sinceReading_ = 0;
    measure_ = (int) std::lround(value(Mode));
    kickFrom_ = (int) std::lround(value(KickFrom));
    clearMeasurements();
    listenFrom_ = listenTo_ = (int) std::lround(value(Listen));
    fadeLeft_ = 0;
    signal_.store(false, std::memory_order_relaxed);
    lowSignal_.store(false, std::memory_order_relaxed);
    correlation_.store(0, std::memory_order_relaxed);
    for (auto* flag : {&kickShown_, &bassShown_, &overlap_, &sidechainShown_}) flag->store(false, std::memory_order_relaxed);
    for (auto* v : {&shared_, &sumDb_, &invertedDb_, &shiftMs_, &shiftCorrelation_}) v->store(0, std::memory_order_relaxed);
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
    const bool pair = measure_ == MeasureKickBass;
    const double mean = 0.5 * (ll_ + rr_);
    bool signal = mean > kSilentEnergy;
    float now = signal ? correlationOf(lr_, ll_, rr_) : 0.0f;
    float shared = 0;
    if (pair)
    {
        // Kick x Bass: only where they sound together counts (the weighted sums).
        const bool kick = akk_ > kPairSilent, bass = abb_ > kPairSilent;
        const double all = akk_ + abb_, together = wkk_ + wbb_;
        shared = all > kTiny ? (float) std::clamp(together / all, 0.0, 1.0) : 0.0f;
        const bool overlap = kick && bass && shared >= kMinShared && wkk_ > kTiny && wbb_ > kTiny;
        const double ratio = together > kTiny ? std::clamp(2.0 * wkb_ / together, -1.0, 1.0) : 0.0;
        // The best delay of the bass: the weighted correlation at every lag, the smallest shift within 0.01 of the best,
        // refined between lags by a parabola.
        std::array<float, kLagSlots> at{};
        for (int j = -lags_; j <= lags_; ++j)
            at[(size_t) (j + kMaxLag)] = correlationOf(lagKb_[(size_t) (j + kMaxLag)], lagKk_, lagBb_[(size_t) (j + kMaxLag)]);
        int best = 0;
        for (int step = 1; step <= lags_; ++step)
            for (const int j : {step, -step})
                if (at[(size_t) (j + kMaxLag)] > at[(size_t) (best + kMaxLag)] + 0.01f) best = j;
        double shift = best, bestValue = at[(size_t) (best + kMaxLag)];
        if (best > -lags_ && best < lags_)
        {
            const double a = at[(size_t) (best - 1 + kMaxLag)], b = at[(size_t) (best + kMaxLag)], c = at[(size_t) (best + 1 + kMaxLag)];
            const double curve = a - 2.0 * b + c;
            if (curve < -1.0e-9)
            {
                const double delta = std::clamp(0.5 * (a - c) / curve, -0.5, 0.5);
                shift += delta;
                bestValue = std::min(1.0, b - 0.25 * (a - c) * delta);
            }
        }
        signal = overlap;
        now = overlap ? correlationOf(wkb_, wkk_, wbb_) : 0.0f;
        kickShown_.store(kick, std::memory_order_relaxed);
        bassShown_.store(bass, std::memory_order_relaxed);
        overlap_.store(overlap, std::memory_order_relaxed);
        shared_.store(shared, std::memory_order_relaxed);
        sumDb_.store(overlap ? decibels(1.0 + ratio, -40.0f) : 0.0f, std::memory_order_relaxed);
        invertedDb_.store(overlap ? decibels(1.0 - ratio, -40.0f) : 0.0f, std::memory_order_relaxed);
        shiftMs_.store(overlap ? (float) (shift * 1000.0 / pairRate_) : 0.0f, std::memory_order_relaxed);
        shiftCorrelation_.store(overlap ? (float) bestValue : 0.0f, std::memory_order_relaxed);
        kickDb_.store(decibels(akk_, -120.0f), std::memory_order_relaxed);
        bassDb_.store(decibels(abb_, -120.0f), std::memory_order_relaxed);
    }
    kickBass_.store(pair, std::memory_order_relaxed);
    sidechainShown_.store(sidechainNow_, std::memory_order_relaxed);
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
        // Kick x Bass: a band is measured where the kick and the bass meet in it (its weighted share).
        const double lr = pair ? band.wlr : band.lr, ll = pair ? band.wll : band.ll, rr = pair ? band.wrr : band.rr;
        const bool bandSignal = energy > bandFloor && (! pair || (0.5 * (ll + rr) >= (double) kMinShared * energy && ll > kTiny && rr > kTiny));
        const float c = bandSignal ? correlationOf(lr, ll, rr) : 0.0f;
        if (bandSignal) holdExtreme(band.minHeld, band.holdLeft, c, hold, dt, 1.0f);
        bandCorrelation_[(size_t) b].store(c, std::memory_order_relaxed);
        bandMin_[(size_t) b].store(none(band.minHeld) ? c : band.minHeld, std::memory_order_relaxed);
        bandLevel_[(size_t) b].store(decibels(energy, -120.0f), std::memory_order_relaxed);
        bandMeasured_[(size_t) b].store(bandSignal, std::memory_order_relaxed);
        if (bandCentre(b) <= kLowEndHz * 1.01f)
        {
            lowLr += lr;
            lowLl += ll;
            lowRr += rr;
        }
    }
    const bool lowSignal = 0.5 * (lowLl + lowRr) > (pair ? kTiny : kLowEnergy);
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
    mono_.store(mean > kSilentEnergy ? decibels(monoRatio, -60.0f) : 0.0f, std::memory_order_relaxed);
    balance_.store(mean > kSilentEnergy ? balance : 0.0f, std::memory_order_relaxed);
    levelL_.store(decibels(ll_, -120.0f), std::memory_order_relaxed);
    levelR_.store(decibels(rr_, -120.0f), std::memory_order_relaxed);
    peakShownL_.store(peakL_ > 0 ? std::max(-120.0f, 20.0f * std::log10(peakL_)) : -120.0f, std::memory_order_relaxed);
    peakShownR_.store(peakR_ > 0 ? std::max(-120.0f, 20.0f * std::log10(peakR_)) : -120.0f, std::memory_order_relaxed);
    peakL_ = peakR_ = 0;

    const int slot = historyNext_;
    historyCorrelation_[(size_t) slot].store(now, std::memory_order_relaxed);
    historyLow_[(size_t) slot].store(low, std::memory_order_relaxed);
    historyShared_[(size_t) slot].store(shared, std::memory_order_relaxed);
    historyFlags_[(size_t) slot].store((uint8_t) ((signal ? 1 : 0) | (lowSignal ? 2 : 0)), std::memory_order_relaxed);
    historyNext_ = (historyNext_ + 1) % kHistory;
    historyHead_.store(historyNext_, std::memory_order_relaxed);
    const uint32_t count = historyCount_.load(std::memory_order_relaxed);
    historyCount_.store(std::min<uint32_t>(count + 1, (uint32_t) kHistory), std::memory_order_release); // publishes the slot
}

// One decimated sample of the low-passed kick and bass: the overlap weight, the window sums, the lag search, the kick's
// onsets and the beat view's ring.
void Correlation::pairStep(double k, double b, bool frozen)
{
    // The overlap weight. rho = 2 |k b| / (k^2 + b^2), numerator and denominator each smoothed over 10 ms: 1 when they are
    // as loud as each other, about twice the quieter one's amplitude ratio when it is far below. Smoothing the product, not
    // each signal, leaves no memory of a kick that stopped before the bass came in. Then a ramp: nothing while the quieter
    // one is ~30 dB down (rho 0.06: it moves the sum by 0.3 dB at most, and a loud kick over a trace of bass would swamp the
    // real overlap), all from ~12 dB down (rho 0.5).
    envKick_ = envKeep_ * envKick_ + envTake_ * 2.0 * std::abs(k * b);
    envBass_ = envKeep_ * envBass_ + envTake_ * (k * k + b * b);
    const double rho = envBass_ > kPairFloor ? envKick_ / envBass_ : 0.0;
    const double w = std::clamp((rho - kWeightFrom) / (kWeightFull - kWeightFrom), 0.0, 1.0);
    weight_ = w;
    akk_ = pairKeep_ * akk_ + pairTake_ * k * k;
    abb_ = pairKeep_ * abb_ + pairTake_ * b * b;
    wkk_ = pairKeep_ * wkk_ + pairTake_ * w * k * k;
    wbb_ = pairKeep_ * wbb_ + pairTake_ * w * b * b;
    wkb_ = pairKeep_ * wkb_ + pairTake_ * w * k * b;

    // Lags: the kick (and its weight) lags_ samples ago against the bass delayed by j = -lags_ .. +lags_ from there.
    const int size = 2 * lags_ + 1;
    ringKick_[(size_t) ringAt_] = k;
    ringBass_[(size_t) ringAt_] = b;
    ringWeight_[(size_t) ringAt_] = w;
    const int centre = (ringAt_ - lags_ + size) % size;
    const double kc = ringKick_[(size_t) centre], wc = ringWeight_[(size_t) centre];
    lagKk_ = lagKeep_ * lagKk_ + lagTake_ * wc * kc * kc;
    for (int j = -lags_; j <= lags_; ++j)
    {
        const double bj = ringBass_[(size_t) ((centre - j + size) % size)];
        auto& kb = lagKb_[(size_t) (j + kMaxLag)];
        auto& bb = lagBb_[(size_t) (j + kMaxLag)];
        kb = lagKeep_ * kb + lagTake_ * wc * kc * bj;
        bb = lagKeep_ * bb + lagTake_ * wc * bj * bj;
    }
    ringAt_ = (ringAt_ + 1) % size;

    // Kick onsets (for the beat view): a peak follower that rises 6 dB within 8 ms to at least half the loudest recent kick.
    onsetEnv_ = std::max(std::abs(k), onsetEnv_ * onsetRelease_);
    onsetPeak_ = std::max(onsetEnv_, onsetPeak_ * onsetPeakRelease_);
    const double before = onsetLook_[(size_t) ((pairCount_ + (uint64_t) kOnsetLook - (uint64_t) onsetBack_) % (uint64_t) kOnsetLook)];
    onsetLook_[(size_t) (pairCount_ % (uint64_t) kOnsetLook)] = onsetEnv_;
    if (! frozen && onsetEnv_ > 1.0e-3 && onsetEnv_ >= 0.5 * onsetPeak_ && onsetEnv_ > 2.0 * before
        && (! anyOnset_ || pairCount_ - lastOnset_ >= (uint64_t) onsetGap_))
    {
        // The onset is where the rise began: up to onsetBack_ samples ago, at the ring's position then.
        const uint64_t at = beatLocal_ >= (uint64_t) onsetBack_ ? beatLocal_ - (uint64_t) onsetBack_ : 0;
        onsetPrevious_.store(onsetLatest_.load(std::memory_order_relaxed), std::memory_order_relaxed);
        onsetLatest_.store(at, std::memory_order_relaxed);
        onsets_.store(std::min(2, onsets_.load(std::memory_order_relaxed) + 1), std::memory_order_release);
        lastOnset_ = pairCount_;
        anyOnset_ = true;
    }
    ++pairCount_;
    if (frozen) return;
    const size_t slot = (size_t) (beatLocal_ % (uint64_t) kBeatPoints);
    beatKick_[slot].store((float) k, std::memory_order_relaxed);
    beatBass_[slot].store((float) b, std::memory_order_relaxed);
    beatWeight_[slot].store((float) w, std::memory_order_relaxed);
    ++beatLocal_;
    beatWritten_.store(beatLocal_, std::memory_order_release);
}

void Correlation::process(juce::AudioBuffer<float>& audio, juce::MidiBuffer&)
{
    const int n = audio.getNumSamples();
    const juce::AudioBuffer<float>* sidechain = sidechain_;
    sidechain_ = nullptr; // delivered again before every block that has one (never keep a pointer into an old graph)
    if (n <= 0 || audio.getNumChannels() < 1) return;
    float* left = audio.getWritePointer(0);
    float* right = audio.getNumChannels() > 1 ? audio.getWritePointer(1) : left;
    if (resetRequested_.exchange(false, std::memory_order_relaxed)) clearHolds();
    const int measure = (int) std::lround(value(Mode)), kickFrom = (int) std::lround(value(KickFrom));
    if (measure != measure_ || kickFrom != kickFrom_) // the sums of the other comparison mean nothing here
    {
        measure_ = measure;
        kickFrom_ = kickFrom;
        clearMeasurements();
    }
    if (const float wanted = wantedIntegrationMs(); std::abs(wanted - integrationMs_) > 0.001f) setCoefficients(wanted);
    tempoShown_.store(host_.tempo > 1.0 ? host_.tempo : 120.0, std::memory_order_relaxed);
    const bool frozen = frozen_.load(std::memory_order_relaxed);
    const bool pair = measure_ == MeasureKickBass, kickLeft = kickFrom_ == KickLeft;
    const juce::AudioBuffer<float>* sc = pair && ! kickLeft && sidechain != nullptr && sidechain->getNumSamples() >= n && sidechain->getNumChannels() > 0
                                             ? sidechain : nullptr;
    sidechainNow_ = sc != nullptr;
    const float* scL = sc != nullptr ? sc->getReadPointer(0) : nullptr;
    const float* scR = sc != nullptr ? sc->getReadPointer(std::min(1, sc->getNumChannels() - 1)) : nullptr;
    const double scGain = 0.5 * (double) sidechainGain_;

    for (int i = 0; i < n; ++i)
    {
        const double l = clean(left[i]), r = clean(right[i]);
        // What is compared: left with right, or (Kick x Bass) the kick with the bass.
        double a = l, b = r;
        if (pair)
        {
            if (! kickLeft)
            {
                a = scL != nullptr ? clean(scGain * ((double) scL[i] + (double) scR[i])) : 0.0;
                b = 0.5 * (l + r);
            }
            const double k = kickLow_[2].run(kickLow_[1].run(kickLow_[0].run(a)));
            const double bass = bassLow_[2].run(bassLow_[1].run(bassLow_[0].run(b)));
            if (++decimationPhase_ >= decimation_)
            {
                decimationPhase_ = 0;
                pairStep(k, bass, frozen);
            }
        }
        ll_ = keep_ * ll_ + take_ * a * a;
        rr_ = keep_ * rr_ + take_ * b * b;
        lr_ = keep_ * lr_ + take_ * a * b;
        peakL_ = std::max(peakL_, (float) std::abs(a));
        peakR_ = std::max(peakR_, (float) std::abs(b));
        const double w = weight_;
        for (auto& band : bands_)
        {
            if (! band.active) continue;
            const double bl = run(band.c, band.l[1], run(band.c, band.l[0], a));
            const double br = run(band.c, band.r[1], run(band.c, band.r[0], b));
            band.ll = band.keep * band.ll + band.take * bl * bl;
            band.rr = band.keep * band.rr + band.take * br * br;
            band.lr = band.keep * band.lr + band.take * bl * br;
            if (pair)
            {
                band.wll = band.keep * band.wll + band.take * w * bl * bl;
                band.wrr = band.keep * band.wrr + band.take * w * br * br;
                band.wlr = band.keep * band.wlr + band.take * w * bl * br;
            }
        }
        if (! frozen)
        {
            const size_t at = (size_t) (writtenLocal_ % (uint64_t) kScopePoints);
            scopeX_[at].store((float) ((b - a) * 0.70710678118654752), std::memory_order_relaxed);
            scopeY_[at].store((float) ((a + b) * 0.70710678118654752), std::memory_order_relaxed);
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
    const bool bassRight = pair && kickLeft;
    for (int i = 0; i < n; ++i)
    {
        float toL, toR;
        listenTo(listenTo_, left[i], right[i], bassRight, toL, toR);
        if (fadeLeft_ > 0)
        {
            float fromL, fromR;
            listenTo(listenFrom_, left[i], right[i], bassRight, fromL, fromR);
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
    r.kickBass = kickBass_.load(std::memory_order_relaxed);
    r.sidechain = sidechainShown_.load(std::memory_order_relaxed);
    r.kick = kickShown_.load(std::memory_order_relaxed);
    r.bass = bassShown_.load(std::memory_order_relaxed);
    r.overlap = overlap_.load(std::memory_order_relaxed);
    r.shared = shared_.load(std::memory_order_relaxed);
    r.sumDb = sumDb_.load(std::memory_order_relaxed);
    r.invertedDb = invertedDb_.load(std::memory_order_relaxed);
    r.shiftMs = shiftMs_.load(std::memory_order_relaxed);
    r.shiftCorrelation = shiftCorrelation_.load(std::memory_order_relaxed);
    r.kickDb = kickDb_.load(std::memory_order_relaxed);
    r.bassDb = bassDb_.load(std::memory_order_relaxed);
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
    b.measured = b.signal && bandMeasured_[(size_t) index].load(std::memory_order_relaxed);
    return b;
}

Correlation::HistoryPoint Correlation::history(int newestFirst) const
{
    const uint32_t count = historyCount_.load(std::memory_order_acquire);
    if (newestFirst < 0 || newestFirst >= kHistory || (uint32_t) newestFirst >= count) return {};
    const int slot = ((historyHead_.load(std::memory_order_relaxed) - 1 - newestFirst) % kHistory + kHistory) % kHistory;
    const auto flags = historyFlags_[(size_t) slot].load(std::memory_order_relaxed);
    return {(flags & 1) != 0, (flags & 2) != 0, historyCorrelation_[(size_t) slot].load(std::memory_order_relaxed),
            historyLow_[(size_t) slot].load(std::memory_order_relaxed), historyShared_[(size_t) slot].load(std::memory_order_relaxed)};
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

int Correlation::beat(float* kick, float* bass, float* weight, int count, float preMs, float spanMs, float& msPerPoint, bool& fromOnset) const
{
    const double rate = pairRateShown_.load(std::memory_order_relaxed);
    msPerPoint = (float) (1000.0 / rate);
    fromOnset = false;
    const uint64_t total = beatWritten_.load(std::memory_order_acquire);
    if (total == 0 || count <= 0) return 0;
    if (spanMs <= 0) spanMs = (float) (60000.0 / std::max(1.0, tempo()));
    const uint64_t pre = (uint64_t) std::lround(std::max(0.0f, preMs) * 0.001 * rate);
    const uint64_t span = std::max<uint64_t>(1, (uint64_t) std::lround(spanMs * 0.001 * rate));
    const uint64_t length = std::min<uint64_t>({pre + span, (uint64_t) kBeatPoints});
    uint64_t start = total > length ? total - length : 0; // the newest span
    // The last onset whose beat is complete, still in the ring.
    const int onsets = onsets_.load(std::memory_order_acquire);
    const uint64_t candidates[2] = {onsetLatest_.load(std::memory_order_relaxed), onsetPrevious_.load(std::memory_order_relaxed)};
    for (int i = 0; i < onsets && i < 2; ++i)
    {
        const uint64_t onset = candidates[i];
        if (onset < pre || onset + span > total) continue;          // its beat is not complete yet
        if (total - (onset - pre) > (uint64_t) kBeatPoints) break;  // already overwritten
        start = onset - pre;
        fromOnset = true;
        break;
    }
    const uint64_t have = std::min<uint64_t>(length, total - start);
    // Several ring samples per point when the span holds more than `count`: the point keeps the largest magnitude.
    const int points = (int) std::min<uint64_t>(have, (uint64_t) count);
    if (points <= 0) return 0;
    const double per = (double) have / (double) points;
    msPerPoint = (float) (per * 1000.0 / rate);
    for (int p = 0; p < points; ++p)
    {
        const uint64_t from = start + (uint64_t) ((double) p * per), to = std::max(from + 1, start + (uint64_t) ((double) (p + 1) * per));
        float k = 0, b = 0, w = 0;
        for (uint64_t at = from; at < to; ++at)
        {
            const size_t slot = (size_t) (at % (uint64_t) kBeatPoints);
            const float kv = beatKick_[slot].load(std::memory_order_relaxed), bv = beatBass_[slot].load(std::memory_order_relaxed);
            if (std::abs(kv) > std::abs(k)) k = kv;
            if (std::abs(bv) > std::abs(b)) b = bv;
            w = std::max(w, beatWeight_[slot].load(std::memory_order_relaxed));
        }
        kick[p] = k;
        bass[p] = b;
        if (weight != nullptr) weight[p] = w;
    }
    return points;
}

std::vector<DeviceProcessor::UiGroup> Correlation::uiGroups() const
{
    return {{"Measure", {Mode, KickFrom}, 2}, {"Meter", {Integration, Sync, IntegrationNote, Hold}, 4},
            {"Scope", {ScopeMode, ScopeAuto, ScopeZoom}, 3}, {"Listen", {Listen}, 1}};
}
} // namespace sonora::devices

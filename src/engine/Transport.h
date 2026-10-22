#pragma once
// Transport state shared between UI (message thread) and the audio thread.
// UI calls the request methods; the audio thread consumes them at the start of each block
// in AudioEngine and publishes the current position.

#include <atomic>
#include <algorithm>
#include <cmath>
#include <functional>

namespace sonora::engine
{

class Transport
{
public:
    // COL-6: while a collab session shares the transport, play/stop and seeks during playback go through this hook
    // (message thread). It returns true when it took the request (the session starts everyone at a scheduled moment);
    // the session itself then acts with playNow/stopNow/seekNow.
    enum class Request { Play, Stop, Seek };
    std::function<bool(Request, double beat)> requestHook;

    // ---- message thread
    void play()
    {
        if (requestHook && requestHook(Request::Play, positionBeats())) return;
        playNow();
    }
    void stop()
    {
        if (requestHook && requestHook(Request::Stop, positionBeats())) return;
        stopNow();
    }
    void playNow() { playRequested_.store(true); }
    void stopNow() { stopRequested_.store(true); }
    void togglePlay() { isPlaying() || isCountingIn() ? stop() : play(); }
    void playWithCountIn(int bars)
    {
        const double beats = std::max(0, bars) * timeSigNumerator() * 4.0 / std::max(1, timeSigDenominator());
        countInRequested_.store(beats);
    }
    void cancelCountIn() { if (isCountingIn() || countInRequested_.load() >= 0) cancelCountInRequested_.store(true); }
    void setPosition(double beats)
    {
        if (isPlaying() && requestHook && requestHook(Request::Seek, beats)) return;
        seekNow(beats);
    }
    void seekNow(double beats)
    {
        seekTarget_.store(beats);
        seekRequested_.store(true);
    }
    void setTempo(double bpm) { tempo_.store(bpm < 20.0 ? 20.0 : bpm > 999.0 ? 999.0 : bpm); }
    void setLoop(bool on, double start, double length)
    {
        loopStart_.store(start);
        loopLength_.store(length > 0.0 ? length : 4.0);
        loopOn_.store(on);
    }
    void setLoopEnabled(bool on) { loopOn_.store(on); }
    void setMetronome(bool on) { metronome_.store(on); }
    void setTimeSignature(int numerator, int denominator)
    {
        sigNum_.store(numerator);
        sigDen_.store(denominator);
    }

    // ---- any thread
    bool isPlaying() const { return playing_.load(); }
    bool isCountingIn() const { return countInRemaining_.load() > 0; }
    double countInPosition() const { return -countInRemaining_.load(); }
    double positionBeats() const { return position_.load(); }
    double tempo() const { return tempo_.load(); }
    bool loopEnabled() const { return loopOn_.load(); }
    bool metronomeOn() const { return metronome_.load(); }
    int timeSigNumerator() const { return sigNum_.load(); }
    int timeSigDenominator() const { return sigDen_.load(); }
    double loopStart() const { return loopStart_.load(); }
    double loopLength() const { return loopLength_.load(); }

    // ---- audio thread
    struct BlockStart
    {
        bool playing = false;
        bool justStopped = false;
        bool jumped = false; // seek happened, all notes must be released
        double position = 0.0;
        int playOffset = 0; // first timeline sample, after count-in
        int countInSamples = 0;
        double countInStart = 0;
    };

    BlockStart beginBlock(int numSamples = 0, double samplesPerBeat = 1)
    {
        BlockStart b;
        if (seekRequested_.exchange(false))
        {
            position_.store(seekTarget_.load());
            b.jumped = true;
        }
        const double requestedCount = countInRequested_.exchange(-1);
        if (requestedCount >= 0 && ! playing_.load()) countInRemaining_.store(requestedCount);
        const bool cancelled = cancelCountInRequested_.exchange(false);
        const bool stopping = stopRequested_.exchange(false) || cancelled;
        if (stopping)
        {
            playing_.store(false);
            b.justStopped = true;
            countInRemaining_.store(0);
        }
        if (! stopping && requestedCount >= 0 && requestedCount <= 0) playing_.store(true);
        if (playRequested_.exchange(false) && ! stopping)
        {
            countInRemaining_.store(0);
            playing_.store(true);
        }
        if (isCountingIn() && numSamples > 0)
        {
            const double remaining = countInRemaining_.load();
            b.countInStart = -remaining;
            b.countInSamples = (int) std::ceil(std::clamp(remaining * samplesPerBeat - 1e-8, 0.0, (double) numSamples));
            const double left = std::max(0.0, remaining - b.countInSamples / samplesPerBeat);
            countInRemaining_.store(left < 1e-8 ? 0 : left);
            b.playOffset = b.countInSamples;
            if (! isCountingIn()) playing_.store(true);
        }
        b.playing = playing_.load();
        b.position = position_.load();
        return b;
    }

    void advanceTo(double beats) { position_.store(beats); }

private:
    std::atomic<double> countInRequested_{-1}, countInRemaining_{0};
    std::atomic<bool> cancelCountInRequested_{false};
    std::atomic<bool> playing_{false};
    std::atomic<bool> playRequested_{false}, stopRequested_{false}, seekRequested_{false};
    std::atomic<double> position_{0.0}, seekTarget_{0.0};
    std::atomic<double> tempo_{120.0};
    std::atomic<bool> loopOn_{false};
    std::atomic<bool> metronome_{false};
    std::atomic<int> sigNum_{4}, sigDen_{4};
    std::atomic<double> loopStart_{0.0}, loopLength_{16.0};
};

} // namespace sonora::engine

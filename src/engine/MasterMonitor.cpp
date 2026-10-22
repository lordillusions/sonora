#include "engine/MasterMonitor.h"

#include <algorithm>

namespace sonora::engine
{

MasterMonitor::MasterMonitor() : juce::Thread("Sonora monitor"), ringL_((size_t) kRingFrames, 0.0f), ringR_((size_t) kRingFrames, 0.0f)
{
    chunkL_.resize((size_t) kRingFrames);
    chunkR_.resize((size_t) kRingFrames);
}

MasterMonitor::~MasterMonitor()
{
    stopThread(2000);
}

void MasterMonitor::prepare(double sampleRate)
{
    std::lock_guard<std::mutex> lock(analysisMutex_);
    sampleRate_ = sampleRate;
    loudness_.prepare(sampleRate);
    stereo_.prepare(sampleRate);
    spectrum_.prepare(sampleRate);
    scope_.prepare(sampleRate);
    readCount_ = writeCount_.load(std::memory_order_acquire);
    integratedFrames_ = 0;
}

void MasterMonitor::setEnabled(bool on)
{
    if (enabled_.exchange(on) == on)
        return;
    if (on)
    {
        {
            std::lock_guard<std::mutex> lock(analysisMutex_);
            readCount_ = writeCount_.load(std::memory_order_acquire); // do not analyse stale audio
        }
        startThread();
    }
    else
        stopThread(2000);
}

void MasterMonitor::run()
{
    while (! threadShouldExit())
    {
        drain();
        const double now = juce::Time::getMillisecondCounterHiRes();
        if (now - lastPublishMs_ >= 16.0)
        {
            lastPublishMs_ = now;
            publish();
        }
        wait(6);
    }
}

void MasterMonitor::drain()
{
    std::lock_guard<std::mutex> lock(analysisMutex_);
    if (resetRequested_.exchange(false))
    {
        loudness_.reset();
        integratedFrames_ = 0;
    }
    const uint64_t write = writeCount_.load(std::memory_order_acquire);
    if (write - readCount_ > kRingFrames)
    {
        dropped_ += write - readCount_ - kRingFrames;
        readCount_ = write - kRingFrames;
    }
    const int count = (int) (write - readCount_);
    if (count <= 0)
        return;
    for (int i = 0; i < count; ++i)
    {
        chunkL_[(size_t) i] = ringL_[(size_t) ((readCount_ + (uint64_t) i) & kRingMask)];
        chunkR_[(size_t) i] = ringR_[(size_t) ((readCount_ + (uint64_t) i) & kRingMask)];
    }
    readCount_ = write;
    loudness_.process(chunkL_.data(), chunkR_.data(), count);
    stereo_.process(chunkL_.data(), chunkR_.data(), count);
    spectrum_.push(chunkL_.data(), chunkR_.data(), count);
    scope_.push(chunkL_.data(), chunkR_.data(), count);
    integratedFrames_ += count;
}

void MasterMonitor::publish()
{
    std::lock_guard<std::mutex> lock(analysisMutex_);
    const double now = juce::Time::getMillisecondCounterHiRes();
    if (lastSpectrumMs_ == 0.0)
        lastSpectrumMs_ = now - 33.0;
    spectrum_.analyze(std::clamp((now - lastSpectrumMs_) * 0.001, 0.001, 0.25));
    lastSpectrumMs_ = now;

    MonitorSnapshot s;
    s.sampleRate = sampleRate_;
    s.momentaryLufs = loudness_.momentary();
    s.shortTermLufs = loudness_.shortTerm();
    s.integratedLufs = loudness_.integrated();
    s.momentaryMaxLufs = loudness_.momentaryMax();
    s.loudnessRange = loudness_.loudnessRange();
    s.samplePeakDb = loudness_.samplePeakDb();
    s.truePeakDb = loudness_.truePeakDb();
    s.correlation = stereo_.correlation();
    s.balance = stereo_.balance();
    s.width = stereo_.width();
    s.bars = spectrum_.bars();
    s.barPeaks = spectrum_.barPeaks();
    s.curve = spectrum_.curve();
    stereo_.goniometer(s.goniometer);
    scope_.trace(kScopeColumns, scopeWindowMs_.load(), scopeTriggered_.load(), s.scope);
    s.integratedSeconds = integratedFrames_ / std::max(1.0, sampleRate_);
    s.droppedFrames = dropped_;

    std::lock_guard<std::mutex> snapshotLock(snapshotMutex_);
    s.serial = ++serial_;
    snapshot_ = std::move(s);
}

MonitorSnapshot MasterMonitor::snapshot() const
{
    std::lock_guard<std::mutex> lock(snapshotMutex_);
    return snapshot_;
}

} // namespace sonora::engine

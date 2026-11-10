#include "engine/EngineWorkers.h"

#include <algorithm>
#include <thread>

#if JUCE_MAC
 #include <sys/sysctl.h>
#endif
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
 #include <immintrin.h>
#endif

namespace sonora::engine
{

namespace
{
inline void cpuPause() noexcept
{
#if defined(__aarch64__) || defined(_M_ARM64)
 #if defined(_MSC_VER) && ! defined(__clang__)
    __yield();
 #else
    __asm__ __volatile__("yield");
 #endif
#elif defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
    _mm_pause();
#else
    std::this_thread::yield();
#endif
}
} // namespace

class EngineWorkers::Worker final : public juce::Thread
{
public:
    Worker(EngineWorkers& owner, int index, const juce::AudioWorkgroup& workgroup)
        : juce::Thread("Sonora engine " + juce::String(index + 1), 8 * 1024 * 1024), // devices with big stack frames (the 0.1.28 Wavetable crash)
          owner_(owner), workgroup_(workgroup)
    {
    }

    void run() override
    {
        juce::WorkgroupToken token;
        if (workgroup_)
        {
            workgroup_.join(token);
            if (token) owner_.joined_.fetch_add(1, std::memory_order_relaxed);
        }
        for (;;)
        {
            owner_.sleeping_.fetch_add(1, std::memory_order_seq_cst);
            owner_.wake_.acquire();
            owner_.sleeping_.fetch_sub(1, std::memory_order_seq_cst);
            if (threadShouldExit()) break;
            owner_.inside_.fetch_add(1, std::memory_order_seq_cst);
            if (auto* batch = owner_.current_.load(std::memory_order_seq_cst))
            {
                const juce::ScopedNoDenormals noDenormals; // same float mode as the callback thread: same results
                owner_.help(*batch, true);
            }
            owner_.inside_.fetch_sub(1, std::memory_order_seq_cst);
        }
        if (token) owner_.joined_.fetch_sub(1, std::memory_order_relaxed);
    }

private:
    EngineWorkers& owner_;
    juce::AudioWorkgroup workgroup_;
};

EngineWorkers::EngineWorkers() = default;
EngineWorkers::~EngineWorkers() { stop(); }

int EngineWorkers::defaultCount()
{
    int cores = 0;
#if JUCE_MAC
    int value = 0;
    size_t size = sizeof(value);
    if (sysctlbyname("hw.perflevel0.physicalcpu", &value, &size, nullptr, 0) == 0 && value > 0) cores = value;
#endif
    if (cores <= 0) cores = (int) std::thread::hardware_concurrency();
    return std::clamp(cores - 1, 0, 15);
}

void EngineWorkers::start(int count, double sampleRate, int blockSize, const juce::AudioWorkgroup& workgroup)
{
    enabled_.store(false, std::memory_order_seq_cst);
    while (inRun_.load(std::memory_order_seq_cst) != 0) // a block that already saw enabled_ finishes with its workers
        std::this_thread::yield();
    for (auto& w : workers_) w->signalThreadShouldExit();
    for (size_t i = 0; i < workers_.size(); ++i) wake_.release();
    for (auto& w : workers_) w->stopThread(2000);
    workers_.clear();
    while (wake_.try_acquire()) {} // tokens nobody consumed
    count = std::clamp(count, 0, 15);
    if (workgroup)
        if (const auto max = (int) workgroup.getMaxParallelThreadCount(); max > 1) count = std::min(count, max - 1);
    auto options = juce::Thread::RealtimeOptions{}.withPriority(10);
    if (sampleRate > 0 && blockSize > 0) options = options.withApproximateAudioProcessingTime(blockSize, sampleRate);
    for (int i = 0; i < count; ++i)
    {
        workers_.push_back(std::make_unique<Worker>(*this, i, workgroup));
        if (! workers_.back()->startRealtimeThread(options) && ! workers_.back()->startThread(juce::Thread::Priority::highest))
            workers_.pop_back();
    }
    count_.store((int) workers_.size(), std::memory_order_relaxed);
    enabled_.store(! workers_.empty(), std::memory_order_seq_cst);
}

void EngineWorkers::push(Batch& b, int job)
{
    const int slot = b.tail.fetch_add(1, std::memory_order_seq_cst);
    b.schedule->queue[(size_t) slot].store(job, std::memory_order_release);
    if (idleWaiters_.load(std::memory_order_seq_cst) > 0) idle_.release(); // a helper parked in help() takes it
}

int EngineWorkers::pop(Batch& b)
{
    int h = b.head.load(std::memory_order_relaxed);
    for (;;)
    {
        if (h >= b.tail.load(std::memory_order_acquire)) return -1;
        if (b.head.compare_exchange_weak(h, h + 1, std::memory_order_acq_rel, std::memory_order_relaxed)) break;
    }
    int job;
    while ((job = b.schedule->queue[(size_t) h].load(std::memory_order_acquire)) < 0) cpuPause(); // reserved, not stored yet
    return job;
}

void EngineWorkers::help(Batch& b, bool mayPark)
{
    bool counted = false;
    const auto spinTicks = juce::Time::secondsToHighResolutionTicks(30.0e-6);
    int64_t idleSince = -1;
    int spins = 0;
    while (b.remaining.load(std::memory_order_acquire) > 0)
    {
        const int job = pop(b);
        if (job < 0)
        {
            cpuPause();
            // A helper that finds nothing to take for 30 us parks until a job becomes ready or the block ends: realtime
            // threads that spin through every block get throttled by the scheduler (they stalled for whole buffers).
            // The render thread never parks, so the block returns as soon as its last job ends.
            if (! mayPark || (++spins & 63) != 0) continue;
            const auto now = juce::Time::getHighResolutionTicks();
            if (idleSince < 0) idleSince = now;
            if (now - idleSince < spinTicks) continue;
            idleWaiters_.fetch_add(1, std::memory_order_seq_cst);
            if (b.head.load(std::memory_order_seq_cst) >= b.tail.load(std::memory_order_seq_cst)
                && b.remaining.load(std::memory_order_seq_cst) > 0)
                idle_.acquire(); // released by push() or by the end of the block
            idleWaiters_.fetch_sub(1, std::memory_order_seq_cst);
            idleSince = -1;
            continue;
        }
        idleSince = -1;
        if (! counted)
        {
            b.participants.fetch_add(1, std::memory_order_relaxed);
            counted = true;
        }
        b.fn(b.context, job);
        auto& schedule = *b.schedule;
        for (const int next : schedule.jobs[(size_t) job].successors)
            if (schedule.pending[(size_t) next].fetch_sub(1, std::memory_order_acq_rel) == 1) push(b, next);
        if (b.remaining.fetch_sub(1, std::memory_order_seq_cst) == 1) // the block is done: unpark every helper
            if (const int parked = idleWaiters_.load(std::memory_order_seq_cst); parked > 0) idle_.release(parked);
    }
}

int EngineWorkers::run(RenderSchedule& schedule, JobFunction fn, void* context, int helpers)
{
    const int numJobs = (int) schedule.jobs.size();
    if (numJobs == 0) return 1;
    inRun_.fetch_add(1, std::memory_order_seq_cst);
    const int available = enabled_.load(std::memory_order_seq_cst) ? count_.load(std::memory_order_relaxed) : 0;
    helpers = std::clamp(helpers, 0, available);

    while (idle_.try_acquire()) {} // tokens left by earlier blocks (no helper is inside help() between blocks)
    auto& b = batch_;
    b.schedule = &schedule;
    b.fn = fn;
    b.context = context;
    b.head.store(0, std::memory_order_relaxed);
    b.tail.store(0, std::memory_order_relaxed);
    b.participants.store(0, std::memory_order_relaxed);
    b.remaining.store(numJobs, std::memory_order_relaxed);
    for (int j = 0; j < numJobs; ++j)
    {
        schedule.pending[(size_t) j].store(schedule.jobs[(size_t) j].dependencies, std::memory_order_relaxed);
        schedule.queue[(size_t) j].store(-1, std::memory_order_relaxed);
    }
    for (int j = 0; j < numJobs; ++j)
        if (schedule.jobs[(size_t) j].dependencies == 0) push(b, j);

    if (helpers > 0)
    {
        current_.store(&b, std::memory_order_seq_cst);
        // Only sleepers need a token; one that is awake already finds the batch. Keeps the semaphore count bounded.
        if (const int wake = std::min(helpers, sleeping_.load(std::memory_order_seq_cst)); wake > 0) wake_.release(wake);
    }
    help(b, false);
    if (helpers > 0)
    {
        current_.store(nullptr, std::memory_order_seq_cst);
        while (inside_.load(std::memory_order_seq_cst) != 0) cpuPause(); // a worker may still be leaving help()
    }
    const int participants = std::max(1, b.participants.load(std::memory_order_relaxed));
    inRun_.fetch_sub(1, std::memory_order_seq_cst);
    return participants;
}

} // namespace sonora::engine

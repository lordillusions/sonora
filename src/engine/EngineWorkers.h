#pragma once
// ENG-14e: realtime helper threads that render the jobs of a RenderSchedule together with the audio callback thread.
//
// start()/stop() on the message thread. run() on the thread that renders the block (the CoreAudio callback, or the
// offline render worker); it never allocates, locks or logs:
//   - the schedule's counters are reset and the jobs without dependencies are queued,
//   - `helpers` sleeping workers are woken (a semaphore post each), the calling thread works too,
//   - a finished job releases its successors (atomic countdown), threads take ready jobs from a lock-free queue; a
//     helper with nothing to take spins 30 us, then parks on a semaphore that push() and the block's end release,
//   - run() returns once every job is done and no worker is still looking at this block.
// On macOS the workers are time-constraint threads that join the audio device's os_workgroup when it has one, so the
// scheduler treats them like the IO thread (P-cores on Apple Silicon).

#include "engine/RenderSchedule.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>
#include <memory>
#include <semaphore>
#include <vector>

namespace sonora::engine
{

class EngineWorkers
{
public:
    using JobFunction = void (*)(void* context, int job);

    EngineWorkers();
    ~EngineWorkers();
    EngineWorkers(const EngineWorkers&) = delete;
    EngineWorkers& operator=(const EngineWorkers&) = delete;

    // Message thread. (Re)starts `count` helper threads (0 stops them). A block being rendered meanwhile finishes first.
    void start(int count, double sampleRate, int blockSize, const juce::AudioWorkgroup& workgroup = {});
    void stop() { start(0, 0, 0); }
    int count() const { return count_.load(std::memory_order_relaxed); }
    bool joinedWorkgroup() const { return joined_.load(std::memory_order_relaxed) > 0; }

    // Render thread. Runs fn(context, job) for every job in dependency order with up to `helpers` workers.
    // Returns how many threads took part (1 = the caller alone).
    int run(RenderSchedule& schedule, JobFunction fn, void* context, int helpers);

    // Helper threads this machine should use: performance cores - 1 (macOS hw.perflevel0.physicalcpu), else
    // hardware_concurrency - 1, capped at 15.
    static int defaultCount();

private:
    struct Batch
    {
        RenderSchedule* schedule = nullptr;
        JobFunction fn = nullptr;
        void* context = nullptr;
        std::atomic<int> head{0}, tail{0}, remaining{0}, participants{0};
    };
    class Worker;
    void help(Batch& b, bool mayPark);
    static int pop(Batch& b);
    void push(Batch& b, int job);

    Batch batch_;
    std::atomic<Batch*> current_{nullptr};
    std::atomic<int> inside_{0};   // workers between "woke up" and "done with the batch"
    std::atomic<int> inRun_{0};    // render threads inside run() (start() waits for 0)
    std::atomic<bool> enabled_{false};
    std::atomic<int> count_{0}, joined_{0}, sleeping_{0};
    std::counting_semaphore<> wake_{0};
    std::counting_semaphore<> idle_{0}; // parked helpers inside a block
    std::atomic<int> idleWaiters_{0};
    std::vector<std::unique_ptr<Worker>> workers_;
};

} // namespace sonora::engine

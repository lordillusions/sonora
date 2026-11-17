#pragma once
// UI-111: when the message thread stops answering for a while (the owner force-quit a frozen 0.1.48 after deleting a
// duplicated track, and no autotest reproduced it), a background thread records every thread's stack with macOS's
// `sample` into <data folder>/Freezes, so the next freeze can be fixed from the file. Keeps the five newest.
#include <juce_core/juce_core.h>

#include <atomic>

namespace sonora::app
{

class FreezeRecorder : private juce::Thread
{
public:
    explicit FreezeRecorder(juce::File folder, int stallMilliseconds = 8000);
    ~FreezeRecorder() override;

    // Message thread, a few times a second: the app is alive.
    void beat() { last_.store(juce::Time::getMillisecondCounter(), std::memory_order_relaxed); }
    // The newest recording, or none (tests).
    juce::File latest() const;

private:
    void run() override;
    void record();

    juce::File folder_;
    juce::uint32 stallMs_;
    std::atomic<juce::uint32> last_;
};

} // namespace sonora::app

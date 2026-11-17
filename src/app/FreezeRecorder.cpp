#include "app/FreezeRecorder.h"

#include <algorithm>
#include <cstdio>
#if JUCE_MAC
 #include <unistd.h>
#endif

namespace sonora::app
{

FreezeRecorder::FreezeRecorder(juce::File folder, int stallMilliseconds)
    : juce::Thread("Sonora freeze recorder"), folder_(std::move(folder)), stallMs_((juce::uint32) stallMilliseconds),
      last_(juce::Time::getMillisecondCounter())
{
#if JUCE_MAC
    startThread(juce::Thread::Priority::low);
#endif
}

FreezeRecorder::~FreezeRecorder()
{
    stopThread(25000); // a recording in progress takes about 3 s
}

juce::File FreezeRecorder::latest() const
{
    auto files = folder_.findChildFiles(juce::File::findFiles, false, "Freeze *.txt");
    juce::File newest;
    for (const auto& f : files)
        if (newest == juce::File() || f.getLastModificationTime() > newest.getLastModificationTime()) newest = f;
    return newest;
}

void FreezeRecorder::run()
{
    bool recorded = false; // once per freeze
    while (! threadShouldExit())
    {
        wait(500);
        const auto stalled = juce::Time::getMillisecondCounter() - last_.load(std::memory_order_relaxed);
        if (stalled < stallMs_) recorded = false;
        else if (! recorded)
        {
            recorded = true;
            record();
        }
    }
}

void FreezeRecorder::record()
{
#if JUCE_MAC // TODO(UI-111): Windows (MiniDumpWriteDump) and Linux (gdb/eu-stack) recorders
    if (! folder_.createDirectory()) return;
    const auto stamp = juce::Time::getCurrentTime().formatted("%Y-%m-%d %H-%M-%S");
    const auto file = folder_.getChildFile("Freeze " + stamp + ".txt");
    std::fprintf(stderr, "[sonora] the app has not answered for %u s: recording its threads to %s\n", (unsigned) (stallMs_ / 1000),
                 file.getFullPathName().toRawUTF8());
    juce::ChildProcess sample;
    if (sample.start(juce::StringArray{"/usr/bin/sample", juce::String((int) getpid()), "3", "-mayDie", "-file", file.getFullPathName()},
                     0))
        sample.waitForProcessToFinish(20000);
    auto files = folder_.findChildFiles(juce::File::findFiles, false, "Freeze *.txt");
    std::sort(files.begin(), files.end(), [](const juce::File& a, const juce::File& b) { return a.getFileName() > b.getFileName(); });
    for (int i = 5; i < files.size(); ++i) files.getReference(i).deleteFile();
#endif
}

} // namespace sonora::app

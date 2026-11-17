// UI-111: a message thread that stops beating gets its threads recorded once into the Freezes folder; beating again
// re-arms it.
#include "TestFramework.h"
#include "app/FreezeRecorder.h"

TEST_CASE("freeze recorder: a stalled app records its threads once, into the folder it was given (UI-111)")
{
#if JUCE_MAC
    const auto folder = juce::File::createTempFile("sonora-freezes");
    {
        sonora::app::FreezeRecorder recorder(folder, 300);
        for (int i = 0; i < 6; ++i) { recorder.beat(); juce::Thread::sleep(100); }
        CHECK(! recorder.latest().existsAsFile()); // beating: nothing
        // Stalled: one recording (sample runs ~3 s), not one per check.
        for (int i = 0; i < 160 && ! recorder.latest().existsAsFile(); ++i) juce::Thread::sleep(100);
        juce::Thread::sleep(5000);
        const auto file = recorder.latest();
        REQUIRE(file.existsAsFile());
        CHECK(folder.findChildFiles(juce::File::findFiles, false, "Freeze *.txt").size() == 1);
        const auto text = file.loadFileAsString();
        CHECK(text.contains("Call graph") || text.contains("Thread_"));
    }
    folder.deleteRecursively();
#endif
}

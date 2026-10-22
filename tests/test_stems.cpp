// Stem separation by an external tool: command lines, tool detection, running a (fake) tool, collecting its files,
// failures and cancel. The real separators (audio-separator, demucs) are not needed to run these.

#include "TestFramework.h"
#include "engine/StemSeparator.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <thread>

using namespace sonora;

namespace
{
[[maybe_unused]] juce::File freshDir(const char* name)
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_stem_tests").getChildFile(name);
    dir.deleteRecursively();
    dir.createDirectory();
    return dir;
}

[[maybe_unused]] juce::File writeWav(const juce::File& file)
{
    file.getParentDirectory().createDirectory();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream(file.createOutputStream());
    auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}.withSampleRate(44100.0).withNumChannels(2).withBitsPerSample(16));
    REQUIRE(writer != nullptr);
    juce::AudioBuffer<float> b(2, 441);
    for (int i = 0; i < 441; ++i) { b.setSample(0, i, 0.25f); b.setSample(1, i, -0.25f); }
    writer->writeFromAudioSampleBuffer(b, 0, 441);
    return file;
}

[[maybe_unused]] juce::File script(const juce::File& dir, const char* name, const juce::String& body)
{
    const auto f = dir.getChildFile(name);
    f.replaceWithText("#!/bin/sh\n" + body + "\n", false, false, "\n"); // JUCE would write CRLF by default
    REQUIRE(f.setExecutePermission(true));
    return f;
}
} // namespace

TEST_CASE("stems: stem names come from both tools' file naming")
{
    CHECK(engine::StemSeparator::stemNameFromFile(juce::File("/x/My Song_(Vocals)_model_name.wav")) == "Vocals");
    CHECK(engine::StemSeparator::stemNameFromFile(juce::File("/x/song_(Instrumental)_UVR-MDX-NET-Inst_HQ_3.wav")) == "Instrumental");
    CHECK(engine::StemSeparator::stemNameFromFile(juce::File("/x/htdemucs/song/drums.wav")) == "Drums");
}

TEST_CASE("stems: command lines for audio-separator (reusing UVR's models) and demucs")
{
    // Real absolute paths on every OS (juce::File("/opt/...") is not absolute on Windows, COM-7).
    const auto root = juce::File::getSpecialLocation(juce::File::tempDirectory);
    const auto input = root.getChildFile("in/song.wav"), output = root.getChildFile("out");
    engine::StemTool a;
    a.kind = engine::StemTool::Kind::AudioSeparator;
    a.executable = root.getChildFile("opt/tools/audio-separator");
    a.modelFolder = root.getChildFile("Apps/UVR/models");
    a.model = "UVR-MDX-NET-Inst_HQ_3.onnx";
    const auto cmd = engine::StemSeparator::commandLine(a, input, output);
    CHECK(cmd[0] == a.executable.getFullPathName());
    CHECK(cmd[1] == input.getFullPathName());
    CHECK(cmd.contains("--model_file_dir"));
    CHECK(cmd[cmd.indexOf("--model_filename") + 1] == "UVR-MDX-NET-Inst_HQ_3.onnx");
    CHECK(cmd[cmd.indexOf("--output_dir") + 1] == output.getFullPathName());

    engine::StemTool d;
    d.kind = engine::StemTool::Kind::Demucs;
    d.executable = root.getChildFile("opt/tools/demucs");
    d.model = "htdemucs";
    const auto dc = engine::StemSeparator::commandLine(d, input, output);
    CHECK(dc[dc.indexOf("-n") + 1] == "htdemucs");
    CHECK(dc[dc.size() - 1] == input.getFullPathName());
    CHECK(! dc.contains("--model_file_dir"));
}

#if ! defined(_WIN32) // the fake tools below are /bin/sh scripts (COM-7)
TEST_CASE("stems: installed tools are found in the given directories, audio-separator first")
{
    const auto dir = freshDir("detect");
    CHECK(engine::StemSeparator::detectTools({dir}).size() >= 0); // whatever the machine has
    script(dir, "audio-separator", "exit 0");
    script(dir, "demucs", "exit 0");
    const auto tools = engine::StemSeparator::detectTools({dir});
    REQUIRE(tools.size() >= 2);
    CHECK(tools[0].kind == engine::StemTool::Kind::AudioSeparator);
    CHECK(tools[0].executable == dir.getChildFile("audio-separator"));
    CHECK(! tools[0].label.empty());
    bool demucs = false;
    for (const auto& t : tools) demucs |= t.kind == engine::StemTool::Kind::Demucs && t.executable == dir.getChildFile("demucs");
    CHECK(demucs);
}

TEST_CASE("stems: a demucs-style tool runs, its files are collected, sorted and named")
{
    const auto dir = freshDir("demucs");
    const auto input = writeWav(dir.getChildFile("in/Song.wav"));
    const auto source = writeWav(dir.getChildFile("source.wav"));
    // demucs layout: <out>/htdemucs/<song>/{drums,bass,other,vocals}.wav; arguments: -n M --int24 -o OUT INPUT
    const auto tool = script(dir, "demucs",
                             "out=$5\nmkdir -p \"$out/htdemucs/Song\"\necho \"Separating $6\"\n"
                             "for s in drums bass other vocals; do cp \"" + source.getFullPathName() + "\" \"$out/htdemucs/Song/$s.wav\"; done");
    engine::StemTool t;
    t.kind = engine::StemTool::Kind::Demucs;
    t.executable = tool;
    t.model = "htdemucs";
    std::atomic<bool> cancel{false};
    std::vector<std::string> lines;
    const auto r = engine::StemSeparator::separate(t, input, dir.getChildFile("out"), cancel, [&](const std::string& l) { lines.push_back(l); });
    if (! r.ok) std::printf("    error: %s\n", r.error.c_str());
    REQUIRE(r.ok);
    REQUIRE(r.stems.size() == 4);
    CHECK(r.stems[0].name == "Vocals");
    CHECK(r.stems[1].name == "Drums");
    CHECK(r.stems[2].name == "Bass");
    CHECK(r.stems[3].name == "Other");
    CHECK(r.stems[0].file.existsAsFile());
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].find("Separating") == 0);
}

TEST_CASE("stems: an audio-separator-style tool with UVR's model folder, a failing tool and a missing input report clearly")
{
    const auto dir = freshDir("sep");
    const auto input = writeWav(dir.getChildFile("in/Track One.wav"));
    const auto source = writeWav(dir.getChildFile("source.wav"));
    // arguments: INPUT --model_filename M --model_file_dir D --output_dir OUT --output_format WAV
    const auto tool = script(dir, "audio-separator",
                             "out=$7\ncp \"" + source.getFullPathName() + "\" \"$out/Track One_(Vocals)_model.wav\"\n"
                             "cp \"" + source.getFullPathName() + "\" \"$out/Track One_(Instrumental)_model.wav\"");
    engine::StemTool t;
    t.kind = engine::StemTool::Kind::AudioSeparator;
    t.executable = tool;
    t.modelFolder = dir;
    t.model = "m.onnx";
    std::atomic<bool> cancel{false};
    auto r = engine::StemSeparator::separate(t, input, dir.getChildFile("out"), cancel);
    REQUIRE(r.ok);
    REQUIRE(r.stems.size() == 2);
    CHECK(r.stems[0].name == "Vocals");
    CHECK(r.stems[1].name == "Instrumental");

    t.executable = script(dir, "broken", "echo 'CUDA out of memory' 1>&2\nexit 3");
    r = engine::StemSeparator::separate(t, input, dir.getChildFile("out2"), cancel);
    CHECK(! r.ok);
    CHECK(r.error.find("exit 3") != std::string::npos);
    CHECK(r.error.find("CUDA out of memory") != std::string::npos);

    t.executable = script(dir, "silent", "exit 0");
    r = engine::StemSeparator::separate(t, input, dir.getChildFile("out3"), cancel);
    CHECK(! r.ok);
    CHECK(r.error.find("no audio files") != std::string::npos);

    r = engine::StemSeparator::separate(t, dir.getChildFile("missing.wav"), dir.getChildFile("out4"), cancel);
    CHECK(! r.ok);
    t.executable = dir.getChildFile("nothing-here");
    CHECK(! engine::StemSeparator::separate(t, input, dir.getChildFile("out5"), cancel).ok);
}

TEST_CASE("stems: cancelling kills the tool promptly")
{
    const auto dir = freshDir("cancel");
    const auto input = writeWav(dir.getChildFile("in.wav"));
    engine::StemTool t;
    t.kind = engine::StemTool::Kind::Demucs;
    t.executable = script(dir, "slow", "sleep 30");
    std::atomic<bool> cancel{false};
    std::thread canceller([&] { juce::Thread::sleep(300); cancel = true; });
    const auto start = juce::Time::getMillisecondCounterHiRes();
    const auto r = engine::StemSeparator::separate(t, input, dir.getChildFile("out"), cancel);
    canceller.join();
    CHECK(r.cancelled);
    CHECK(juce::Time::getMillisecondCounterHiRes() - start < 5000.0);
}
#endif

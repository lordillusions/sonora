// fx_render: run any VST3/AU effect over a WAV file, offline, with parameters set by name. Black-box measurements
// (PLG-14 resonance suppressor parity) and A/B renders of Sonora's own plug-ins against a reference use it.
//
//   fx_render <plugin> --params                       list parameters: index, name, default, steps, text at 0/.25/.5/.75/1
//   fx_render <plugin> --editor-size                  the editor's default size in points
//   fx_render <plugin> --in in.wav --out out.wav [--sc key.wav] [--set "Name=0.5"] [--text "Name=12 dB"]
//             [--rate 48000] [--block 512] [--state-in s.bin] [--state-out s.bin]
//
// --set takes a normalised value, --text the plug-in's own text for a value. Names match exactly, else case-insensitively.
// Output is 32-bit float at the input's channel count (stereo when the input is mono). The plug-in's latency is printed
// ("latency N" after prepare, "latency_end N" after the render) and NOT compensated. With --sc the plug-in gets a stereo sidechain bus when it offers one.

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

#include <cstdio>
#include <memory>

namespace
{
juce::AudioProcessorParameter* findParameter(juce::AudioPluginInstance& p, const juce::String& name)
{
    for (auto* prm : p.getParameters())
        if (prm->getName(256) == name) return prm;
    for (auto* prm : p.getParameters())
        if (prm->getName(256).equalsIgnoreCase(name)) return prm;
    return nullptr;
}

std::unique_ptr<juce::AudioBuffer<float>> readWav(const juce::File& file, double& rate)
{
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (reader == nullptr) return nullptr;
    rate = reader->sampleRate;
    auto buffer = std::make_unique<juce::AudioBuffer<float>>((int) reader->numChannels, (int) reader->lengthInSamples);
    reader->read(buffer.get(), 0, (int) reader->lengthInSamples, 0, true, true);
    return buffer;
}

int run(int argc, char** argv)
{
    if (argc < 3)
    {
        std::fprintf(stderr, "usage: fx_render <plugin> --params | --in in.wav --out out.wav [options]\n");
        return 2;
    }
    const juce::String pluginPath = juce::File::getCurrentWorkingDirectory().getChildFile(argv[1]).getFullPathName();
    juce::String inPath, outPath, scPath, stateIn, stateOut;
    juce::StringArray sets, texts;
    bool listParams = false, editorSize = false;
    double rate = 48000.0;
    int block = 512;
    for (int i = 2; i < argc; ++i)
    {
        const juce::String a = argv[i];
        const auto next = [&]() -> juce::String { return i + 1 < argc ? juce::String(argv[++i]) : juce::String(); };
        if (a == "--params") listParams = true;
        else if (a == "--editor-size") editorSize = true;
        else if (a == "--in") inPath = next();
        else if (a == "--out") outPath = next();
        else if (a == "--sc") scPath = next();
        else if (a == "--set") sets.add(next());
        else if (a == "--text") texts.add(next());
        else if (a == "--rate") rate = next().getDoubleValue();
        else if (a == "--block") block = juce::jmax(16, next().getIntValue());
        else if (a == "--state-in") stateIn = next();
        else if (a == "--state-out") stateOut = next();
        else
        {
            std::fprintf(stderr, "unknown option %s\n", a.toRawUTF8());
            return 2;
        }
    }

    juce::AudioPluginFormatManager formats;
    juce::addDefaultFormatsToManager(formats);
    juce::OwnedArray<juce::PluginDescription> found;
    for (auto* format : formats.getFormats())
        if (format->fileMightContainThisPluginType(pluginPath)) format->findAllTypesForFile(found, pluginPath);
    if (found.isEmpty())
    {
        std::fprintf(stderr, "no plug-in in %s\n", pluginPath.toRawUTF8());
        return 1;
    }
    const auto& description = *found.getFirst();

    std::unique_ptr<juce::AudioBuffer<float>> input, key;
    if (inPath.isNotEmpty())
    {
        input = readWav(juce::File::getCurrentWorkingDirectory().getChildFile(inPath), rate);
        if (input == nullptr)
        {
            std::fprintf(stderr, "cannot read %s\n", inPath.toRawUTF8());
            return 1;
        }
    }
    if (scPath.isNotEmpty())
    {
        double scRate = rate;
        key = readWav(juce::File::getCurrentWorkingDirectory().getChildFile(scPath), scRate);
        if (key == nullptr) return 1;
    }

    juce::String error;
    auto plugin = formats.createPluginInstance(description, rate, block, error);
    if (plugin == nullptr)
    {
        std::fprintf(stderr, "cannot instantiate: %s\n", error.toRawUTF8());
        return 1;
    }

    // Stereo main bus; stereo sidechain when asked for and offered.
    auto layout = plugin->getBusesLayout();
    for (auto& set : layout.inputBuses) set = juce::AudioChannelSet::disabled();
    if (! layout.inputBuses.isEmpty()) layout.inputBuses.getReference(0) = juce::AudioChannelSet::stereo();
    if (key != nullptr && layout.inputBuses.size() > 1) layout.inputBuses.getReference(1) = juce::AudioChannelSet::stereo();
    for (auto& set : layout.outputBuses) set = juce::AudioChannelSet::disabled();
    if (! layout.outputBuses.isEmpty()) layout.outputBuses.getReference(0) = juce::AudioChannelSet::stereo();
    if (! plugin->setBusesLayout(layout))
        std::fprintf(stderr, "note: layout with %d inputs refused, keeping the default\n", layout.inputBuses.size());

    if (stateIn.isNotEmpty())
    {
        juce::MemoryBlock state;
        juce::File(stateIn).loadFileAsData(state);
        plugin->setStateInformation(state.getData(), (int) state.getSize());
    }
    for (const auto& s : sets)
        if (auto* p = findParameter(*plugin, s.upToLastOccurrenceOf("=", false, false)))
            p->setValueNotifyingHost(s.fromLastOccurrenceOf("=", false, false).getFloatValue());
        else
            std::fprintf(stderr, "no parameter %s\n", s.toRawUTF8());
    for (const auto& s : texts)
        if (auto* p = findParameter(*plugin, s.upToFirstOccurrenceOf("=", false, false)))
            p->setValueNotifyingHost(p->getValueForText(s.fromFirstOccurrenceOf("=", false, false)));
        else
            std::fprintf(stderr, "no parameter %s\n", s.toRawUTF8());

    if (editorSize)
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor(plugin->createEditorAndMakeActive());
        if (editor == nullptr) { std::fprintf(stderr, "no editor\n"); return 1; }
        std::printf("editor %d x %d resizable %d\n", editor->getWidth(), editor->getHeight(), editor->isResizable() ? 1 : 0);
        plugin->editorBeingDeleted(editor.get());
        return 0;
    }
    if (listParams)
    {
        std::printf("plugin %s | %s | latency %d | inputs %d outputs %d\n", description.name.toRawUTF8(),
                    description.manufacturerName.toRawUTF8(), plugin->getLatencySamples(), plugin->getTotalNumInputChannels(),
                    plugin->getTotalNumOutputChannels());
        int index = 0;
        for (auto* p : plugin->getParameters())
        {
            const float current = p->getValue();
            std::printf("%d\t%s\tvalue=%.6f '%s'\tdefault=%.6f\tsteps=%d\tlabel='%s'\t|", index++, p->getName(256).toRawUTF8(),
                        current, p->getCurrentValueAsText().toRawUTF8(), p->getDefaultValue(), p->getNumSteps(),
                        p->getLabel().toRawUTF8());
            for (float v : {0.0f, 0.1f, 0.25f, 0.5f, 0.75f, 0.9f, 1.0f}) std::printf(" %s", p->getText(v, 64).toRawUTF8());
            std::printf("\n");
        }
        return 0;
    }
    if (input == nullptr || outPath.isEmpty())
    {
        std::fprintf(stderr, "--in and --out are needed to render\n");
        return 2;
    }

    plugin->prepareToPlay(rate, block);
    std::fprintf(stderr, "latency %d\n", plugin->getLatencySamples());
    const int channels = juce::jmax(2, plugin->getTotalNumInputChannels(), plugin->getTotalNumOutputChannels());
    const int length = input->getNumSamples();
    juce::AudioBuffer<float> out(2, length), work(channels, block);
    juce::MidiBuffer midi;
    for (int start = 0; start < length; start += block)
    {
        const int n = juce::jmin(block, length - start);
        work.setSize(channels, n, false, false, true);
        work.clear();
        for (int ch = 0; ch < 2; ++ch) work.copyFrom(ch, 0, *input, juce::jmin(ch, input->getNumChannels() - 1), start, n);
        if (key != nullptr && channels >= 4)
            for (int ch = 0; ch < 2; ++ch)
                if (start < key->getNumSamples())
                    work.copyFrom(2 + ch, 0, *key, juce::jmin(ch, key->getNumChannels() - 1), start,
                                  juce::jmin(n, key->getNumSamples() - start));
        midi.clear();
        plugin->processBlock(work, midi);
        for (int ch = 0; ch < 2; ++ch) out.copyFrom(ch, start, work, ch, 0, n);
    }
    std::fprintf(stderr, "latency_end %d\n", plugin->getLatencySamples()); // plug-ins may change it once they run
    plugin->releaseResources();

    if (stateOut.isNotEmpty())
    {
        juce::MemoryBlock state;
        plugin->getStateInformation(state);
        juce::File(stateOut).replaceWithData(state.getData(), state.getSize());
    }

    juce::File outFile = juce::File::getCurrentWorkingDirectory().getChildFile(outPath);
    outFile.deleteFile();
    juce::WavAudioFormat wav;
    std::unique_ptr<juce::OutputStream> stream(outFile.createOutputStream().release());
    if (stream == nullptr) return 1;
    std::unique_ptr<juce::AudioFormatWriter> writer(wav.createWriterFor(
        stream, juce::AudioFormatWriterOptions{}.withSampleRate(rate).withNumChannels(2).withBitsPerSample(32)
                    .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint)));
    if (writer == nullptr) return 1;
    writer->writeFromAudioSampleBuffer(out, 0, length);
    return 0;
}
} // namespace

int main(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juce;
    const int result = run(argc, argv);
    std::fflush(stdout);
    return result;
}

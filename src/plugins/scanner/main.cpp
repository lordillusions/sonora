// sonora_plugin_scanner — scans ONE plugin file in its own process (ROADMAP PLG-3).
//
//   sonora_plugin_scanner <format-name> <plugin file or identifier> <output.xml>
//
// Loading a plugin to read its description can hang or crash (bad dlopen, licence dialogs,
// etc.). PluginHost runs this helper per file with a timeout and kills it when it does not
// finish, so the app never hangs. Result: <output.xml> with a <PLUGINS> element holding one
// PluginDescription per plugin found in the file. Exit code 0 = scan completed (even if empty).

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_events/juce_events.h>

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: sonora_plugin_scanner <format> <file> <output.xml>\n");
        return 2;
    }

    juce::ScopedJuceInitialiser_GUI juceInit; // plugins expect a message manager
    juce::AudioPluginFormatManager formats;
    juce::addDefaultFormatsToManager(formats);

    juce::AudioPluginFormat* format = nullptr;
    for (int i = 0; i < formats.getNumFormats(); ++i)
        if (formats.getFormat(i)->getName() == argv[1])
            format = formats.getFormat(i);
    if (format == nullptr)
    {
        std::fprintf(stderr, "unknown format %s\n", argv[1]);
        return 3;
    }

    juce::OwnedArray<juce::PluginDescription> found;
    juce::KnownPluginList list;
    list.scanAndAddFile(juce::String::fromUTF8(argv[2]), true, found, *format);

    juce::XmlElement root("PLUGINS");
    for (auto* d : found)
        root.addChildElement(d->createXml().release());

    const juce::File out(juce::String::fromUTF8(argv[3]));
    return root.writeTo(out) ? 0 : 4;
}

#include "engine/AudioFileFormats.h"

namespace sonora::engine
{

const juce::StringArray& audioFileExtensions()
{
    static const juce::StringArray extensions = [] {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        juce::StringArray result;
        for (int i = 0; i < formats.getNumKnownFormats(); ++i)
            for (const auto& extension : formats.getKnownFormat(i)->getFileExtensions())
            {
                const auto clean = extension.trimCharactersAtStart(".").toLowerCase().trim();
                if (clean.isNotEmpty())
                    result.addIfNotAlreadyThere(clean);
            }
        result.sort(true);
        return result;
    }();
    return extensions;
}

juce::String audioFileWildcard()
{
    juce::StringArray patterns;
    for (const auto& extension : audioFileExtensions())
        patterns.add("*." + extension);
    return patterns.joinIntoString(";");
}

bool isAudioFile(const juce::File& file)
{
    const auto extension = file.getFileExtension().trimCharactersAtStart(".").toLowerCase();
    return extension.isNotEmpty() && audioFileExtensions().contains(extension);
}

} // namespace sonora::engine

#pragma once
// Which audio files Sonora can decode. Derived from the formats registerBasicFormats() installs (WAV, AIFF,
// FLAC, Ogg Vorbis, MP3 and, on macOS, every CoreAudio container: M4A/AAC/ALAC, CAF, MP4, AIFC, AU, ...), so
// file pickers, the browser and drops accept exactly what SampleCache can read. Any thread except audio.

#include <juce_audio_formats/juce_audio_formats.h>

namespace sonora::engine
{

// Lowercase extensions without the dot ("wav", "m4a", ...), sorted, no duplicates.
const juce::StringArray& audioFileExtensions();

// "*.wav;*.m4a;..." for juce::FileChooser.
juce::String audioFileWildcard();

// True when the file's extension belongs to a readable audio format. Does not open the file.
bool isAudioFile(const juce::File& file);

} // namespace sonora::engine

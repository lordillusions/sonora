#include "engine/SampleExport.h"

#include <algorithm>

namespace sonora::engine
{

namespace
{
std::string writeWav(const juce::AudioBuffer<float>& audio, double sampleRate, const juce::File& target)
{
    if (audio.getNumSamples() <= 0 || audio.getNumChannels() <= 0 || sampleRate <= 0)
        return "nothing to write";
    if (! target.getParentDirectory().createDirectory())
        return "cannot create " + target.getParentDirectory().getFullPathName().toStdString();
    std::unique_ptr<juce::OutputStream> stream(target.createOutputStream());
    if (stream == nullptr)
        return "cannot write " + target.getFullPathName().toStdString();
    juce::WavAudioFormat wav;
    auto writer = wav.createWriterFor(stream, juce::AudioFormatWriterOptions{}
                                                  .withSampleRate(sampleRate)
                                                  .withNumChannels(audio.getNumChannels())
                                                  .withBitsPerSample(24));
    if (writer == nullptr)
        return "cannot create a WAV writer";
    const bool ok = writer->writeFromAudioSampleBuffer(audio, 0, audio.getNumSamples());
    writer.reset();
    return ok ? std::string{} : "cannot write " + target.getFullPathName().toStdString();
}
} // namespace

std::string writeReversedWav(const juce::AudioBuffer<float>& audio, double sampleRate, const juce::File& target)
{
    juce::AudioBuffer<float> reversed(audio);
    for (int channel = 0; channel < reversed.getNumChannels(); ++channel)
        std::reverse(reversed.getWritePointer(channel), reversed.getWritePointer(channel) + reversed.getNumSamples());
    return writeWav(reversed, sampleRate, target);
}

std::string writeCroppedWav(const juce::AudioBuffer<float>& audio, double sampleRate, int startFrame, int numFrames, const juce::File& target)
{
    startFrame = std::clamp(startFrame, 0, audio.getNumSamples());
    numFrames = std::clamp(numFrames, 0, audio.getNumSamples() - startFrame);
    juce::AudioBuffer<float> cropped(audio.getNumChannels(), numFrames);
    for (int channel = 0; channel < cropped.getNumChannels(); ++channel)
        cropped.copyFrom(channel, 0, audio, channel, startFrame, numFrames);
    return writeWav(cropped, sampleRate, target);
}

} // namespace sonora::engine

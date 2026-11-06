#include "devices/native/WavetableSprites.h"

#include "devices/native/WavetableInstrument.h"
#include "io/als/AlsReader.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>
#include <tuple>

namespace sonora::devices
{
namespace
{
constexpr int kFrame = kWavetableFrameLength;
constexpr const char* kPrefix = "Vector_Sprite_";

std::mutex& cacheLock()
{
    static std::mutex lock;
    return lock;
}

std::string findFolder()
{
    if (const char* forced = std::getenv("SONORA_WAVETABLE_SPRITES"))
        return juce::File::isAbsolutePath(forced) && juce::File(forced).isDirectory() ? std::string(forced) : std::string();
    // Any installed edition of the reference application (Suite / Standard / Intro / Lite, 12 or 11).
    const juce::File apps("/Applications");
    juce::Array<juce::File> bundles = apps.findChildFiles(juce::File::findDirectories, false, "Ableton Live*.app");
    bundles.sort();
    for (int i = bundles.size(); --i >= 0;)
    {
        const auto folder = bundles[i].getChildFile("Contents/App-Resources/Builtin/Samples/Vector/Sprites");
        if (folder.isDirectory()) return folder.getFullPathName().toStdString();
    }
    return {};
}

std::shared_ptr<const WavetableData> fromBuffer(const juce::AudioBuffer<float>& decoded, double sampleRate, const std::string& name,
                                                std::string& error)
{
    const int channels = std::min(2, decoded.getNumChannels());
    int length = decoded.getNumSamples();
    if (channels < 1 || length < 2) { error = "empty wavetable file"; return nullptr; }
    std::vector<float> interleaved;
    int frames = std::min(WavetableData::maxFrames, length / kFrame);
    if (frames < 1)
    {
        // A file shorter than one frame becomes one frame (linear resampling of the whole file).
        frames = 1;
        interleaved.resize((size_t) kFrame * (size_t) channels);
        for (int i = 0; i < kFrame; ++i)
        {
            const double x = (double) i * (length - 1) / kFrame;
            const int a = (int) x;
            const float mix = (float) (x - a);
            for (int c = 0; c < channels; ++c)
            {
                const float* d = decoded.getReadPointer(c);
                interleaved[(size_t) (i * channels + c)] = d[a] + (d[std::min(a + 1, length - 1)] - d[a]) * mix;
            }
        }
    }
    else
    {
        length = frames * kFrame;
        interleaved.resize((size_t) length * (size_t) channels);
        for (int i = 0; i < length; ++i)
            for (int c = 0; c < channels; ++c) interleaved[(size_t) (i * channels + c)] = decoded.getSample(c, i);
    }
    auto result = WavetableData::fromSamples(interleaved, channels, kFrame, sampleRate, name);
    if (! result) error = result.error;
    return result.data;
}
} // namespace

std::string factoryWavetableFolder()
{
    static const std::string folder = findFolder();
    return folder;
}

const std::vector<WavetableSprite>& factoryWavetables()
{
    static const std::vector<WavetableSprite> sprites = [] {
        std::vector<WavetableSprite> out;
        const auto folder = factoryWavetableFolder();
        if (folder.empty()) return out;
        for (const auto& file : juce::File(folder).findChildFiles(juce::File::findFiles, false, juce::String(kPrefix) + "*.wav"))
        {
            const auto stem = file.getFileNameWithoutExtension().fromFirstOccurrenceOf(kPrefix, false, false);
            const auto category = stem.upToFirstOccurrenceOf(" ", false, false);
            const auto name = stem.fromFirstOccurrenceOf(" ", false, false);
            if (category.isEmpty() || name.isEmpty()) continue;
            out.push_back({category.toStdString(), name.toStdString(), file.getFullPathName().toStdString()});
        }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return std::tie(a.category, a.name) < std::tie(b.category, b.name); });
        return out;
    }();
    return sprites;
}

const WavetableSprite* findFactoryWavetable(const std::string& name)
{
    for (const auto& sprite : factoryWavetables())
        if (sprite.name == name || sprite.category + " " + sprite.name == name) return &sprite;
    return nullptr;
}

std::shared_ptr<const WavetableData> loadWavetableFile(const std::string& path, std::string& error)
{
    static std::map<std::string, std::weak_ptr<const WavetableData>> cache;
    {
        const std::lock_guard<std::mutex> lock(cacheLock());
        if (auto found = cache.find(path); found != cache.end())
            if (auto table = found->second.lock()) return table;
    }
    const juce::File file(path);
    if (! juce::File::isAbsolutePath(path) || ! file.existsAsFile()) { error = "file not found"; return nullptr; }
    juce::AudioFormatManager formats;
    formats.registerBasicFormats();
    const std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
    if (! reader || reader->numChannels < 1) { error = "unsupported audio file"; return nullptr; }
    const int64_t limit = (int64_t) kFrame * WavetableData::maxFrames;
    const int count = (int) std::min<int64_t>(reader->lengthInSamples, limit);
    const int channels = (int) std::min(2u, reader->numChannels);
    juce::AudioBuffer<float> decoded(channels, std::max(1, count));
    decoded.clear();
    if (count <= 0 || ! reader->read(&decoded, 0, count, 0, true, channels == 2)) { error = "unreadable audio file"; return nullptr; }
    auto table = fromBuffer(decoded, reader->sampleRate, file.getFileNameWithoutExtension().toStdString(), error);
    if (table)
    {
        const std::lock_guard<std::mutex> lock(cacheLock());
        cache[path] = table;
    }
    return table;
}

std::shared_ptr<const WavetableData> standInWavetable()
{
    static const std::shared_ptr<const WavetableData> table = [] {
        // Sonora's own morph: sine -> triangle -> saw -> square across 16 frames (additive, band-limited by the bank).
        constexpr int frames = 16;
        std::vector<float> samples((size_t) frames * kFrame);
        for (int f = 0; f < frames; ++f)
        {
            const float x = (float) f / (frames - 1) * 3.0f;
            const int segment = std::min(2, (int) x);
            const float mix = x - (float) segment;
            for (int i = 0; i < kFrame; ++i)
            {
                const double p = (double) i / kFrame;
                const float shapes[4] = {(float) std::sin(2 * M_PI * p), (float) (1 - 4 * std::abs(std::fmod(p + 0.75, 1.0) - 0.5)),
                                         (float) (1 - 2 * std::fmod(p + 0.5, 1.0)), p < 0.5 ? 0.8f : -0.8f};
                samples[(size_t) (f * kFrame + i)] = shapes[segment] + (shapes[segment + 1] - shapes[segment]) * mix;
            }
        }
        return WavetableData::fromSamples(samples, 1, kFrame, 44100, "Sonora stand-in").data;
    }();
    return table;
}

WavetableSpriteRef readWavetableSpriteRef(const model::Device& device, int index)
{
    WavetableSpriteRef ref;
    if (! device.sourceXml) return ref;
    const auto xml = juce::XmlDocument::parse(juce::String(*device.sourceXml));
    if (! xml) return ref;
    const auto n = juce::String(index + 1);
    if (const auto* name = xml->getChildByName("SpriteName" + n)) ref.spriteName = name->getStringAttribute("Value").toStdString();
    if (const auto* raw = xml->getChildByName("UseRawUserWavetable" + n)) ref.raw = raw->getStringAttribute("Value") == "true";
    if (const auto* slot = xml->getChildByName("UserSprite" + n))
        if (const auto* value = slot->getChildByName("Value"))
            if (const auto* sample = value->getChildByName("SampleRef"))
                if (const auto* file = sample->getChildByName("FileRef"))
                {
                    model::FileRef fileRef;
                    const auto leaf = [file](const char* tag) {
                        const auto* e = file->getChildByName(tag);
                        return e != nullptr ? e->getStringAttribute("Value").toStdString() : std::string();
                    };
                    fileRef.absolutePath = leaf("Path");
                    fileRef.relativePath = leaf("RelativePath");
                    fileRef.sourceDirectory = device.sourceDirectory;
                    model::Song empty;
                    ref.userPath = io::resolveSamplePath(empty, fileRef);
                    if (ref.userPath.empty()) ref.userPath = "?" + (fileRef.absolutePath.empty() ? fileRef.relativePath : fileRef.absolutePath);
                }
    return ref;
}

void setWavetableSpriteName(model::Device& device, int index, const std::string& name)
{
    if (! device.sourceXml) return;
    auto xml = juce::XmlDocument::parse(juce::String(*device.sourceXml));
    if (! xml) return;
    const auto n = juce::String(index + 1);
    auto* sprite = xml->getChildByName("SpriteName" + n);
    if (sprite == nullptr) sprite = xml->createNewChildElement("SpriteName" + n);
    sprite->setAttribute("Value", juce::String::fromUTF8(name.c_str()));
    if (auto* slot = xml->getChildByName("UserSprite" + n))
        if (auto* value = slot->getChildByName("Value")) value->deleteAllChildElements();
    device.sourceXml = std::make_shared<const std::string>(xml->toString(juce::XmlElement::TextFormat().withoutHeader()).toStdString());
    device.nativeContentEdited = true; // the writer copies SpriteName / UserSprite from sourceXml (AlsWriter patchDevice)
}

void prepareWavetableInstrument(WavetableInstrument& synth, const model::Device& device, std::vector<std::string>& warnings)
{
    const auto label = device.displayName.empty() ? std::string("Wavetable") : device.displayName;
    for (int osc = 0; osc < 2; ++osc)
    {
        const auto ref = readWavetableSpriteRef(device, osc);
        std::string error;
        if (! ref.userPath.empty())
        {
            if (ref.userPath[0] != '?')
                if (auto table = loadWavetableFile(ref.userPath, error))
                {
                    synth.setTable(osc, std::move(table), {}, juce::File(ref.userPath).getFileNameWithoutExtension().toStdString());
                    continue;
                }
            warnings.push_back(label + ": user wavetable of oscillator " + std::to_string(osc + 1) + " not found ("
                               + ref.userPath.substr(ref.userPath[0] == '?' ? 1 : 0) + "); a generated stand-in plays");
            synth.setTable(osc, standInWavetable(), {}, "Stand-in");
            continue;
        }
        const auto name = ref.spriteName.empty() ? std::string("Basic Shapes") : ref.spriteName;
        if (const auto* sprite = findFactoryWavetable(name))
            if (auto table = loadWavetableFile(sprite->file, error))
            {
                synth.setTable(osc, std::move(table), sprite->category, sprite->name);
                continue;
            }
        // Only warn for oscillators that sound: the second oscillator is often off with an untouched default table.
        if (device.param(osc == 0 ? "Voice_Oscillator1_On" : "Voice_Oscillator2_On", osc == 0 ? 1.0 : 0.0) > 0.5)
            warnings.push_back(label + ": factory wavetable \"" + name + "\" needs the reference application's library ("
                               + (error.empty() ? std::string("not installed") : error) + "); a generated stand-in plays");
        synth.setTable(osc, standInWavetable(), {}, name);
    }
    for (int f = 0; f < 2; ++f)
        if (device.param(f == 0 ? "Voice_Filter1_Type" : "Voice_Filter2_Type", 0) > 4.5)
            warnings.push_back(label + ": filter " + std::to_string(f + 1) + " type "
                               + std::to_string((int) device.param(f == 0 ? "Voice_Filter1_Type" : "Voice_Filter2_Type", 0))
                               + " is not known yet; Morph plays instead");
}
} // namespace sonora::devices

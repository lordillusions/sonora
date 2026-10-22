#include "plugins/PluginHost.h"

#include "plugins/Vst3PresetBuilder.h"
#include "core/AppDataFolder.h"

#include <atomic>
#include <algorithm>
#include <functional>
#include <string_view>
#include <cstdio>
#include <mutex>
#include <thread>

namespace sonora::plugins
{

PluginHost::PluginHost()
{
    juce::addDefaultFormatsToManager(formatManager_);
}

PluginHost::~PluginHost() = default;

juce::File PluginHost::cacheFile()
{
    return core::appDataDirectory().getChildFile("PluginCache.xml");
}

void PluginHost::loadCache()
{
    if (auto xml = juce::XmlDocument::parse(cacheFile()))
        knownPlugins_.recreateFromXml(*xml);
}

void PluginHost::saveCache() const
{
    if (auto xml = knownPlugins_.createXml())
    {
        cacheFile().getParentDirectory().createDirectory();
        xml->writeTo(cacheFile());
    }
}

juce::File PluginHost::scannerExecutable()
{
    if (const auto env = juce::SystemStats::getEnvironmentVariable("SONORA_PLUGIN_SCANNER", {}); env.isNotEmpty())
        return juce::File(env);
    const auto exeDir = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getParentDirectory();
#if defined(_WIN32)
    const char* name = "sonora_plugin_scanner.exe";
#else
    const char* name = "sonora_plugin_scanner";
#endif
    for (const auto& dir : {exeDir, exeDir.getSiblingFile("bin")})
        if (const auto f = dir.getChildFile(name); f.existsAsFile())
            return f;
    return exeDir.getChildFile(name);
}

bool PluginHost::scanFile(const juce::String& formatName, const juce::String& file, juce::String& error)
{
    const auto scanner = scannerExecutable();
    if (! scanner.existsAsFile())
    {
        error = "Plugin scanner helper not found: " + scanner.getFullPathName();
        return false;
    }

    juce::TemporaryFile result(".xml");
    juce::ChildProcess child;
    if (! child.start(juce::StringArray{scanner.getFullPathName(), formatName, file, result.getFile().getFullPathName()}))
    {
        error = "Could not start the plugin scanner";
        return false;
    }

    const auto deadline = juce::Time::getMillisecondCounter() + (juce::uint32) scanTimeoutMs;
    while (child.isRunning())
    {
        if (juce::Time::getMillisecondCounter() > deadline)
        {
            child.kill();
            knownPlugins_.addToBlacklist(file);
            error = "Timed out after " + juce::String(scanTimeoutMs / 1000) + " s: " + file;
            return false;
        }
        juce::Thread::sleep(20);
    }

    const auto xml = juce::XmlDocument::parse(result.getFile());
    if (child.getExitCode() != 0 || xml == nullptr)
    {
        knownPlugins_.addToBlacklist(file);
        error = "Scanner failed (exit " + juce::String((int) child.getExitCode()) + "): " + file;
        return false;
    }

    int added = 0;
    for (const auto* e : xml->getChildIterator())
    {
        juce::PluginDescription d;
        if (d.loadFromXml(*e))
        {
            knownPlugins_.addType(d);
            ++added;
        }
    }
    if (added == 0)
    {
        // Typical causes: macOS refuses to load the binary (quarantine / signature policy),
        // wrong CPU architecture (Intel-only on this arm64 build), missing dependencies.
        knownPlugins_.addToBlacklist(file);
        error = "No loadable plugin (blocked by macOS policy, wrong architecture or missing dependency): " + file;
        return false;
    }
    return true;
}

void PluginHost::scan(bool includeAu, const std::function<void(float, const juce::String&)>& progress)
{
    // Collect the work first so progress is meaningful.
    struct Item
    {
        juce::String format, file;
    };
    std::vector<Item> work;
    for (int i = 0; i < formatManager_.getNumFormats(); ++i)
    {
        auto* format = formatManager_.getFormat(i);
        const auto formatName = format->getName();
        if (formatName != "VST3" && ! (includeAu && formatName == "AudioUnit"))
            continue;

        const auto blacklisted = knownPlugins_.getBlacklistedFiles();
        for (const auto& file : format->searchPathsForPlugins(format->getDefaultLocationsToSearch(), true, false))
        {
            if (blacklisted.contains(file))
                continue;
            bool upToDate = false;
            for (const auto& t : knownPlugins_.getTypes())
                if (t.pluginFormatName == formatName && t.fileOrIdentifier == file && ! format->pluginNeedsRescanning(t))
                    upToDate = true;
            if (! upToDate)
                work.push_back({formatName, file});
        }
    }

    // Several helper processes at once: a hanging plugin only costs its own timeout slot.
    std::atomic<size_t> next{0}, finished{0};
    std::mutex reportMutex;
    auto worker = [&] {
        for (;;)
        {
            const size_t i = next.fetch_add(1);
            if (i >= work.size())
                return;
            juce::String error;
            const bool ok = scanFile(work[i].format, work[i].file, error);
            const size_t done = ++finished;
            std::lock_guard lock(reportMutex);
            if (! ok)
                std::fprintf(stderr, "[sonora] plugin scan: %s\n", error.toRawUTF8());
            if (progress)
                progress((float) done / (float) work.size(), juce::File(work[i].file).getFileName());
            if (done % 8 == 0)
                saveCache(); // keep progress if the app is quit mid-scan
        }
    };
    const int workers = std::max(1, std::min(4, (int) work.size()));
    std::vector<std::thread> threads;
    for (int w = 0; w < workers; ++w)
        threads.emplace_back(worker);
    for (auto& t : threads)
        t.join();
    saveCache();
}

std::optional<juce::PluginDescription> PluginHost::findMatch(const model::PluginInfo& info,
                                                             model::DeviceKind kind) const
{
    const auto types = knownPlugins_.getTypes();
    const juce::String wanted = juce::String::fromUTF8(info.name.c_str()).trim();
    if (kind == model::DeviceKind::PluginVst3
        && std::any_of(info.vst3Uid.begin(), info.vst3Uid.end(), [](uint32_t word) { return word != 0; }))
    {
        // JUCE 8's PluginDescription::uniqueId hashes the four normalised FUID words.
        // Prefer class identity: separate plugins may expose the same display name (UI-7).
        uint32_t signature = 0;
        for (uint32_t word : info.vst3Uid) signature = signature * 31 + word;
        for (const auto& description : types)
            if (description.pluginFormatName == "VST3" && description.uniqueId == (int32_t) signature)
                return description;
    }

    auto byName = [&](const juce::String& format) -> std::optional<juce::PluginDescription> {
        for (const auto& d : types)
            if (d.pluginFormatName == format && d.name.equalsIgnoreCase(wanted))
                return d;
        // Live often stores names with suffixes like "_x64" or "-1.1" for VST2.
        for (const auto& d : types)
            if (d.pluginFormatName == format && (wanted.startsWithIgnoreCase(d.name) || d.name.startsWithIgnoreCase(wanted)))
                return d;
        return std::nullopt;
    };

    switch (kind)
    {
        case model::DeviceKind::PluginVst3:
        case model::DeviceKind::PluginVst2:
            return byName("VST3");
        case model::DeviceKind::PluginAu:
        {
            // JUCE AU identifiers look like "AudioUnit:Synths/aumu,XXXX,YYYY".
            const auto fourCC = [](uint32_t v) {
                char s[5] = {(char) (v >> 24), (char) (v >> 16), (char) (v >> 8), (char) v, 0};
                return juce::String(s);
            };
            const auto needle = fourCC(info.auType) + "," + fourCC(info.auSubType) + "," + fourCC(info.auManufacturer);
            for (const auto& d : types)
                if (d.pluginFormatName == "AudioUnit" && d.fileOrIdentifier.endsWith(needle))
                    return d;
            if (auto vst3 = byName("VST3"))
                return vst3; // fall back to the VST3 build of the same plugin
            return std::nullopt;
        }
        case model::DeviceKind::Native:
        case model::DeviceKind::Rack:
        case model::DeviceKind::MaxForLive:
            return std::nullopt;
    }
    return std::nullopt;
}

std::string PluginHost::preloadKey(const juce::PluginDescription& desc, const model::PluginInfo& src, model::DeviceKind kindInSet,
                                   double sampleRate, int blockSize)
{
    // Instances with the same plug-in, saved state and processing setup are interchangeable.
    const auto bytes = [](const std::vector<uint8_t>& v) { return std::hash<std::string_view>{}(std::string_view((const char*) v.data(), v.size())); };
    std::string key = desc.createIdentifierString().toStdString();
    key += "|" + std::to_string((int) kindInSet) + "|" + std::to_string(sampleRate) + "|" + std::to_string(blockSize);
    for (const auto part : src.vst3Uid) key += "|" + std::to_string(part);
    key += "|" + std::to_string(src.state.size()) + ":" + std::to_string(bytes(src.state));
    key += "|" + std::to_string(src.controllerState.size()) + ":" + std::to_string(bytes(src.controllerState));
    return key;
}

bool PluginHost::preload(const juce::PluginDescription& desc, const model::PluginInfo& src, model::DeviceKind kindInSet,
                         double sampleRate, int blockSize)
{
    Preloaded entry;
    entry.key = preloadKey(desc, src, kindInSet, sampleRate, blockSize);
    entry.instance = createInstance(desc, src, kindInSet, sampleRate, blockSize, entry.error);
    entry.createMs = lastCreateMs_;
    entry.stateMs = lastStateMs_;
    if (entry.instance == nullptr) return false; // the build retries and reports the error
    preloaded_.push_back(std::move(entry));
    return true;
}

std::unique_ptr<juce::AudioPluginInstance> PluginHost::instantiate(const juce::PluginDescription& desc,
                                                                   const model::PluginInfo& src,
                                                                   model::DeviceKind kindInSet,
                                                                   double sampleRate,
                                                                   int blockSize,
                                                                   juce::String& error)
{
    if (! preloaded_.empty())
    {
        const auto key = preloadKey(desc, src, kindInSet, sampleRate, blockSize);
        const auto it = std::find_if(preloaded_.begin(), preloaded_.end(), [&key](const Preloaded& p) { return p.key == key; });
        if (it != preloaded_.end())
        {
            auto instance = std::move(it->instance);
            error = it->error;
            lastCreateMs_ = it->createMs;
            lastStateMs_ = it->stateMs;
            preloaded_.erase(it);
            return instance;
        }
    }
    return createInstance(desc, src, kindInSet, sampleRate, blockSize, error);
}

std::unique_ptr<juce::AudioPluginInstance> PluginHost::createInstance(const juce::PluginDescription& desc,
                                                                      const model::PluginInfo& src,
                                                                      model::DeviceKind kindInSet,
                                                                      double sampleRate,
                                                                      int blockSize,
                                                                      juce::String& error)
{
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    auto instance = formatManager_.createPluginInstance(desc, sampleRate, blockSize, error);
    lastCreateMs_ = juce::Time::getMillisecondCounterHiRes() - t0;
    lastStateMs_ = 0.0;
    if (instance == nullptr)
        return nullptr;
    const auto t1 = juce::Time::getMillisecondCounterHiRes();

    // ---- restore state saved by Live
    if (kindInSet == model::DeviceKind::PluginVst3 && desc.pluginFormatName == "VST3" && ! src.state.empty())
    {
        if (auto* client = instance->getVST3Client())
        {
            const auto preset = buildVstPreset(src.vst3Uid, src.state, src.controllerState);
            if (! client->setPreset(juce::MemoryBlock(preset.data(), preset.size())))
                error = "VST3 state restore failed for " + desc.name;
            else
            {
                // AGT-7a: JUCE 8's VST3-client preset route bypasses its cached parameter values.
                // Its public processor-state route also synchronizes that cache. Reuse the original
                // opaque chunks in JUCE's pinned VST3PluginState envelope rather than replaying controls.
                juce::XmlElement state("VST3PluginState");
                state.createNewChildElement("IComponent")->addTextElement(juce::MemoryBlock(src.state.data(), src.state.size()).toBase64Encoding());
                if (! src.controllerState.empty())
                    state.createNewChildElement("IEditController")->addTextElement(juce::MemoryBlock(src.controllerState.data(), src.controllerState.size()).toBase64Encoding());
                juce::MemoryBlock wrapped;
                juce::AudioProcessor::copyXmlToBinary(state, wrapped);
                instance->setStateInformation(wrapped.getData(), static_cast<int>(wrapped.getSize()));
            }
        }
    }
    else if (kindInSet == model::DeviceKind::PluginAu && desc.pluginFormatName == "AudioUnit" && ! src.state.empty())
    {
        // Hypothesis (PLG-4): <AuPreset><Buffer> is the AU ClassInfo plist, which is what
        // JUCE's AU wrapper accepts in setStateInformation(). Verify with real plugins.
        instance->setStateInformation(src.state.data(), (int) src.state.size());
    }
    else if (kindInSet == model::DeviceKind::PluginVst2)
    {
        error = desc.name + ": VST2 state cannot be migrated yet, loaded with default settings (PLG-5)";
    }
    lastStateMs_ = juce::Time::getMillisecondCounterHiRes() - t1;
    return instance;
}

} // namespace sonora::plugins

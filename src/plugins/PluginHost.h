#pragma once
// Discovers and instantiates third-party plugins.
//
// Formats (docs/adr/0003-plugin-format.md): VST3, VST2 (PLG-15, through FST's clean-room headers; JUCE's name "VST")
// and Audio Units on macOS. Each loads with the state Live stored for it.
//
// Scanning is slow and may hang or crash on buggy plugins, so every plugin file is scanned
// by a separate helper process (`sonora_plugin_scanner`, PLG-3) with a timeout. Files whose
// scan hangs, crashes or fails are blacklisted (persisted in the cache) and listed in
// failedPlugins() so the UI can show them. Results are cached in
//   <data folder>/PluginCache.xml (core/AppDataFolder.h: Application Support/Sonora, or Sonora Nightly)

#include "core/Model.h"

#include <juce_audio_processors/juce_audio_processors.h>

#include <functional>
#include <memory>

namespace sonora::plugins
{

class PluginHost
{
public:
    PluginHost();
    ~PluginHost();

    juce::AudioPluginFormatManager& formats() { return formatManager_; }
    juce::KnownPluginList& knownPlugins() { return knownPlugins_; }

    // Loads the cache file. Cheap; call at startup.
    void loadCache();
    void saveCache() const;

    // Scans the default VST3 and VST2 folders (and AU when includeAu). Blocking: call from a
    // background thread. `progress` receives (0..1, currentFileName).
    void scan(bool includeAu, const std::function<void(float, const juce::String&)>& progress = {});

    // Scans one plugin file/bundle out of process. Adds what it finds to the known list.
    // Returns false (and blacklists the file) when the helper timed out, crashed or failed;
    // `error` then says why. Blocking: call from a background thread.
    bool scanFile(const juce::String& formatName, const juce::String& file, juce::String& error);

    // Files that failed or hung during scanning (blacklisted). Cleared by clearBlacklist().
    juce::StringArray failedPlugins() const { return knownPlugins_.getBlacklistedFiles(); }
    void clearBlacklist() { knownPlugins_.clearBlacklistedFiles(); }

    // The helper executable: next to the running executable, else ../bin, else
    // $SONORA_PLUGIN_SCANNER. Returns a non-existent File when none is found.
    static juce::File scannerExecutable();

    int scanTimeoutMs = 60000; // per plugin file

    // Finds the installed plugin that corresponds to a plugin referenced in a .als.
    // Matching rules (in order): VST3 by class uid, then name; VST2 by unique id, then name, then the VST3 build of the same
    // name (no state migration, PLG-5); AU by type/subtype/manufacturer, then the VST3, then the VST2 of the same name.
    std::optional<juce::PluginDescription> findMatch(const model::PluginInfo& info, model::DeviceKind kind) const;

    // Instantiates a plugin and restores the state stored in the .als. Message thread.
    std::unique_ptr<juce::AudioPluginInstance> instantiate(const juce::PluginDescription& desc,
                                                           const model::PluginInfo& stateSource,
                                                           model::DeviceKind kindInSet,
                                                           double sampleRate,
                                                           int blockSize,
                                                           juce::String& error);

    // UI-51: starts an instance ahead of a graph build. Opening a set preloads its plug-ins one per message-loop turn, so
    // the window keeps painting (no system wait cursor); instantiate() then hands out the matching instance (same plug-in,
    // kind, state, rate and block size) instead of creating one. Message thread. False when the instance failed.
    bool preload(const juce::PluginDescription& desc, const model::PluginInfo& stateSource, model::DeviceKind kindInSet,
                 double sampleRate, int blockSize);
    void clearPreloaded() { preloaded_.clear(); } // drops instances no build used
    // A running instance handed back for the next build (AudioEngine::reloadSongKeepingPlugins): claimed like a preloaded
    // one when the build asks for the same plug-in with the same state and processing setup.
    void donate(const juce::PluginDescription& desc, const model::PluginInfo& stateSource, model::DeviceKind kindInSet,
                double sampleRate, int blockSize, std::unique_ptr<juce::AudioPluginInstance> instance);
    size_t preloadedCount() const { return preloaded_.size(); }

    // Milliseconds the last instantiate() spent creating the instance and restoring its state (diagnostics).
    double lastCreateMs() const { return lastCreateMs_; }
    double lastStateMs() const { return lastStateMs_; }

    static juce::File cacheFile();

private:
    struct Preloaded
    {
        std::string key;
        juce::String error;
        double createMs = 0, stateMs = 0;
        std::unique_ptr<juce::AudioPluginInstance> instance;
    };
    static std::string preloadKey(const juce::PluginDescription&, const model::PluginInfo&, model::DeviceKind, double, int);
    std::unique_ptr<juce::AudioPluginInstance> createInstance(const juce::PluginDescription&, const model::PluginInfo&,
                                                              model::DeviceKind, double, int, juce::String& error);
    std::vector<Preloaded> preloaded_;
    double lastCreateMs_ = 0.0, lastStateMs_ = 0.0;
    juce::AudioPluginFormatManager formatManager_;
    juce::KnownPluginList knownPlugins_;
};

} // namespace sonora::plugins

// PLG-16: after a set loads, offer Sonora devices in place of missing Serum 2 / LFOTool instances (plugins/NativeReplacement.h).
#include "app/AppController.h"
#include "app/PromptWindow.h"
#include "plugins/NativeReplacement.h"
#include "ui/DeviceMenu.h"
#include "util/Log.h"

namespace sonora::app
{
namespace
{
// SONORA_NATIVE_SWAP_AUTOTEST=replace|keep|replace-undo answers the prompt without driving the UI and logs each
// affected track's devices afterwards ("[sonora] native swap:" lines).
juce::String swapAutotest() { return juce::SystemStats::getEnvironmentVariable("SONORA_NATIVE_SWAP_AUTOTEST", {}); }

void logDevices(const model::Song& song, const std::vector<plugins::NativeReplacement>& replacements, const char* when)
{
    std::set<int> tracks;
    for (const auto& r : replacements) tracks.insert(r.track);
    for (int index : tracks)
        if (const auto* track = model::trackAt(song, index))
        {
            std::string line;
            const std::function<void(const std::vector<model::Device>&)> list = [&](const std::vector<model::Device>& devices) {
                for (const auto& d : devices)
                {
                    line += (line.empty() || line.back() == '[' ? "" : ", ") + d.type + (d.plugin ? ":" + d.plugin->name : std::string());
                    for (const auto& chain : d.chains) { line += " ["; list(chain.devices); line += "]"; }
                }
            };
            list(track->devices);
            logf("native swap: %s, track \"%s\": %s", when, track->name.c_str(), line.c_str());
        }
}
} // namespace

void AppController::offerNativeReplacements()
{
    if (! song_ || installingCollabSong_ || inCollabSession()) return; // a shared document is the host's to change
    auto found = plugins::findNativeReplacements(*song_, [this](const model::Device& d) {
        return d.plugin && host_.findMatch(*d.plugin, d.kind).has_value();
    });
    if (found.empty()) return;
    for (const auto& r : found)
        logf("native swap: offer %s -> %s on \"%s\" (%zu notes)", r.pluginName.c_str(), plugins::nativeAlternativeName(r.alternative),
             r.trackName.c_str(), r.notes.size());
    showNativeReplacementOffer(std::make_shared<std::vector<plugins::NativeReplacement>>(std::move(found)), documentGeneration_, 0);
}

void AppController::showNativeReplacementOffer(std::shared_ptr<std::vector<plugins::NativeReplacement>> found, int generation, int attempt)
{
    std::weak_ptr<bool> alive = alive_;
    // The set is installed while the busy overlay (and at launch the splash) is still up: wait for them and for any
    // other prompt, for up to a minute.
    juce::Timer::callAfterDelay(attempt == 0 ? 400 : 250, [this, alive, found, generation, attempt] {
        if (alive.expired() || generation != documentGeneration_ || ! song_) return;
        if (ui_.waitingForBusy() || PromptWindow::isShowing())
        {
            if (attempt < 240) showNativeReplacementOffer(found, generation, attempt + 1);
            return;
        }
        const auto offer = plugins::describeNativeReplacements(*found);
        PromptOptions options;
        options.state.title = offer.title;
        options.state.message = offer.message;
        options.state.buttons = {{found->size() == 1 ? "Replace" : "Replace All", true, false}, {"Keep", false, false}};
        options.defaultButton = 0;
        options.cancelButton = 1;
        options.shortcuts = {0, 'k'};
        const bool shown = PromptWindow::show(std::move(options), [this, alive, found, generation](int button) {
            if (alive.expired() || generation != documentGeneration_ || ! song_) return;
            if (button != 0)
            {
                logf("native swap: kept %zu plug-in placeholders", found->size());
                return;
            }
            replaceWithNativeDevices(*found);
        });
        if (! shown) return;
        if (const auto answer = swapAutotest(); answer.isNotEmpty())
            juce::Timer::callAfterDelay(800, [answer] {
                logf("native swap: autotest answers %s", answer.toRawUTF8());
                PromptWindow::pressKey(answer.startsWith("replace") ? juce::KeyPress(juce::KeyPress::returnKey)
                                                                    : juce::KeyPress(juce::KeyPress::escapeKey));
            });
    });
}

void AppController::replaceWithNativeDevices(const std::vector<plugins::NativeReplacement>& replacements)
{
    const int generation = documentGeneration_;
    syncModelFromEngine();
    if (generation != documentGeneration_ || ! song_) return;
    const auto result = applyEdit(plugins::replaceWithNative(*song_, replacements));
    if (! result.ok)
    {
        logf("native swap: not applied: %s", ui_.statusText.c_str());
        return;
    }
    const auto before = ui_.buildLog.size();
    ui::reportNativeReplacements(ui_, replacements); // the same notes and status line as the device menu's swap
    for (size_t i = before; i < ui_.buildLog.size(); ++i) logf("native swap: note: %s", ui_.buildLog[i].c_str());
    logDevices(*song_, replacements, "replaced");
    if (swapAutotest() == "replace-undo")
    {
        undo();
        logDevices(*song_, replacements, "after undo");
    }
    if (ui_.requestRepaint) ui_.requestRepaint();
}
} // namespace sonora::app

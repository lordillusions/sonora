#pragma once
// Floating Sonora Prism editor (UI-27). The dashboard does not fit the Device View, so unlike the other native
// devices it opens in its own window by default, like a hosted plug-in editor. This widget is the root of that window:
// a header with the device name and a Dock button above a WavetableEditor. It follows its instance through graph
// rebuilds by UiContext::WavetableKey and asks the host to close the window when the instance is docked or deleted.

#include "ui/RootWidget.h"

#include <optional>
#include <string>

namespace sonora::devices { class WavetableSynth; }
namespace sonora::ui
{
class TextButton;
class WavetableEditor;

// Where a Wavetable instance currently lives: control-track index, track device index, rack chain/child (or -1).
struct WavetableSite
{
    int track = -1, device = -1, chain = -1, child = -1;
    const model::Device* model = nullptr;
    devices::WavetableSynth* processor = nullptr; // null while the graph is being rebuilt
};
std::optional<WavetableSite> findWavetable(const UiContext&, const UiContext::WavetableKey&);

// Device edits (and their undo/redo) replace a track-level device in place under a fresh id. Snapshot the ids before an
// edit; afterwards, the same track with the same device count but another id at a position is that replacement.
using DeviceIdsByTrack = std::map<model::TrackId, std::vector<int>>;
struct ReplacedDevice { model::TrackId track; int oldId, newId; };
DeviceIdsByTrack deviceIdsByTrack(const model::Song&);
std::vector<ReplacedDevice> replacedDevices(const DeviceIdsByTrack& before, const model::Song& after);

class WavetableWindowView final : public RootWidget
{
public:
    static constexpr float kHeader = 26.0f;
    static constexpr float kPreferredWidth = 1092.0f, kPreferredHeight = 746.0f; // full dashboard below the header

    WavetableWindowView(UiContext&, UiContext::WavetableKey);

    const UiContext::WavetableKey& key() const { return key_; }
    void setKey(const UiContext::WavetableKey& key);
    // Rebuilds the editor after song/graph changes. Call synchronously when the model changes (pointers into the old
    // graph must go before it is deleted). Asks the host to close the window once the instance is docked or gone.
    void sync();
    WavetableEditor* editor() const { return editor_; }
    TextButton* dockButton() const { return dock_; }
    const std::string& title() const { return title_; }
    std::function<void(const std::string&)> onTitleChanged; // the window name follows the device and track names

    void layout() override;
    void paint(SkCanvas&, const Theme&) override;
    void tick() override;

private:
    bool stale() const;
    void rebuild();
    void dock();
    void requestClose();

    UiContext::WavetableKey key_;
    WavetableEditor* editor_ = nullptr;
    TextButton* dock_ = nullptr;
    const void* builtSong_ = nullptr;
    const void* builtGraph_ = nullptr;
    uint64_t builtStructure_ = 0;
    bool built_ = false, closeRequested_ = false;
    std::string title_;
};
} // namespace sonora::ui

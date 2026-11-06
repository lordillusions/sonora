#pragma once
// Everything around one Sonora Hush instance that is not audio (PLG-14, PLG-14g): presets (factory, user, and preset files
// in the same JSON layout), A/B slots, the face's own undo/redo, the face's view settings and the saved state. Shared by
// the VST3 (plugin/HushPlugin) and the built-in device (HushDevice below), so both save the same state bytes and show
// the same face (ui/views/HushFace).
//
// Message thread only, except that the Hush parameters it writes are the device's atomics. Parameters are known by
// their reference names (ParameterInfo::name, e.g. "Band 1 frequency"): preset files and saved states use those, while
// ParameterInfo::id is the XML-safe form ("Band_1_frequency") the .als and the VST3 host use.

#include "devices/native/Hush.h"

#include <juce_core/juce_core.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace sonora::devices
{

// Settings menu: shared by every instance in the process (VST3s and built-in devices alike) and kept in
// ~/Library/Application Support/Sonora/Hush.settings.
struct HushSettings
{
    std::atomic<bool> showTooltips{true}, renderUltra{false}, lowLatencyAll{false}, listenOnDrag{false}, listenDelta{true},
        frequencyNearCursor{false};
    std::string defaultPreset; // message thread; empty = "Factory default"
    static HushSettings& get();
    static juce::File file();
    void save() const;
};

struct HushPreset
{
    std::string name, category, description;
    std::map<std::string, float> values; // parameter name -> plain value; missing names take their defaults
    bool factory = false;
    juce::File file; // user and imported presets
};

class HushSession
{
public:
    // Loads the preset chosen as the default (preset menu > Set as default), like every new instance.
    explicit HushSession(Hush& hush);

    Hush& hush() { return hush_; }
    const Hush& hush() const { return hush_; }

    // ---- edits from the face
    // Owners hear about edits here: the VST3 tells the host (value + gestures), the built-in device needs nothing.
    std::function<void(int index)> onEdited;
    std::function<void(int index, bool begin)> onGesture;
    void setFromEditor(int index, float plainValue); // clamps, sets, then onEdited
    void beginGesture(int index) { if (onGesture && index >= 0) onGesture(index, true); }
    void endGesture(int index) { if (onGesture && index >= 0) onGesture(index, false); }

    // Text the host and the face show for a plain value ("4.0 kHz", "1.0x", "even", "off"), and back.
    static juce::String formatValue(const ParameterInfo&, float plainValue);
    static float parseValue(const ParameterInfo&, const juce::String& text, float fallback);
    static bool isLogarithmic(const ParameterInfo&); // frequencies, tilts and Q

    // ---- presets
    static const std::vector<HushPreset>& factoryPresets();
    // Sonora's factory presets, then user presets, then the reference plug-in's own factory and user preset files when
    // it is installed (read in place, never copied), then other files of the same JSON layout.
    std::vector<HushPreset> allPresets() const;
    static std::vector<juce::File> referencePresetFolders(); // the installed reference's factory and user folders
    static std::string categoryName(int category);
    static juce::File userPresetFolder();
    static bool readPresetFile(const juce::File&, HushPreset&);
    bool savePreset(const juce::String& name, const juce::String& category, bool overwrite);
    void loadPreset(const HushPreset&);
    const std::string& presetName() const { return presetName_; }
    const std::string& presetCategory() const { return presetCategory_; }
    bool presetModified() const;
    void stepPreset(int delta); // < > buttons
    bool qualityLocked = false, lowLatencyLocked = false;

    // ---- A/B and undo (face)
    int activeSlot() const { return activeSlot_; }
    void selectSlot(int slot);
    void copyToOtherSlot();
    void pushUndo(); // before an edit gesture
    bool undo();
    bool redo();
    bool canUndo() const { return ! undo_[(size_t) activeSlot_].empty(); }
    bool canRedo() const { return ! redo_[(size_t) activeSlot_].empty(); }

    // ---- face state kept with the instance
    float editorScale = 1050.0f / 900.0f; // window size; the reference opens at 1050 x 621
    bool sidePanelOpen = false;
    float graphRange = 9.0f; // dB shown by the reduction graph
    std::string hostName = "Sonora"; // Help > Copy system information

    // The shared settings onto this instance's device; the face reapplies them while it shows (a menu change reaches
    // every instance). `offline`: rendering faster than real time (the VST3 host says so; the app does not yet).
    void applySettings();
    // TODO(PLG-14g): app exports do not set this yet (DeviceProcessor has no offline flag), and the engine compensates a
    // device's latency when the graph is built, so switching linear phase / low latency on the built-in device is
    // compensated only after the next graph rebuild.
    bool offline = false;

    // State: every parameter by name, preset, view settings, the other A/B slot (a juce binary XML block). The VST3's
    // getStateInformation body and the built-in device's .als content are these same bytes.
    std::vector<uint8_t> encodeState() const;
    bool decodeState(const void* data, size_t size, std::string& error);

private:
    using Snapshot = std::vector<float>;
    Snapshot snapshot() const;
    void restore(const Snapshot&);

    Hush& hush_;
    std::string presetName_ = "Factory default", presetCategory_ = "misc";
    Snapshot presetValues_;
    int activeSlot_ = 0;
    std::array<Snapshot, 2> slots_;
    std::array<std::vector<Snapshot>, 2> undo_, redo_;
};

// The built-in Sonora Hush device (catalog "SonoraHush"): the processor plus its session. Also plays a Sonora Hush VST3
// on a computer that does not have it (COL-4e, DeviceFactory createSonoraStandIn).
class HushDevice final : public Hush
{
public:
    static constexpr const char* kContentFormat = "sonora-hush-v1"; // model::NativeContent::format
    HushDevice() : session_(*this) { session_.hostName = "Sonora (built-in device)"; }
    HushSession& session() { return session_; }
    const HushSession& session() const { return session_; }

private:
    HushSession session_;
};

} // namespace sonora::devices

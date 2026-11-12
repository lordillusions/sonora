#pragma once
// PLG-23: building and wiring the ProFace of Sonora's creative devices (Stutter, Sweep, Phase Align) for the device chain
// and for their own window, and the root of that window. Like the floating Hush (HushWindowView) the window follows its
// device through graph rebuilds by key and reports when the device went away.

#include "ui/RootWidget.h"
#include "ui/UiContext.h"
#include "ui/views/ProFace.h"

#include <functional>
#include <optional>
#include <string>

namespace sonora::devices { class DeviceProcessor; }

namespace sonora::ui
{
// The ProFace for a processor (added to `parent`), or null when the device has none.
ProFace* addProFace(Widget& parent, devices::DeviceProcessor* processor);
// Connects a face to the app: MIDI/key mapping, Sweep's sample drop and chooser, Phase Align's sidechain source. The
// context is looked up on use (`context` may not be attached yet when the face is built). Track-level devices only.
void wireProFace(ProFace&, std::function<UiContext*()> context, int trackIndex, int deviceIndex);
// Sweep: puts a sample path into the device's native content (saved with the set), one undoable edit.
void loadSweepSample(UiContext&, int trackIndex, int deviceIndex, const std::string& path);

class ProFaceWindowView final : public RootWidget
{
public:
    ProFaceWindowView(UiContext&, UiContext::WavetableKey);
    const UiContext::WavetableKey& key() const { return key_; }
    void setKey(const UiContext::WavetableKey&);
    void sync();
    ProFace* face() const { return face_; }
    const std::string& title() const { return title_; }
    ProFace::Size designSize() const { return size_; }
    std::function<void(const std::string&)> onTitleChanged;
    std::function<void()> onGone;

    void layout() override;
    void paint(SkCanvas&, const Theme&) override;
    void tick() override;

    struct Site
    {
        int track = -1, device = -1;
        const model::Device* model = nullptr;
        devices::DeviceProcessor* processor = nullptr;
    };
    static std::optional<Site> find(const UiContext&, const UiContext::WavetableKey&);
    static bool isProFaceDevice(const std::string& type);

private:
    bool stale() const;
    void rebuild();

    UiContext::WavetableKey key_;
    ProFace* face_ = nullptr;
    ProFace::Size size_{1000, 600};
    const void* builtSong_ = nullptr;
    const void* builtGraph_ = nullptr;
    uint64_t builtStructure_ = 0;
    bool built_ = false, goneSent_ = false;
    std::string title_;
};
} // namespace sonora::ui

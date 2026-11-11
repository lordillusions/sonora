#include "ui/DeviceMenu.h"

#include "plugins/NativeReplacement.h"
#include "ui/MenuLookAndFeel.h"
#include "ui/UiContext.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cmath>

namespace sonora::ui
{
namespace
{
const model::Device* deviceAt(const UiContext& ctx, int trackIndex, const std::vector<int>& path)
{
    const auto* track = ctx.trackAt(trackIndex);
    if (track == nullptr || path.empty() || path.size() % 2 == 0) return nullptr;
    const std::vector<model::Device>* devices = &track->devices;
    const model::Device* device = nullptr;
    for (size_t i = 0; i < path.size(); i += 2)
    {
        if (path[i] < 0 || (size_t) path[i] >= devices->size()) return nullptr;
        device = &(*devices)[(size_t) path[i]];
        if (i + 1 < path.size())
        {
            if (path[i + 1] < 0 || (size_t) path[i + 1] >= device->chains.size()) return nullptr;
            devices = &device->chains[(size_t) path[i + 1]].devices;
        }
    }
    return device;
}

juce::PopupMenu menuFrom(const std::vector<MenuAction>& actions)
{
    juce::PopupMenu menu;
    for (const auto& action : actions)
    {
        if (action.label.empty()) { menu.addSeparator(); continue; }
        juce::PopupMenu::Item item(juce::String::fromUTF8(action.label.c_str()));
        item.isEnabled = action.enabled;
        item.isTicked = action.ticked;
        item.shortcutKeyDescription = juce::String::fromUTF8(action.shortcut.c_str());
        if (! action.submenu.empty()) item.subMenu = std::make_unique<juce::PopupMenu>(menuFrom(action.submenu));
        else item.action = action.run;
        menu.addItem(std::move(item));
    }
    return menu;
}

juce::PopupMenu createMenu(UiContext& ctx, int track, const std::vector<int>& path)
{
    return menuFrom(deviceContextActions(ctx, track, path));
}
} // namespace

std::vector<MenuAction> deviceContextActions(UiContext& ctx, int track, const std::vector<int>& path)
{
    std::vector<MenuAction> actions;
    const auto* device = deviceAt(ctx, track, path);
    if (device == nullptr) return actions;
    const auto* song = ctx.song;
    const bool editable = ! ctx.recording && ! ctx.recordingRequested && static_cast<bool>(ctx.applyEdit);
    if (path.size() == 1)
    {
        const int id = device->id;
        for (const auto* label : {"Cut", "Copy", "Paste", "Duplicate"}) actions.push_back({label, {}, false});
        actions.push_back({"Delete", [&ctx, song, track, id] {
            if (ctx.song != song || ctx.recording || ctx.recordingRequested) return;
            ctx.selectedTrack = track; ctx.selectedDeviceId = id;
            if (ctx.deleteSelectedDevice() && ctx.modelChanged) ctx.modelChanged();
        }, ! ctx.recording && ! ctx.recordingRequested});
        for (const auto* label : {"Rename", "Edit Info Text"}) actions.push_back({label, {}, false});
        actions.push_back({"", {}, false});
        actions.push_back({"Group", {}, false});
        actions.push_back({"Save as Default Preset", {}, false});
        if (auto sources = sidechainActions(ctx, track, path[0]); ! sources.empty())
        {
            actions.push_back({"", {}, false});
            MenuAction sidechain{"Sidechain", {}, true};
            sidechain.submenu = std::move(sources);
            actions.push_back(std::move(sidechain));
        }
    }
    const auto alternative = plugins::nativeAlternativeFor(*device);
    if (alternative == plugins::NativeAlternative::None || song == nullptr) return actions;
    const std::string target = plugins::nativeAlternativeName(alternative);
    const std::string plugin = device->plugin ? device->plugin->name : std::string();
    const auto* host = ctx.trackAt(track);
    const bool trackEditable = host != nullptr && ! host->frozen && ! host->deactivated;
    if (! actions.empty()) actions.push_back({"", {}, false});
    actions.push_back({"Replace with " + target, [&ctx, song, track, path] {
        if (ctx.song == song) replaceWithNativeDevices(ctx, track, path, false);
    }, editable && trackEditable});
    if (const int count = plugins::countNativeCandidates(*song, alternative); count > 1)
        actions.push_back({"Replace All " + std::to_string(count) + " " + plugin + " in This Set with " + target, [&ctx, song, track, path] {
            if (ctx.song == song) replaceWithNativeDevices(ctx, track, path, true);
        }, editable});
    return actions;
}

bool deviceTakesSidechain(const model::Device& device)
{
    const auto& t = device.type;
    return t == "SonoraHush" || t == "Compressor2" || t == "GlueCompressor" || t == "Gate" || t == "SonoraPhaseAlign";
}

std::vector<MenuAction> sidechainActions(UiContext& ctx, int track, int deviceIndex)
{
    std::vector<MenuAction> actions;
    const auto* host = ctx.trackAt(track);
    if (host == nullptr || deviceIndex < 0 || (size_t) deviceIndex >= host->devices.size() || ctx.song == nullptr) return actions;
    const auto& device = host->devices[(size_t) deviceIndex];
    if (! deviceTakesSidechain(device)) return actions;
    const bool editable = ! ctx.recording && ! ctx.recordingRequested && static_cast<bool>(ctx.editDevice) && ! host->frozen;
    const auto* song = ctx.song;
    const auto route = [&ctx, song, track, deviceIndex](int source, std::string name) {
        if (ctx.song != song || ! ctx.editDevice) return;
        ctx.editDevice(track, deviceIndex, [source](model::Device& d) {
            d.sidechain.on = source >= 0;
            d.sidechain.sourceTrackId = source;
            d.sidechain.tap = model::Sidechain::Tap::PostFx;
            d.sidechain.edited = true;
            // The device's own switch follows: Hush "Sidechain_on", the reference's dynamics "SideChain/OnOff".
            for (auto& p : d.parameters)
                if (p.path == "Sidechain_on" || p.path == "SideChain/OnOff") p.value = source >= 0 ? 1.0 : 0.0;
        }, source >= 0 ? "Sidechain from " + name : "Sidechain off");
        ctx.statusText = source >= 0 ? "Sidechain from " + name : std::string("Sidechain off");
    };
    actions.push_back({"None", [route] { route(-1, {}); }, editable, {}, ! device.sidechain.routed()});
    actions.push_back({"", {}, false});
    for (const auto& t : ctx.song->tracks)
    {
        if (t.id == host->id) continue;
        const std::string name = t.name.empty() ? "Track " + std::to_string(t.id) : t.name;
        const int id = t.id;
        actions.push_back({name, [route, id, name] { route(id, name); }, editable, {},
                           device.sidechain.routed() && device.sidechain.sourceTrackId == id});
    }
    return actions;
}

void showSidechainMenu(UiContext& ctx, int track, int device)
{
    auto actions = sidechainActions(ctx, track, device);
    if (actions.empty()) return;
    actions.insert(actions.begin(), MenuAction{"Sidechain from", {}, false});
    menuFrom(actions).showMenuAsync(juce::PopupMenu::Options());
}

void showDeviceContextMenu(UiContext& ctx, int track, const std::vector<int>& path)
{
    auto menu = createMenu(ctx, track, path);
    if (menu.getNumItems() > 0) menu.showMenuAsync(juce::PopupMenu::Options());
}

juce::Image renderDeviceMenuPreview(UiContext& ctx, int track, const std::vector<int>& path, float scale)
{
    MenuLookAndFeel look; look.configure(ctx.theme, ctx.uiZoom);
    auto menu = createMenu(ctx, track, path);
    int width = 0, height = 4;
    juce::PopupMenu::MenuItemIterator sizes(menu);
    while (sizes.next())
    {
        int w = 0, h = 0;
        look.getIdealPopupMenuItemSize(sizes.getItem().text, sizes.getItem().isSeparator, 0, w, h);
        width = std::max(width, w); height += h;
    }
    juce::Image image(juce::Image::ARGB, std::max(1, (int) std::ceil((float) width * scale)), std::max(1, (int) std::ceil((float) height * scale)), true);
    juce::Graphics graphics(image); graphics.addTransform(juce::AffineTransform::scale(scale));
    look.drawPopupMenuBackground(graphics, width, height);
    juce::PopupMenu::MenuItemIterator rows(menu);
    int y = 2;
    while (rows.next())
    {
        const auto& item = rows.getItem();
        int w = 0, h = 0;
        look.getIdealPopupMenuItemSize(item.text, item.isSeparator, 0, w, h);
        look.drawPopupMenuItem(graphics, {0, y, width, h}, item.isSeparator, item.isEnabled, false, item.isTicked, item.subMenu != nullptr,
                               item.text, item.shortcutKeyDescription, item.image.get(), nullptr);
        y += h;
    }
    return image;
}

int replaceWithNativeDevices(UiContext& ctx, int track, const std::vector<int>& path, bool all)
{
    if (ctx.song == nullptr || ! ctx.applyEdit || ctx.recording || ctx.recordingRequested) return 0;
    const auto* song = ctx.song;
    const auto* device = deviceAt(ctx, track, path);
    const auto alternative = device != nullptr ? plugins::nativeAlternativeFor(*device) : plugins::NativeAlternative::None;
    if (alternative == plugins::NativeAlternative::None) return 0;
    // The running instances' states first: edits made since the load carry over (a plug-in that reports nothing keeps
    // the state the set had, plugins::pluginChunkUnreported).
    if (ctx.capturePluginStates) ctx.capturePluginStates();
    if (ctx.song != song) return 0;
    std::vector<plugins::NativeReplacement> replacements;
    if (all) replacements = plugins::allNativeReplacements(*song, alternative);
    else if (auto one = plugins::nativeReplacementAt(*song, track, path)) replacements.push_back(std::move(*one));
    if (replacements.empty())
    {
        ctx.statusText = "Nothing to replace here (frozen or deactivated tracks cannot be edited)";
        return 0;
    }
    auto command = plugins::replaceWithNative(*song, replacements);
    if (! command.apply) return 0;
    ctx.applyEdit(std::move(command));
    if (path.size() == 1)
        if (const auto* host = ctx.trackAt(track); host != nullptr && (size_t) path[0] < host->devices.size())
        {
            ctx.selectedTrack = track;
            ctx.selectedDeviceId = host->devices[(size_t) path[0]].id; // the swap gave the device a fresh id
        }
    reportNativeReplacements(ctx, replacements);
    if (ctx.modelChanged) ctx.modelChanged();
    return (int) replacements.size();
}

void reportNativeReplacements(UiContext& ctx, const std::vector<plugins::NativeReplacement>& replacements)
{
    size_t notes = 0;
    for (const auto& r : replacements)
        for (const auto& note : r.notes)
        {
            ctx.buildLog.push_back(r.pluginName + " → " + plugins::nativeAlternativeName(r.alternative) + " (" + r.trackName + "): " + note);
            ++notes;
        }
    ctx.statusText = "Replaced " + std::to_string(replacements.size()) + " plug-in" + (replacements.size() == 1 ? "" : "s")
                     + " with Sonora devices" + (notes > 0 ? " — " + std::to_string(notes) + " compatibility notes" : std::string());
    if (ctx.requestRepaint) ctx.requestRepaint();
}

} // namespace sonora::ui

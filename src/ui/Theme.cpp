#include "ui/Theme.h"

#include "core/LiveColors.h"

#include <juce_core/juce_core.h>

namespace sonora::ui
{

namespace
{
SkColor rgb(uint32_t v)
{
    return SkColorSetRGB((v >> 16) & 0xff, (v >> 8) & 0xff, v & 0xff);
}

SkColor rgba(uint32_t v)
{
    return SkColorSetARGB(v & 0xff, (v >> 24) & 0xff, (v >> 16) & 0xff, (v >> 8) & 0xff);
}

// Parses "#rrggbb" or "#rrggbbaa".
bool parseHex(const juce::String& s, SkColor& out)
{
    if (! s.startsWithChar('#'))
        return false;
    const auto hex = s.substring(1);
    const auto v = (uint32_t) hex.getHexValue64();
    if (hex.length() == 6)
        out = rgb(v);
    else if (hex.length() == 8)
        out = rgba(v);
    else
        return false;
    return true;
}
} // namespace

Theme::Theme()
{
    // Approximation of Live 12's light, cool, high-contrast look, measured from screenshots of a
    // running Live. Import a real .ask (Options > Load Live Theme) for exact values or another theme.
    const std::pair<const char*, uint32_t> defaults[] = {
        {"Desktop", 0x737782},
        {"SurfaceArea", 0x575b67},
        {"SurfaceBackground", 0xa4a8b1},
        {"SurfaceHighlight", 0xc0c3cb},
        {"DetailViewBackground", 0xcbced4},
        {"ControlBackground", 0xd4d6dc},
        {"ControlForeground", 0x020308},
        {"ControlDisabled", 0xf3f4f5},
        {"ControlFillHandle", 0xd4d6dc},
        {"ControlOnForeground", 0x020308},
        {"ControlOffForeground", 0x020308},
        {"ControlContrastFrame", 0x313541},
        {"ControlSelectionFrame", 0x313541},
        {"ControlTextBack", 0xe4e5e9},
        {"TextDisabled", 0x575b67},
        {"ChosenDefault", 0xffb901},
        {"ChosenAlternative", 0x00eeff},
        {"ChosenRecord", 0xff595f},
        {"ChosenPlay", 0x00faa3},
        {"ChosenAlert", 0xff7d43},
        {"ChosenPreListen", 0x1a7df1},
        {"RangeDefault", 0x00eeff},
        {"ViewControlOn", 0xffb901},
        {"ViewControlOff", 0xa4a8b1},
        {"ClipSlotButton", 0x3f434f},
        {"ClipText", 0x000000},
        {"ClipBorder", 0x000000},
        {"SceneContrast", 0x494d59},
        {"SelectionBackground", 0xcdf8ff},
        {"SelectionForeground", 0x020308},
        {"StandbySelectionBackground", 0xabc6cb},
        {"SelectionFrame", 0x1e222d},
        {"BrowserBar", 0xa4a8b1},
        {"TreeRowCategoryForeground", 0x313541},
        {"TreeColumnHeadBackground", 0xc0c3cb},
        {"TreeColumnHeadForeground", 0x1e222d},
        {"TreeColumnHeadControl", 0x575b67},
        {"DisplayBackground", 0x3f434f},
        {"MeterBackground", 0x090c16},
        {"LoopColor", 0x1e222d},
        {"ArrangementRulerMarkings", 0x1e222d},
        {"GridLabel", 0x1e222d},
        {"WaveformColor", 0x070b14},
        {"AutomationColor", 0xea3c3c},
        {"Poti", 0x090c16},
        {"PotiNeedle", 0x090c16},
        {"DeactivatedPoti", 0x575b67},
        {"TransportOffBackground", 0xa4a8b1},
        {"TransportOffForeground", 0x020308},
        {"TransportSelectionBackground", 0xd4d6dc},
        {"MainViewFocusIndicator", 0x1e222d},
        {"ScrollbarInnerHandle", 0x3f434f},
        {"SpectrumDefaultColor", 0xbfc2cb},
        {"RetroDisplayBackground", 0x090c16},
        {"RetroDisplayForeground", 0xffb901},
        {"RetroDisplayForeground2", 0x00eeff},
        {"RetroDisplayScaleText", 0xd5d7dd},
        {"SampleDisplayBackground", 0x2e6c79}, // the sampler's sample view (measured)
        {"PianoBlackKey", 0x3d414d},
        {"PianoWhiteKey", 0xf3f4f5},
        {"PianoKeySeparator", 0xa4a8b1},
        {"Modulation", 0x008cad},
        {"RangeEditField", 0x00cadb},
    };
    for (const auto& [k, v] : defaults)
        colors_[k] = rgb(v);
    colors_["GridLineBase"] = SkColorSetARGB(0x3f, 0x00, 0x04, 0x08);
    colors_["ArrangerGridTiles"] = SkColorSetARGB(0x11, 0x00, 0x00, 0x0f);
    colors_["ShadowDark"] = SkColorSetARGB(0x54, 0x03, 0x03, 0x09);
}

void Theme::useBuiltin(bool dark)
{
    Theme fresh;
    fresh.metrics = metrics;
    if (dark)
    {
        const std::pair<const char*, uint32_t> overrides[] = {
            {"Desktop", 0x25272d}, {"SurfaceArea", 0x17191e}, {"SurfaceBackground", 0x474b55},
            {"SurfaceHighlight", 0x606571}, {"DetailViewBackground", 0x545964}, {"ControlBackground", 0x6a707b},
            {"ControlForeground", 0xf0f2f6}, {"ControlDisabled", 0x41454f}, {"ControlTextBack", 0x373c46},
            {"ControlContrastFrame", 0x20232a}, {"TextDisabled", 0xb1b6c1}, {"ClipSlotButton", 0xbfc5d0},
            {"BrowserBar", 0x474b55}, {"TreeRowCategoryForeground", 0xe0e4ec}, {"TreeColumnHeadBackground", 0x606571},
            {"TreeColumnHeadForeground", 0xf0f2f6}, {"TreeColumnHeadControl", 0xb1b6c1},
            {"TransportOffBackground", 0x474b55}, {"TransportOffForeground", 0xf0f2f6},
            {"Poti", 0xdde2eb}, {"PotiNeedle", 0xdde2eb}, {"DeactivatedPoti", 0x818794},
            {"ArrangementRulerMarkings", 0xdde2eb}, {"GridLabel", 0xdde2eb}, {"WaveformColor", 0xe0e5ef},
            {"ScrollbarInnerHandle", 0xb1b6c1}, {"MainViewFocusIndicator", 0x090b10},
        };
        for (const auto& [key, value] : overrides) fresh.colors_[key] = rgb(value);
        fresh.sourceName = "Built-in Dark";
    }
    *this = std::move(fresh);
}

bool Theme::loadAsk(const std::string& path)
{
    auto xml = juce::XmlDocument::parse(juce::File(juce::String::fromUTF8(path.c_str())));
    if (xml == nullptr)
        return false;
    const auto* theme = xml->getChildByName("Theme");
    if (theme == nullptr)
        return false;

    // Only the first occurrence of a key wins (meter sub-themes repeat names).
    std::unordered_map<std::string, bool> seen;
    for (const auto* e : theme->getChildIterator())
    {
        SkColor c;
        const auto key = e->getTagName().toStdString();
        if (! seen[key] && parseHex(e->getStringAttribute("Value"), c))
        {
            colors_[key] = c;
            seen[key] = true;
        }
    }
    sourceName = juce::File(juce::String::fromUTF8(path.c_str())).getFileNameWithoutExtension().toStdString();
    return true;
}

SkColor Theme::color(const std::string& token) const
{
    if (auto it = colors_.find(token); it != colors_.end())
        return it->second;
    return SK_ColorMAGENTA; // makes missing tokens obvious during development
}

SkColor Theme::paletteColor(int index)
{
    return rgb(model::livePaletteColour(index));
}

} // namespace sonora::ui

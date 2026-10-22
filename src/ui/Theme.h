#pragma once
// Colour tokens + metrics for the UI.
//
// Token names deliberately match the keys used in Live's .ask theme files
// (e.g. "SurfaceArea", "ChosenDefault"), so Theme::loadAsk() can import any Live 12
// theme the user has installed and make Sonora look identical to their setup.
// Built-in defaults approximate Live 12's "Default Light Cool High".
// See docs/UI_GUIDE.md for which token is used where.

#include "include/core/SkColor.h"

#include <string>
#include <unordered_map>

namespace sonora::ui
{

class Theme
{
public:
    Theme(); // built-in light defaults

    // Imports colours from a Live .ask file. Unknown keys are ignored, missing keys keep
    // their current value. Returns false when the file cannot be parsed.
    bool loadAsk(const std::string& path);

    SkColor color(const std::string& token) const;
    void set(const std::string& token, SkColor c) { colors_[token] = c; }

    // Live track/clip palette colour for a <Color Value="N"/> index.
    static SkColor paletteColor(int index);

    // ---- metrics (logical pixels at 100 % zoom)
    struct Metrics
    {
        // Live 12 draws rounded panels on the "Desktop" colour. controlBarHeight/statusBarHeight
        // include the gap to the neighbouring panel, so panels start exactly at controlBarHeight.
        float controlBarHeight = 36;
        float statusBarHeight = 32;
        float outerMargin = 8;
        float panelGap = 6;
        float panelRadius = 4;
        float browserWidth = 400;
        float browserSidebarWidth = 160; // Library / Places column inside the browser
        float clipPanelWidth = 170;      // Clip view properties panel
        float velocityLaneHeight = 44;   // note editor velocity / probability lane
        float detailHeight = 200;
        float sessionTrackWidth = 95;
        float sessionSlotHeight = 18;
        float sessionHeaderHeight = 24;
        float sessionMixerHeight = 210;
        float sessionVolumeHeight = 120; // fader section of the Session mixer (resizable)
        float sceneColumnWidth = 104;
        float arrangementRulerHeight = 48;
        float arrangementLaneHeight = 87;
        float arrangementHeaderWidth = 306;
        float arrangementFooterHeight = 18;
        float fontSize = 10.0f;
        float smallFontSize = 9.0f;
    } metrics;

    std::string sourceName = "Built-in Light";
    void useBuiltin(bool dark);

private:
    std::unordered_map<std::string, SkColor> colors_;
};

} // namespace sonora::ui

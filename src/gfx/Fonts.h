#pragma once
// UI font access. Default: the macOS system UI font through CoreText; DirectWrite (Segoe UI) on
// Windows and fontconfig (Noto/DejaVu/Liberation Sans) on Linux.
// Set the environment variable SONORA_UI_FONT=/path/to/font.ttf to use another face
// (see docs/UI_GUIDE.md "Typography").

#include "include/core/SkFont.h"
#include "include/core/SkFontMetrics.h"
#include "include/core/SkFontTypes.h"
#include "include/core/SkFontMgr.h"
#include "include/core/SkTypeface.h"

#include <string_view>

namespace sonora::gfx
{

sk_sp<SkFontMgr> fontManager();

// Cached fonts. `bold` selects semibold emphasis (or the explicit SONORA_UI_FONT_BOLD override).
SkFont uiFont(float size, bool bold = false);

float measureText(std::string_view text, float size, bool bold = false);

} // namespace sonora::gfx

#include "gfx/Fonts.h"

#include "include/core/SkData.h"
#if defined(__APPLE__)
#include "include/ports/SkFontMgr_mac_ct.h"
#elif defined(_WIN32)
#include "include/ports/SkTypeface_win.h"
#else
#include "include/ports/SkFontMgr_fontconfig.h"
#include "include/ports/SkFontScanner_FreeType.h"
#endif

#include <cstdlib>

namespace sonora::gfx
{

sk_sp<SkFontMgr> fontManager()
{
#if defined(__APPLE__)
    static sk_sp<SkFontMgr> mgr = SkFontMgr_New_CoreText(nullptr);
#elif defined(_WIN32)
    static sk_sp<SkFontMgr> mgr = SkFontMgr_New_DirectWrite();
#else
    static sk_sp<SkFontMgr> mgr = SkFontMgr_New_FontConfig(nullptr, SkFontScanner_Make_FreeType());
#endif
    return mgr;
}

namespace
{
sk_sp<SkTypeface> loadTypeface(bool bold)
{
    auto mgr = fontManager();
    if (const char* path = std::getenv(bold ? "SONORA_UI_FONT_BOLD" : "SONORA_UI_FONT"))
        if (auto tf = mgr->makeFromFile(path))
            return tf;

    const SkFontStyle style = bold ? SkFontStyle(SkFontStyle::kSemiBold_Weight, SkFontStyle::kNormal_Width, SkFontStyle::kUpright_Slant)
                                   : SkFontStyle::Normal();
    // macOS faces first; then Windows (Segoe UI) and common Linux sans faces (COM-7).
    for (const char* family : {"SF Pro Text", "SF Pro", ".AppleSystemUIFont", "Helvetica Neue", "Helvetica", "Arial",
                               "Segoe UI", "Inter", "Noto Sans", "DejaVu Sans", "Liberation Sans"})
        if (auto tf = mgr->matchFamilyStyle(family, style))
            return tf;
    return mgr->legacyMakeTypeface(nullptr, style);
}
} // namespace

SkFont uiFont(float size, bool bold)
{
    static sk_sp<SkTypeface> regular = loadTypeface(false);
    static sk_sp<SkTypeface> boldFace = loadTypeface(true);
    SkFont f(bold ? boldFace : regular, size);
    f.setEdging(SkFont::Edging::kSubpixelAntiAlias);
    f.setSubpixel(true);
    f.setHinting(SkFontHinting::kSlight);
    return f;
}

float measureText(std::string_view text, float size, bool bold)
{
    return uiFont(size, bold).measureText(text.data(), text.size(), SkTextEncoding::kUTF8);
}

} // namespace sonora::gfx

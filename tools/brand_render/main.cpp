// brand_render: writes the Sonora brand assets (UI-51) drawn by src/gfx/Brand.cpp.
//
//   brand_render <out-dir>                  icon PNGs (16..1024), icon/mark/wordmark SVGs, splash and cursor previews
//   brand_render <out-dir> --status "TEXT"  splash preview with that status line (and --progress 0..1)
//
// resources/brand/ holds the committed output; src/CMakeLists.txt uses resources/brand/icon-1024.png as ICON_BIG.

#include "gfx/Brand.h"

#include "include/core/SkCanvas.h"
#include "include/core/SkPixmap.h"
#include "include/core/SkStream.h"
#include "include/core/SkSurface.h"
#include "include/encode/SkPngEncoder.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>

namespace brand = sonora::gfx::brand;

namespace
{
bool writePng(const std::string& path, int w, int h, float scale, const std::function<void(SkCanvas&)>& draw, SkColor background = 0)
{
    auto surface = SkSurfaces::Raster(SkImageInfo::MakeN32Premul(w, h));
    if (! surface) return false;
    auto* canvas = surface->getCanvas();
    canvas->clear(background);
    canvas->scale(scale, scale);
    draw(*canvas);
    SkPixmap pixmap;
    if (! surface->peekPixels(&pixmap)) return false;
    SkFILEWStream out(path.c_str());
    const bool ok = out.isValid() && SkPngEncoder::Encode(&out, pixmap, {});
    std::printf("%s %s\n", ok ? "wrote" : "FAILED", path.c_str());
    return ok;
}

bool writeText(const std::string& path, const std::string& text)
{
    std::ofstream out(path, std::ios::binary);
    out << text;
    std::printf("%s %s\n", out ? "wrote" : "FAILED", path.c_str());
    return (bool) out;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: brand_render <out-dir> [--status TEXT] [--progress 0..1]\n");
        return 2;
    }
    const std::filesystem::path dir = argv[1];
    brand::SplashState splash;
    splash.version = "Version 1.0 (1)";
    splash.status = "Starting plug-ins 3 of 12: Serum 2";
    splash.timeSeconds = 0.55;
    for (int i = 2; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--status" && i + 1 < argc) splash.status = argv[++i];
        else if (a == "--progress" && i + 1 < argc) splash.progress = std::stof(argv[++i]);
    }
    std::filesystem::create_directories(dir);
    bool ok = true;

    for (int size : {16, 32, 64, 128, 256, 512, 1024})
        ok &= writePng((dir / ("icon-" + std::to_string(size) + ".png")).string(), size, size, 1.0f,
                       [size](SkCanvas& c) { brand::drawAppIcon(c, (float) size); });

    ok &= writeText((dir / "icon.svg").string(), brand::markSvg(true));
    ok &= writeText((dir / "mark.svg").string(), brand::markSvg(false));
    ok &= writeText((dir / "wordmark-white.svg").string(), brand::wordmarkSvg(SK_ColorWHITE));
    ok &= writeText((dir / "wordmark-black.svg").string(), brand::wordmarkSvg(SkColorSetRGB(0x11, 0x11, 0x11)));

    // Previews (not committed): splash and the save prompt (UI-44) at 2x, wordmark on white, cursor and activity frames.
    ok &= writePng((dir / "preview-splash.png").string(), (int) brand::splashWidth * 2, (int) brand::splashHeight * 2, 2.0f,
                   [&](SkCanvas& c) { brand::drawSplash(c, brand::splashWidth, brand::splashHeight, splash); });
    brand::PromptState prompt;
    prompt.title = "Save changes to \u201CPsyform_Neelix_Final\u201D?";
    prompt.message = "Your changes will be lost if you don\u2019t save them.";
    prompt.buttons = {{"Save", true, false}, {"Don\u2019t Save", false, true}, {"Cancel", false, false}};
    prompt.focused = 0;
    prompt.hovered = 2;
    const float promptH = brand::promptHeight(prompt);
    ok &= writePng((dir / "preview-prompt.png").string(), (int) brand::promptWidth * 2, (int) promptH * 2, 2.0f,
                   [&](SkCanvas& c) { brand::drawPrompt(c, brand::promptWidth, promptH, prompt); });
    // About window (COM-8), with sample supporters and with none.
    for (const bool sample : {true, false})
    {
        brand::AboutState about;
        about.version = "Version 0.1.2";
        about.credit = "Created by Wellington Chaves \u00B7 Lordillusions";
        about.emptyText = "No supporters listed yet. Monthly supporters on Ko-fi can have their name here and on sonoradaw.com.";
        if (sample)
            about.groups = {{"Studio", {"Example Studio"}}, {"Producers", {"Ana B.", "Kenji M."}},
                            {"Supporters", {"Lucas R.", "Mika T.", "Jordan P.", "Sam O.", "Chiara L.", "Noah W."}}};
        about.hovered = brand::AboutSupport;
        const float h = brand::aboutHeight(about);
        ok &= writePng((dir / (sample ? "preview-about.png" : "preview-about-empty.png")).string(), (int) brand::aboutWidth * 2,
                       (int) h * 2, 2.0f, [&](SkCanvas& c) { brand::drawAbout(c, brand::aboutWidth, h, about); });
    }
    ok &= writePng((dir / "preview-wordmark.png").string(), 1200, 320, 2.0f, [](SkCanvas& c) {
        brand::drawWordmark(c, 40, 110, 70, SkColorSetRGB(0x11, 0x11, 0x11));
    }, SK_ColorWHITE);
    ok &= writePng((dir / "preview-cursor.png").string(), 8 * brand::busyCursorSize * 4, brand::busyCursorSize * 4, 4.0f, [](SkCanvas& c) {
        for (int f = 0; f < 8; ++f)
        {
            c.save();
            c.translate((float) (f * brand::busyCursorSize), 0);
            brand::drawBusyCursor(c, f * 0.09);
            c.restore();
        }
    }, SkColorSetRGB(0xb4, 0xb4, 0xb4));
    return ok ? 0 : 1;
}

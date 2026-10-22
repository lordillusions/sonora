#pragma once
// Live's 70-entry clip/track colour palette, indexed by the <Color Value="N"/> in .als.
// Values are an approximation of Live 10+'s palette (5 rows x 14 columns).
// TODO(ROADMAP UI-12): verify each swatch against Live's colour picker.

#include <cstdint>

namespace sonora::model
{

constexpr int kLivePaletteSize = 70;

// 0xRRGGBB
uint32_t livePaletteColour(int index);

} // namespace sonora::model

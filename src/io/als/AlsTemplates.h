#pragma once
// Authored from the documented file format; no presets or application assets (UI-7).
#include "core/Model.h"

namespace sonora::io
{
std::string audioClipTemplate(const model::Clip& clip);
std::string deviceTemplate(const model::Device& device);
} // namespace sonora::io

#pragma once
// DEV-18d: native media/curve persistence and graph preparation. Message/load thread only.
#include "core/Model.h"
#include "dsp/CurveGraph.h"
#include "devices/native/WavetableEffects.h"

#include <string>
#include <memory>
#include <vector>

namespace sonora::devices
{
class WavetableSynth;
class WavetableData;
class OscillatorMedia;
class OscillatorSample;

bool storeWavetableTable(model::Device&, int oscillator, const WavetableData&, std::string& error);
bool storeWavetableVoiceRate(model::Device&, int factor, std::string& error);
bool storeWavetableNoise(model::Device&, std::shared_ptr<const OscillatorSample>, std::string& error,
                         bool preferFileReferences = false);
bool storeWavetableLfo(model::Device&, int lfo, const dsp::CurveGraph&, std::string& error);
bool storeWavetableModulationCurve(model::Device&, int slot, bool auxiliary, const dsp::CurveGraph&,
                                  bool enabled, std::string& error);
bool storeOscillatorMedia(model::Device&, int oscillator, const OscillatorMedia&, std::string& error,
                          bool preferFileReferences = false);
bool storeWavetableEffects(model::Device&, const WavetableEffects::Configuration&, std::string& error);
bool readWavetableEffects(const model::Device&, WavetableEffects::Configuration&, std::string& error);
// Restores immutable content before graph publication; unsupported content adds warnings without failing a song.
void prepareWavetableContent(WavetableSynth&, const model::Device&, std::vector<std::string>& warnings);
} // namespace sonora::devices

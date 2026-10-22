#include "core/DeviceCatalog.h"
#include "core/Model.h"

#include <array>

namespace sonora::model
{

namespace
{
using R = DeviceRole;

// Keep sorted by usage in the reference projects, most used first.
// When you implement a device, flip `implemented` and register it in
// src/devices/DeviceFactory.cpp.
constexpr std::array kCatalog{
    CatalogEntry{"OriginalSimpler", "Simpler", R::Instrument, true},
    CatalogEntry{"Eq8", "EQ Eight", R::AudioEffect, true},
    CatalogEntry{"Reverb", "Reverb", R::AudioEffect, true},
    CatalogEntry{"MultibandDynamics", "Multiband Dynamics", R::AudioEffect, true},
    CatalogEntry{"Compressor2", "Compressor", R::AudioEffect, true},
    CatalogEntry{"StereoGain", "Utility", R::AudioEffect, true},
    CatalogEntry{"Delay", "Delay", R::AudioEffect, true},
    CatalogEntry{"AutoFilter", "Auto Filter", R::AudioEffect, true},
    CatalogEntry{"AutoFilter2", "Auto Filter", R::AudioEffect, true},
    CatalogEntry{"FilterEQ3", "EQ Three", R::AudioEffect, true},
    CatalogEntry{"SpectrumAnalyzer", "Spectrum", R::AudioEffect, false},
    CatalogEntry{"Limiter", "Limiter", R::AudioEffect, true},
    CatalogEntry{"GlueCompressor", "Glue Compressor", R::AudioEffect, true},
    CatalogEntry{"Saturator", "Saturator", R::AudioEffect, false},
    CatalogEntry{"Chorus2", "Chorus-Ensemble", R::AudioEffect, false},
    CatalogEntry{"PhaserNew", "Phaser-Flanger", R::AudioEffect, false},
    CatalogEntry{"AutoPan", "Auto Pan", R::AudioEffect, true},
    CatalogEntry{"Redux2", "Redux", R::AudioEffect, true},
    CatalogEntry{"Tuner", "Tuner", R::AudioEffect, true},
    CatalogEntry{"Gate", "Gate", R::AudioEffect, true},
    CatalogEntry{"Echo", "Echo", R::AudioEffect, false},
    CatalogEntry{"Hybrid", "Hybrid Reverb", R::AudioEffect, false},
    CatalogEntry{"AutoPan2", "Auto Pan-Tremolo", R::AudioEffect, false},
    CatalogEntry{"Roar", "Roar", R::AudioEffect, false},
    CatalogEntry{"DrumCell", "Drum Sampler", R::Instrument, false},
    CatalogEntry{"InstrumentVector", "Wavetable", R::Instrument, false},
    CatalogEntry{"MultiSampler", "Sonora Sampler", R::Instrument, true},
    CatalogEntry{"SonoraFm", "Sonora FM", R::Instrument, true},
    CatalogEntry{"SonoraVector", "Sonora Vector", R::Instrument, true},
    CatalogEntry{"SonoraResynth", "Sonora Resynth", R::Instrument, true},
    CatalogEntry{"SonoraImpulse", "Sonora Impulse", R::Instrument, true},
    CatalogEntry{"SonoraEcho", "Sonora Echo", R::AudioEffect, true},
    CatalogEntry{"SonoraWavetable", "Sonora Prism", R::Instrument, true}, // tag kept: sets saved before the rename
    CatalogEntry{"SonoraLfo", "Sonora LFO", R::AudioEffect, true},
    CatalogEntry{"SonoraCurveLfo", "Sonora Curve LFO", R::AudioEffect, true},
    CatalogEntry{"SonoraVelocity", "Sonora Velocity", R::MidiEffect, true},
    CatalogEntry{"InstrumentGroupDevice", "Sonora Instrument Rack", R::Instrument, true},
    CatalogEntry{"DrumGroupDevice", "Sonora Drum Rack", R::Instrument, true},
    CatalogEntry{"AudioEffectGroupDevice", "Audio Effect Rack", R::AudioEffect, true},
    CatalogEntry{"Operator", "Operator", R::Instrument, false},
    CatalogEntry{"UltraAnalog", "Analog", R::Instrument, false},
    CatalogEntry{"Drift", "Drift", R::Instrument, false},
    CatalogEntry{"InstrumentMeld", "Meld", R::Instrument, false},
    CatalogEntry{"InstrumentImpulse", "Impulse", R::Instrument, false},
    CatalogEntry{"Amp", "Amp", R::AudioEffect, false},
    CatalogEntry{"AutoShift", "Auto Shift", R::AudioEffect, false},
    CatalogEntry{"BeatRepeat", "Beat Repeat", R::AudioEffect, false},
    CatalogEntry{"Cabinet", "Cabinet", R::AudioEffect, false},
    CatalogEntry{"ChannelEq", "Channel EQ", R::AudioEffect, false},
    CatalogEntry{"Chorus", "Chorus (Legacy)", R::AudioEffect, false},
    CatalogEntry{"Collision", "Collision", R::Instrument, false},
    CatalogEntry{"Corpus", "Corpus", R::AudioEffect, false},
    CatalogEntry{"DrumBuss", "Drum Buss", R::AudioEffect, false},
    CatalogEntry{"Erosion", "Erosion (Legacy)", R::AudioEffect, false},
    CatalogEntry{"Erosion2", "Erosion", R::AudioEffect, false},
    CatalogEntry{"FilterDelay", "Filter Delay", R::AudioEffect, false},
    CatalogEntry{"Flanger", "Flanger (Legacy)", R::AudioEffect, false},
    CatalogEntry{"FrequencyShifter", "Frequency Shifter (Legacy)", R::AudioEffect, false},
    CatalogEntry{"GrainDelay", "Grain Delay", R::AudioEffect, false},
    CatalogEntry{"Looper", "Looper", R::AudioEffect, false},
    CatalogEntry{"LoungeLizard", "Electric", R::Instrument, false},
    CatalogEntry{"Overdrive", "Overdrive", R::AudioEffect, false},
    CatalogEntry{"Pedal", "Pedal", R::AudioEffect, false},
    CatalogEntry{"Phaser", "Phaser (Legacy)", R::AudioEffect, false},
    CatalogEntry{"Redux", "Redux (Legacy)", R::AudioEffect, false},
    CatalogEntry{"Resonator", "Resonators", R::AudioEffect, false},
    CatalogEntry{"Shifter", "Shifter", R::AudioEffect, false},
    CatalogEntry{"Spectral", "Spectral Time", R::AudioEffect, false},
    CatalogEntry{"Transmute", "Spectral Resonator", R::AudioEffect, false},
    CatalogEntry{"StringStudio", "Tension", R::Instrument, false},
    CatalogEntry{"Tube", "Dynamic Tube", R::AudioEffect, false},
    CatalogEntry{"Vinyl", "Vinyl Distortion", R::AudioEffect, false},
    CatalogEntry{"Vocoder", "Vocoder", R::AudioEffect, false},
    CatalogEntry{"MidiCcControl", "CC Control", R::MidiEffect, false},
    CatalogEntry{"MidiNoteLength", "Note Length", R::MidiEffect, true},
    CatalogEntry{"MidiEffectGroupDevice", "MIDI Effect Rack", R::MidiEffect, false},
    CatalogEntry{"MidiArpeggiator", "Arpeggiator", R::MidiEffect, true},
    CatalogEntry{"MidiChord", "Chord", R::MidiEffect, true},
    CatalogEntry{"MidiScale", "Scale", R::MidiEffect, true},
    CatalogEntry{"MidiPitcher", "Pitch", R::MidiEffect, true},
    CatalogEntry{"MidiVelocity", "Velocity", R::MidiEffect, true},
    CatalogEntry{"MidiRandom", "Random", R::MidiEffect, true},
};

// DEV-23: Max for Live patches with a native replacement. `xmlTag` is a browser key ("max:" + .amxd name); the device
// itself is an MxDevice* element (Device::maxPatch). Kept out of kCatalog: these have no automatable parameters, and the
// catalogue's users (agent control, parameter tests) require them.
constexpr std::string_view kMaxPrefix = "max:";
constexpr std::array kMaxReplacements{
    CatalogEntry{"max:Swiss Army Meter", "Swiss Army Meter", R::AudioEffect, true},
};
} // namespace

DeviceRole deviceRole(const Device& device)
{
    if (device.plugin) return device.plugin->isInstrument ? DeviceRole::Instrument : DeviceRole::AudioEffect;
    if (const auto* entry = findCatalogEntry(device.type)) return entry->role;
    return DeviceRole::Unknown;
}

int instrumentIndex(const Track& track)
{
    for (size_t i = 0; i < track.devices.size(); ++i)
        if (deviceRole(track.devices[i]) == DeviceRole::Instrument) return (int) i;
    return -1;
}

bool isMidiOnly(const Track& track)
{
    return track.kind == TrackKind::Midi && instrumentIndex(track) < 0;
}

const CatalogEntry* findCatalogEntry(std::string_view xmlTag)
{
    for (const auto& e : kCatalog)
        if (e.xmlTag == xmlTag)
            return &e;
    for (const auto& e : kMaxReplacements)
        if (e.xmlTag == xmlTag)
            return &e;
    return nullptr;
}

std::span<const CatalogEntry> deviceCatalog() { return kCatalog; }
std::span<const CatalogEntry> maxForLiveReplacements() { return kMaxReplacements; }

std::string_view deviceCategory(const CatalogEntry& entry)
{
    const auto tag = entry.xmlTag;
    if (isRackTag(tag)) return "Racks";
    if (entry.role == DeviceRole::Instrument)
        return tag == "OriginalSimpler" || tag == "MultiSampler" || tag == "SonoraImpulse" ? "Sampling" : "Synthesis";
    if (entry.role == DeviceRole::MidiEffect) return "Note & Velocity";
    if (tag == "Eq8" || tag == "FilterEQ3" || tag == "AutoFilter" || tag == "AutoFilter2") return "EQ & Filters";
    if (tag == "Reverb" || tag == "Delay" || tag == "SonoraEcho") return "Delay & Reverb";
    if (tag == "Compressor2" || tag == "GlueCompressor" || tag == "Limiter" || tag == "MultibandDynamics" || tag == "Gate") return "Dynamics";
    if (tag == "SonoraLfo" || tag == "SonoraCurveLfo" || tag == "AutoPan" || tag == "AutoPan2") return "Modulation";
    return "Utilities";
}

bool isRackTag(std::string_view t)
{
    return t == "InstrumentGroupDevice" || t == "AudioEffectGroupDevice"
           || t == "MidiEffectGroupDevice" || t == "DrumGroupDevice";
}

std::string_view maxForLiveNativeName(std::string_view patchName)
{
    for (const auto& e : kMaxReplacements)
        if (e.xmlTag.substr(kMaxPrefix.size()) == patchName)
            return e.displayName;
    return {};
}

bool isMaxForLiveTag(std::string_view t)
{
    return t == "MxDeviceAudioEffect" || t == "MxDeviceInstrument" || t == "MxDeviceMidiEffect";
}

} // namespace sonora::model

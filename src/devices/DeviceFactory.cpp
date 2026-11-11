#include "devices/DeviceFactory.h"

#include <cstdlib>

#include "core/DeviceCatalog.h"

#include <algorithm>
#include <cmath>
#include "devices/PlaceholderSynth.h"
#include "devices/RackProcessor.h"
#include "devices/native/AutoFilter.h"
#include "devices/native/Bands.h"
#include "devices/native/ChannelEq.h"
#include "devices/native/Delay.h"
#include "devices/native/Compressor.h"
#include "devices/native/EqEight.h"
#include "devices/native/Limiter.h"
#include "devices/native/Reverb.h"
#include "devices/native/Redux.h"
#include "devices/native/ClassicEffects.h"
#include "devices/native/SwissArmyMeter.h"
#include "devices/native/Tuner.h"
#include "devices/native/Simpler.h"
#include "devices/native/Sampler.h"
#include "devices/native/FmSynth.h"
#include "devices/native/VectorSynth.h"
#include "devices/native/Tension.h"
#include "devices/native/WavetableInstrument.h"
#include "devices/native/WavetableSprites.h"
#include "devices/native/Resynth.h"
#include "devices/native/Impulse.h"
#include "devices/native/Echo.h"
#include "devices/native/WavetableSynth.h"
#include "devices/native/WavetableContent.h"
#include "devices/native/LfoShaper.h"
#include "devices/native/CurveLfo.h"
#include "devices/native/HushSession.h"
#include "devices/native/Kickstract.h"
#include "devices/native/Stutter.h"
#include "devices/native/BeatRepeat.h"
#include "devices/native/PhaseAlign.h"
#include "devices/native/Sweep.h"
#include "devices/native/MidiVelocity.h"
#include "devices/native/MidiNoteEffects.h"
#include "devices/native/Arpeggiator.h"
#include "devices/native/NoteLength.h"
#include "devices/native/Utility.h"
#include "engine/SampleCache.h"
#include "io/als/AlsReader.h"
#include "io/als/LiveCompat.h"
#include "plugins/PluginDevice.h"
#include "plugins/PluginHost.h"
#include "plugins/PrismPreset.h"
#include "plugins/SonoraPluginState.h"

namespace sonora::devices
{

namespace
{
void note(BuildContext& ctx, std::string text)
{
    if (ctx.log != nullptr)
        ctx.log->push_back(std::move(text));
}

// PLG-14g: the built-in Hush keeps its session (preset, A/B slot, view) as .als content; the bytes are the VST3's state.
std::shared_ptr<const model::NativeContent> hushContent(const HushSession& session)
{
    auto content = std::make_shared<model::NativeContent>();
    content->format = HushDevice::kContentFormat;
    content->data = session.encodeState();
    return content;
}

bool isInstrumentDevice(const model::Device& d)
{
    if (d.kind == model::DeviceKind::Rack)
        return d.type == "InstrumentGroupDevice" || d.type == "DrumGroupDevice";
    if (d.plugin)
        return d.plugin->isInstrument;
    if (const auto* e = model::findCatalogEntry(d.type))
        return e->role == model::DeviceRole::Instrument;
    return false;
}

std::unique_ptr<DeviceProcessor> placeholderIfInstrument(const model::Device& d, BuildContext& ctx)
{
    if (ctx.placeholderForMissingInstruments && isInstrumentDevice(d))
        return std::make_unique<PlaceholderSynth>(d.displayName);
    return nullptr;
}

std::unique_ptr<DeviceProcessor> createRack(const model::Device& d, BuildContext& ctx)
{
    if (d.type == "MidiEffectGroupDevice")
    {
        note(ctx, "MIDI Effect Rack skipped (not implemented)");
        return nullptr;
    }

    std::vector<RackProcessor::Chain> chains;
    std::vector<RackProcessor::MacroLink> links;
    for (const auto& mc : d.chains)
    {
        RackProcessor::Chain c;
        c.volume = (float) mc.volume;
        c.pan = (float) mc.pan;
        c.active = mc.speakerOn;
        c.solo = mc.solo;
        c.receivingNote = mc.receivingNote;
        c.sendingNote = mc.sendingNote;
        c.chokeGroup = mc.chokeGroup;
        c.keyMin = mc.keyMin;
        c.keyMax = mc.keyMax;
        c.velMin = mc.velMin;
        c.velMax = mc.velMax;
        c.selMin = mc.selMin;
        c.selMax = mc.selMax;
        for (const auto& child : mc.devices)
        {
            auto p = createDevice(child, ctx);
            c.processorForModelDevice.push_back(p.get());
            if (p)
            {
                // Parameters of this device that follow one of the rack's macros.
                for (const auto& param : child.parameters)
                {
                    if (param.macroIndex < 0 || param.macroIndex >= RackProcessor::kNumMacros)
                        continue;
                    const auto& infos = p->parameters();
                    const auto it = std::find_if(infos.begin(), infos.end(), [&](const ParameterInfo& pi) { return pi.id == param.path; });
                    if (it != infos.end())
                        links.push_back({param.macroIndex, p.get(), (int) (it - infos.begin()), (float) param.rangeMin,
                                         (float) param.rangeMax});
                }
                c.devices.push_back(std::move(p));
            }
        }
        chains.push_back(std::move(c));
    }
    auto rack = std::make_unique<RackProcessor>(d.displayName, isInstrumentDevice(d), d.type == "DrumGroupDevice",
                                                std::move(chains));
    std::array<float, RackProcessor::kNumMacros> macroValues{};
    for (int i = 0; i < RackProcessor::kNumMacros; ++i)
        macroValues[(size_t) i] = (float) d.param("MacroControls." + std::to_string(i), 0.0);
    rack->setMacros(d.macroNames, macroValues, std::move(links), d.numVisibleMacros);
    rack->setParameter(RackProcessor::kChainSelector, (float) d.param("ChainSelector", 0.0));
    rack->setBypassed(! d.enabled);
    return rack;
}

std::unique_ptr<DeviceProcessor> createSimpler(const model::Device& d, BuildContext& ctx)
{
    std::vector<Simpler::Zone> zones;
    for (const auto& part : d.sampleParts)
    {
        Simpler::Zone z;
        z.part = part;
        if (ctx.samples != nullptr && ctx.song != nullptr)
        {
            const auto path = io::resolveSamplePath(*ctx.song, part.sample);
            if (path.empty())
                note(ctx, "Simpler sample not found: " + (part.sample.relativePath.empty() ? part.sample.absolutePath
                                                                                          : part.sample.relativePath));
            else
                z.slot = ctx.samples->request(path);
        }
        zones.push_back(std::move(z));
    }
    const auto plain = [&](const char* key, double fallback) {
        const auto it = d.plain.find(key);
        return it != d.plain.end() ? it->second : fallback;
    };
    if (d.type == "SonoraImpulse")
    {
        auto dev = std::make_unique<Impulse>(std::move(zones));
        dev->loadFromModel(d);
        return dev;
    }
    if (d.type == "MultiSampler") // DEV-14b: the Sampler's own device (same engine laws, every Sampler control)
    {
        auto sampler = std::make_unique<Sampler>(std::move(zones), (int) plain("Player/MultiSampleMap/RoundRobinRandomSeed", 0));
        sampler->loadFromModel(d);
        return sampler;
    }
    auto dev = std::make_unique<Simpler>(std::move(zones), static_cast<Simpler::Mode>((int) plain("Globals/PlaybackMode", 0)),
                                         (int) plain("Globals/PitchBendRange", 5));
    dev->loadFromModel(d);
    return dev;
}

// COL-4c: one of Sonora's own VST3s that is not installed here (a collab guest, a set made on another computer). The
// built-in device plays its saved state; the model keeps the plug-in reference and state as they were (see
// captureDeviceState), so saving or syncing the set changes nothing for computers that have the VST3.
std::unique_ptr<DeviceProcessor> createSonoraStandIn(const model::Device& d, BuildContext& ctx)
{
    using plugins::SonoraPlugin;
    const auto which = plugins::sonoraPluginFor(*d.plugin, d.kind);
    if (which == SonoraPlugin::None) return nullptr;
    const auto body = plugins::juceVst3StateBody(d.plugin->state);
    std::string error;
    std::unique_ptr<DeviceProcessor> dev;
    if (which == SonoraPlugin::Prism)
    {
        model::Device native = nativeDeviceModel("SonoraWavetable");
        native.id = d.id;
        if (! body.empty() && ! plugins::decodePrismPreset(body, native, error))
            note(ctx, d.plugin->name + ": the saved sound could not be read (" + error + "), playing the default");
        std::vector<std::string> warnings;
        dev = createNative(native, &warnings);
        for (auto& warning : warnings) note(ctx, d.plugin->name + ": " + warning);
    }
    else if (which == SonoraPlugin::CurveLfo)
    {
        auto lfo = std::make_unique<CurveLfo>();
        if (! body.empty() && ! plugins::decodeCurveLfoState(body.data(), body.size(), *lfo, error))
            note(ctx, d.plugin->name + ": the saved curves could not be read (" + error + "), using the defaults");
        dev = std::move(lfo);
    }
    else if (which == SonoraPlugin::Kickstract) // PLG-17
    {
        auto kick = std::make_unique<Kickstract>();
        if (! body.empty() && ! plugins::decodeKickstractState(body.data(), body.size(), *kick, error))
            note(ctx, d.plugin->name + ": the saved settings could not be read (" + error + "), using the defaults");
        dev = std::move(kick);
    }
    else if (which == SonoraPlugin::Stutter) // PLG-18
    {
        auto stutter = std::make_unique<Stutter>();
        if (! body.empty() && ! plugins::decodeStutterState(body.data(), body.size(), *stutter, error))
            note(ctx, d.plugin->name + ": the saved settings could not be read (" + error + "), using the defaults");
        dev = std::move(stutter);
    }
    else if (which == SonoraPlugin::Sweep) // PLG-19: parameters + the sample's path
    {
        auto sweep = std::make_unique<Sweep>();
        std::map<std::string, std::string> attributes;
        if (! body.empty() && ! plugins::decodeParameterState(body.data(), body.size(), *sweep, "SonoraSweep", "Sonora Sweep", error, &attributes))
            note(ctx, d.plugin->name + ": the saved settings could not be read (" + error + "), using the defaults");
        if (const auto path = attributes.find("samplePath"); path != attributes.end() && ! path->second.empty())
            if (std::string loadError; ! sweep->loadSample(path->second, loadError)) note(ctx, d.plugin->name + ": " + loadError);
        dev = std::move(sweep);
    }
    else if (which == SonoraPlugin::PhaseAlign) // PLG-21
    {
        auto phase = std::make_unique<PhaseAlign>();
        if (! body.empty() && ! plugins::decodeParameterState(body.data(), body.size(), *phase, "SonoraPhaseAlign", "Sonora Phase Align", error))
            note(ctx, d.plugin->name + ": the saved settings could not be read (" + error + "), using the defaults");
        dev = std::move(phase);
    }
    else // COL-4e: the VST3's state body is the built-in device's session state
    {
        auto hush = std::make_unique<HushDevice>();
        if (! body.empty() && ! hush->session().decodeState(body.data(), body.size(), error))
            note(ctx, d.plugin->name + ": the saved settings could not be read (" + error + "), using the defaults");
        dev = std::move(hush);
    }
    if (! dev) return nullptr;
    dev->setBypassed(! d.enabled);
    note(ctx, d.plugin->name + ": VST3 not installed here, played by Sonora's built-in device");
    return dev;
}

std::unique_ptr<DeviceProcessor> createPlugin(const model::Device& d, BuildContext& ctx)
{
    if (! d.plugin)
        return placeholderIfInstrument(d, ctx);

    const auto match = ctx.pluginHost ? ctx.pluginHost->findMatch(*d.plugin, d.kind) : std::nullopt;
    if (! match)
    {
        if (auto standIn = createSonoraStandIn(d, ctx)) return standIn;
        if (ctx.pluginHost != nullptr) note(ctx, "Plugin not installed/scanned: " + d.plugin->name);
        return placeholderIfInstrument(d, ctx);
    }

    juce::String error;
    std::unique_ptr<juce::AudioPluginInstance> instance = ctx.takeRunningPlugin ? ctx.takeRunningPlugin(ctx.trackId, d.id) : nullptr;
    if (instance == nullptr) instance = ctx.pluginHost->instantiate(*match, *d.plugin, d.kind, ctx.sampleRate, ctx.blockSize, error);
    if (error.isNotEmpty())
        note(ctx, error.toStdString());
    if (instance == nullptr)
        return placeholderIfInstrument(d, ctx);

    const auto createMs = ctx.pluginHost->lastCreateMs(), stateMs = ctx.pluginHost->lastStateMs();
    const auto t0 = juce::Time::getMillisecondCounterHiRes();
    auto dev = std::make_unique<plugins::PluginDevice>(std::move(instance));
    dev->setBypassed(! d.enabled);
    dev->setSidechainRouted(d.sidechain.routed() && dev->takesSidechain()); // PLG-22: before the graph prepares it
    const auto prepareMs = juce::Time::getMillisecondCounterHiRes() - t0;
    static const double thresholdMs = std::getenv("SONORA_LOG_LOAD") != nullptr ? 0.0 : 150.0; // SONORA_LOG_LOAD=1 logs every plugin
    if (createMs + stateMs + prepareMs >= thresholdMs)
        note(ctx, "Plugin load time: " + d.plugin->name + " " + std::to_string((int) (createMs + stateMs + prepareMs)) + " ms (create "
                      + std::to_string((int) createMs) + ", state " + std::to_string((int) stateMs) + ", prepare "
                      + std::to_string((int) prepareMs) + ")");

    if (d.kind == model::DeviceKind::PluginVst2 && match->pluginFormatName != "VST" && ! d.pluginParams.empty())
    {
        // A VST2 reference loaded into the VST3 build of the same plugin: the saved chunk is not
        // portable, so restore what Live stored as individual parameter values, matched by name.
        int restored = 0;
        for (const auto& p : d.pluginParams)
        {
            const int idx = dev->findParameterByHostId(-1, p.name);
            if (idx >= 0)
            {
                dev->setParameter(idx, (float) p.value);
                ++restored;
            }
        }
        note(ctx, "VST2 -> VST3 (" + d.plugin->name + "): restored " + std::to_string(restored) + " of "
                      + std::to_string(d.pluginParams.size()) + " parameters by name; the rest of its state is not portable");
    }
    return dev;
}
} // namespace

std::unique_ptr<DeviceProcessor> createNative(const model::Device& d, std::vector<std::string>* warnings)
{
    std::unique_ptr<NativeDevice> dev;
    if (d.type == "Gate") dev = std::make_unique<Gate>();
    else if (d.type == "AutoPan") dev = std::make_unique<AutoPan>();
    else if (d.type == "MidiPitcher") dev = std::make_unique<MidiNoteEffects>(MidiNoteEffects::Kind::Pitch);
    else if (d.type == "MidiChord") dev = std::make_unique<MidiNoteEffects>(MidiNoteEffects::Kind::Chord);
    else if (d.type == "MidiScale") dev = std::make_unique<MidiNoteEffects>(MidiNoteEffects::Kind::Scale);
    else if (d.type == "MidiRandom") dev = std::make_unique<MidiNoteEffects>(MidiNoteEffects::Kind::Random);
    else if (d.type == "MidiVelocity") dev = std::make_unique<MidiNoteEffects>(MidiNoteEffects::Kind::Velocity);
    else if (d.type == "MidiArpeggiator") dev = std::make_unique<Arpeggiator>();
    else if (d.type == "MidiNoteLength") dev = std::make_unique<NoteLength>();
    else if (d.type == "Redux2")
        dev = std::make_unique<Redux>();
    else if (d.type == "Tuner")
        dev = std::make_unique<Tuner>();
    else if (d.type == "StereoGain")
        dev = std::make_unique<Utility>();
    else if (d.type == "Eq8")
        dev = std::make_unique<EqEight>();
    else if (d.type == "Compressor2")
        dev = std::make_unique<Compressor>();
    else if (d.type == "GlueCompressor")
        dev = std::make_unique<GlueCompressor>();
    else if (d.type == "Reverb")
        dev = std::make_unique<Reverb>();
    else if (d.type == "Delay")
        dev = std::make_unique<Delay>();
    else if (d.type == "AutoFilter")
        dev = std::make_unique<AutoFilter>();
    else if (d.type == "AutoFilter2")
        dev = std::make_unique<AutoFilter2>();
    else if (d.type == "FilterEQ3")
        dev = std::make_unique<EqThree>();
    else if (d.type == "ChannelEq") // DEV-30
        dev = std::make_unique<ChannelEq>();
    else if (d.type == "MultibandDynamics")
        dev = std::make_unique<MultibandDynamics>();
    else if (d.type == "Limiter")
        dev = std::make_unique<Limiter>();
    else if (d.type == "SonoraFm")
        dev = std::make_unique<FmSynth>();
    else if (d.type == "SonoraVector")
        dev = std::make_unique<VectorSynth>();
    else if (d.type == "StringStudio") // DEV-28j: the reference's Tension
        dev = std::make_unique<Tension>();
    else if (d.type == "InstrumentVector") // DEV-28b: the reference's Wavetable
    {
        auto synth = std::make_unique<WavetableInstrument>();
        std::vector<std::string> localWarnings;
        prepareWavetableInstrument(*synth, d, warnings != nullptr ? *warnings : localWarnings);
        dev = std::move(synth);
    }
    else if (d.type == "SonoraResynth")
    {
        std::string error;
        auto model = d.nativeContent ? model::decodeResynth(*d.nativeContent, error) : nullptr;
        if (d.nativeContent && ! model && warnings) warnings->push_back("Sonora Resynth: " + error);
        auto synth = std::make_unique<Resynth>(std::move(model));
        if (d.nativeContent && error.empty() && ! synth->ready() && warnings)
            warnings->push_back("Sonora Resynth: model could not be prepared within its memory limit");
        dev = std::move(synth);
    }
    else if (d.type == "SonoraEcho")
        dev = std::make_unique<Echo>();
    else if (d.type == "SonoraWavetable")
    {
        auto synth = std::make_unique<WavetableSynth>();
        std::vector<std::string> localWarnings;
        prepareWavetableContent(*synth, d, warnings != nullptr ? *warnings : localWarnings);
        dev = std::move(synth);
    }
    else if (d.type == "SonoraLfo")
        dev = std::make_unique<LfoShaper>();
    else if (d.type == "SonoraCurveLfo")
        dev = std::make_unique<CurveLfo>();
    else if (d.type == "SonoraHush")
    {
        // Session first (preset, A/B, view); the parameters below then take the model's values, which automation and
        // the engine sync keep current.
        auto hush = std::make_unique<HushDevice>();
        if (d.nativeContent && d.nativeContent->format == HushDevice::kContentFormat)
        {
            std::string error;
            if (! hush->session().decodeState(d.nativeContent->data.data(), d.nativeContent->data.size(), error) && warnings)
                warnings->push_back("Sonora Hush: its saved settings could not be read (" + error + "); parameters kept");
        }
        dev = std::move(hush);
    }
    else if (d.type == "SonoraKickstract") // PLG-17
        dev = std::make_unique<Kickstract>();
    else if (d.type == "SonoraStutter") // PLG-18
        dev = std::make_unique<Stutter>();
    else if (d.type == "BeatRepeat") // PLG-18b
        dev = std::make_unique<BeatRepeat>();
    else if (d.type == "SonoraPhaseAlign") // PLG-21
        dev = std::make_unique<PhaseAlign>();
    else if (d.type == "SonoraSweep") // PLG-19: the Sample source's file is native content "SonoraSweep/Sample" (a path)
    {
        auto sweep = std::make_unique<Sweep>();
        if (d.nativeContent && d.nativeContent->format == "SonoraSweep/Sample" && ! d.nativeContent->data.empty())
        {
            const std::string path(d.nativeContent->data.begin(), d.nativeContent->data.end());
            std::string error;
            if (! sweep->loadSample(path, error) && warnings) warnings->push_back("Sonora Sweep: " + error);
        }
        dev = std::move(sweep);
    }
    else if (d.type == "SonoraVelocity")
        dev = std::make_unique<MidiVelocity>();

    if (dev)
    {
        dev->loadFromModel(d);
        if (warnings)
        {
            if (dynamic_cast<MidiNoteEffects*>(dev.get()) && (d.param("UseCurrentScale", 0) > 0.5 || std::abs(d.param("Strum", 0)) > 1e-9))
                warnings->push_back(d.displayName + ": current-scale/strum processing is not implemented; settings retained");
            if (d.type == "MidiArpeggiator" && d.param("UseCurrentScale", 0) > 0.5)
                warnings->push_back(d.displayName + ": Use Current Scale is not implemented; transposes in its own Root/Scale");
            if (d.type == "Gate" && d.param("SideChainEq/On", 0) > 0.5)
                warnings->push_back("Gate: sidechain EQ is not implemented; settings retained");
            if (d.type == "AutoPan" && (d.param("Lfo/StereoMode", 0) > 0.5 || std::abs(d.param("Lfo/LfoShape", 0)) > 1e-9))
                warnings->push_back("Auto Pan: spin/shape processing is not implemented; settings retained");
        }
    }
    return dev;
}

// ALS-15: the state Sonora's VST3 of a native device would save, so a set opened in the reference app plays Prism,
// Curve LFO and Hush with the same settings there (io/als/LiveCompat writes it into the device's PluginDevice wrapper).
namespace
{
std::vector<uint8_t> sonoraVst3State(const model::Device& d)
{
    if (d.type == "SonoraWavetable") return plugins::encodePrismPreset(d);
    if (d.type != "SonoraCurveLfo" && d.type != "SonoraHush" && d.type != "SonoraKickstract" && d.type != "SonoraStutter" && d.type != "SonoraPhaseAlign" && d.type != "SonoraSweep") return {};
    const auto dev = createNative(d);
    if (const auto* lfo = dynamic_cast<const CurveLfo*>(dev.get())) return plugins::encodeCurveLfoState(*lfo);
    if (auto* hush = dynamic_cast<HushDevice*>(dev.get())) return hush->session().encodeState();
    if (const auto* kick = dynamic_cast<const Kickstract*>(dev.get())) return plugins::encodeKickstractState(*kick);
    if (const auto* stutter = dynamic_cast<const Stutter*>(dev.get())) return plugins::encodeStutterState(*stutter);
    if (const auto* phase = dynamic_cast<const PhaseAlign*>(dev.get())) return plugins::encodeParameterState(*phase, "SonoraPhaseAlign");
    if (const auto* sweep = dynamic_cast<const Sweep*>(dev.get()))
        return plugins::encodeParameterState(*sweep, "SonoraSweep", {{"samplePath", sweep->samplePath()}});
    return {};
}
} // namespace

void installSonoraVst3StateEncoder()
{
    io::setSonoraVst3StateEncoder(sonoraVst3State);
}

namespace
{
[[maybe_unused]] const bool sonoraVst3StateInstalled = [] {
    installSonoraVst3StateEncoder();
    return true;
}();
} // namespace

std::unique_ptr<DeviceProcessor> createDevice(const model::Device& d, BuildContext& ctx)
{
    switch (d.kind)
    {
        case model::DeviceKind::Native:
            if (d.type == "OriginalSimpler" || d.type == "MultiSampler" || d.type == "SonoraImpulse")
                return createSimpler(d, ctx);
            if (auto dev = createNative(d, ctx.log))
                return dev;
            return placeholderIfInstrument(d, ctx);
        case model::DeviceKind::Rack:
            return createRack(d, ctx);
        case model::DeviceKind::PluginVst2:
        case model::DeviceKind::PluginVst3:
        case model::DeviceKind::PluginAu:
            return createPlugin(d, ctx);
        case model::DeviceKind::MaxForLive:
            if (d.maxPatch == "Swiss Army Meter") // DEV-23; the retained MxDevice XML is saved back unchanged
            {
                auto meter = std::make_unique<SwissArmyMeter>();
                meter->loadFromModel(d);
                return meter;
            }
            return nullptr;
    }
    return nullptr;
}

model::Device nativeDeviceModel(const std::string& type)
{
    model::Device d;
    if (type.starts_with("max:")) // DEV-23: a Max for Live device with a native replacement
    {
        const auto* entry = model::findCatalogEntry(type);
        const auto role = entry ? entry->role : model::DeviceRole::AudioEffect;
        d.kind = model::DeviceKind::MaxForLive;
        d.type = role == model::DeviceRole::Instrument ? "MxDeviceInstrument"
                 : role == model::DeviceRole::MidiEffect ? "MxDeviceMidiEffect" : "MxDeviceAudioEffect";
        d.maxPatch = type.substr(4);
        d.displayName = std::string(model::maxForLiveNativeName(d.maxPatch));
        return d;
    }
    d.type = type;
    const auto* entry = model::findCatalogEntry(type);
    d.displayName = entry ? std::string(entry->displayName) : type;
    d.kind = model::isRackTag(type) ? model::DeviceKind::Rack : model::DeviceKind::Native;
    if (d.kind == model::DeviceKind::Rack) d.numVisibleMacros = 16;
    BuildContext context;
    if (auto p = createDevice(d, context))
    {
        // A new Hush starts from the preset chosen as its default (shared Hush settings), like the VST3.
        std::unique_ptr<HushDevice> fresh;
        if (type == "SonoraHush") fresh = std::make_unique<HushDevice>();
        const auto& infos = p->parameters();
        for (size_t i = 0; i < infos.size(); ++i)
        {
            const auto& info = infos[i];
            const float value = fresh ? fresh->getParameter((int) i) : info.defaultValue;
            if (info.plainValue) { d.plain[info.id] = value; continue; }
            model::Parameter parameter;
            parameter.path = info.id; parameter.value = value; parameter.booleanValue = info.isToggle && ! info.intNode;
            d.parameters.push_back(std::move(parameter));
        }
        if (fresh) d.nativeContent = hushContent(fresh->session());
    }
    return d;
}

void captureDeviceState(model::Device& d, DeviceProcessor* processor, const PluginCaptureFilter* capturePlugin)
{
    if (! processor) return;
    d.enabled = ! processor->isBypassed();
    if (auto* plugin = dynamic_cast<plugins::PluginDevice*>(processor))
    {
        if (capturePlugin == nullptr || (*capturePlugin)(*plugin)) plugin->captureState(d);
    }
    else if (d.plugin) return; // a placeholder or built-in stand-in (COL-4c): the plug-in's saved state stays as it was
    if (const auto* synth = dynamic_cast<WavetableSynth*>(processor); synth && synth->hasEffectsConfiguration())
    {
        std::string error;
        // Save runs with processing suspended. Retain graph structure, IR ownership and current controls.
        storeWavetableEffects(d, synth->effectsConfiguration(), error);
    }
    if (const auto* synth = dynamic_cast<WavetableSynth*>(processor); synth && (synth->requestedVoiceRateFactor() != 1
        || (d.nativeContent && d.nativeContent->format == "sonora-wavetable-v1")))
    {
        std::string error;
        storeWavetableVoiceRate(d, synth->requestedVoiceRateFactor(), error);
    }
    const auto& infos = processor->parameters();
    for (size_t i = 0; i < infos.size(); ++i)
    {
        const float value = processor->getParameter((int) i);
        const auto found = std::find_if(d.parameters.begin(), d.parameters.end(), [&](const auto& p) { return p.path == infos[i].id; });
        // The processor holds floats: a value that only differs by float rounding keeps the file's double, so saving an
        // untouched device rewrites nothing (DEV-14b round trip).
        const auto changed = [&](double stored) { return std::abs(stored - (double) value) > 1e-6 * std::max(1.0, std::abs(stored)); };
        if (infos[i].plainValue)
        {
            if (const auto it = d.plain.find(infos[i].id); it == d.plain.end() || changed(it->second)) d.plain[infos[i].id] = value;
            continue;
        }
        if (found != d.parameters.end()) { if (changed(found->value)) found->value = value; }
        else if (! d.plugin && std::abs(value - infos[i].defaultValue) > 1e-6f)
        {
            model::Parameter p; p.path = infos[i].id; p.value = value; p.booleanValue = infos[i].isToggle && ! infos[i].intNode;
            d.parameters.push_back(std::move(p));
        }
    }
    if (const auto* hush = dynamic_cast<HushDevice*>(processor))
    {
        // Preset name, A/B slot and view as content; replaced only when they changed, so saving twice writes the same.
        auto content = hushContent(hush->session());
        if (! d.nativeContent || d.nativeContent->format != content->format || d.nativeContent->data != content->data)
            d.nativeContent = std::move(content);
    }
    if (const auto* rack = dynamic_cast<RackProcessor*>(processor))
        for (size_t ci = 0; ci < d.chains.size() && ci < rack->chains().size(); ++ci)
        {
            if (const auto* mix = rack->chainMix((int) ci)) // UI-66: the chain list edits the mixer live
            {
                // Untouched values keep their double precision from the file (the processor holds floats).
                auto keep = [](double& stored, float live) { if (std::abs(stored - (double) live) > 1e-6) stored = live; };
                keep(d.chains[ci].volume, mix->volume.load());
                keep(d.chains[ci].pan, mix->pan.load());
                d.chains[ci].speakerOn = mix->active.load();
                d.chains[ci].solo = mix->solo.load();
            }
            auto& children = d.chains[ci].devices;
            const auto& processors = rack->chains()[ci].processorForModelDevice;
            for (size_t pi = 0; pi < children.size() && pi < processors.size(); ++pi)
                captureDeviceState(children[pi], processors[pi], capturePlugin);
        }
}

} // namespace sonora::devices

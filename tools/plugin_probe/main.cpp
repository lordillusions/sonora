// plugin_probe — headless check of third-party plugin hosting.
//
//   plugin_probe --scan-file "/Library/Audio/Plug-Ins/VST3/Foo.vst3"   add one plugin to the cache (out of process, 60 s timeout)
//   plugin_probe --scan                                                scan all default VST3 folders
//   plugin_probe --list                                                print the cache
//   plugin_probe --decode-serum-state <state.bin>                       native state inspection without loading the plugin
//   plugin_probe --inspect-file <plugin> [--output <report.json>] [--state-dir <directory>]
//        [--set <hostId>=<normalised>] ... [--render-out <directory>] [--class-id <CID>]
//        [--component-state <state.bin>] [--controller-state <state.bin>] [--editor]
//        [--note <0..127>] [--velocity <1..127>]
//        [--stimulus sine|impulse|step|noise|gated|gatednoise] [--gate <highSamples>:<lowSamples>:<lowScale>] [--amplitude <0..1>] [--frequency <Hz>]
//        [--right-scale <-1..1>] (0 isolates the left input)
//        [--midi-event <sample>:<channel-message-hex>] ... (replaces the default MIDI pair)
//        [--render-block-size <128|256|512|1024|2048>] [--preroll-samples <0..65536>]
//        [--render-samples <48000..480000>] (default96000; offline capture only)
//        [--playhead-jump <sample>:<delta-samples>] ... (preroll and jumps align to the render block)
//        [--text-sweep <hostId>=<2..4097 points>] ... (active hosted controller text, no writes)
//        [--native-interfaces] (opt-in public VST3 method module-relative offsets on macOS)
//        inspect/restore isolated instances and export state/audio (60 s watchdog; editor 180 s)
//   plugin_probe <set.als> [--render]                                  for every plugin in the set:
//        match against the cache, instantiate, restore Live's saved state, report the
//        result, and (with --render) process one second of audio to check it runs.
//
// Scan/list/set modes share the stable app's cache (~/Library/Application Support/Sonora/PluginCache.xml).
// --data-folder "Sonora Nightly" (any position) uses another app's data folder instead (COM-4c, core/AppDataFolder.h).
// --inspect-file leaves that cache untouched.

#include "core/AppDataFolder.h"
#include "io/als/AlsReader.h"
#include "plugins/PluginHost.h"
#include "plugins/Vst3PresetBuilder.h"
#include "plugins/SerumState.h"

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_events/juce_events.h>
JUCE_BEGIN_IGNORE_WARNINGS_GCC_LIKE("-Wshadow-field-in-constructor")
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>
#include <pluginterfaces/vst/ivsthostapplication.h>
#include <pluginterfaces/base/ibstream.h>
JUCE_END_IGNORE_WARNINGS_GCC_LIKE

#include <cstdio>
#include <cmath>
#include <initializer_list>
#include <limits>
#include <utility>
#include <cstdlib>
#include <charconv>
#include <map>
#include <atomic>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <type_traits>

#if JUCE_MAC
#include <dlfcn.h>
#endif

using namespace sonora;

static int runProbeCommand(int argc, char** argv);

namespace
{
int editorArgc = 0;
char** editorArgv = nullptr;

// The command-line worker must be an actual standalone JUCE application while hosting a native
// editor. ScopedJuceInitialiser_GUI alone selects JUCE's plugin-context focus/mouse event path.
class ReferenceProbeApplication final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "SerumReferenceProbe"; }
    const juce::String getApplicationVersion() override { return "1"; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void initialise(const juce::String&) override
    {
        juce::MessageManager::callAsync([this]
        {
            setApplicationReturnValue(runProbeCommand(editorArgc, editorArgv));
            quit();
        });
    }
    void shutdown() override {}
    void systemRequestedQuit() override { quit(); }
};

constexpr double inspectSampleRate = 48000.0;
constexpr int inspectBlockSize = 512;

struct ParameterOverride
{
    juce::String hostId;
    float normalised = 0.0f;
};

struct ProbeMidiEvent
{
    int sample = 0, size = 0;
    std::array<juce::uint8, 3> data{};
    juce::String hex;
};

struct ProbePlayHeadJump
{
    int sample = 0, delta = 0;
};

struct ParameterTextSweep
{
    juce::String hostId;
    int points = 0;
};

struct InspectionOptions
{
    juce::String outputPath, stateDirectory, renderDirectory, classId, componentState, controllerState;
    std::vector<ParameterOverride> overrides;
    std::vector<ProbeMidiEvent> midiEvents;
    std::vector<ProbePlayHeadJump> playHeadJumps;
    std::vector<ParameterTextSweep> textSweeps;
    int note = 60, velocity = 100;
    int prerollSamples = 4096;
    int settleMilliseconds = 0; // --settle-ms: wait after restoring state (plugins that load samples on a background thread)
    int renderBlockSize = inspectBlockSize;
    int renderSamples = 96000;
    juce::String stimulus = "sine";
    double amplitude = 0.1, frequency = 220.0, rightScale = 1.0;
    int gateHigh = 24000, gateLow = 24000; // "gated" stimulus: sine alternating between amplitude and amplitude * gateScale
    double gateScale = 0.1;
    bool editor = false;
    bool nativeInterfaces = false;
    bool rawState = false; // --raw-state: hand Serum states to the plugin without the identity/version check (preset migration studies)
};

bool readClassState(juce::AudioPluginInstance& instance, model::PluginInfo& state)
{
    if (const auto* client = instance.getVST3Client())
    {
        const auto preset = client->getPreset();
        const auto* bytes = static_cast<const uint8_t*>(preset.getData());
        return ! preset.isEmpty() && plugins::readVstPreset({bytes, bytes + preset.getSize()}, state);
    }
    return false;
}

bool readStateBytes(const juce::String& path, std::vector<uint8_t>& bytes, juce::String& error)
{
    if (path.isEmpty()) return true;
    const juce::File file(path);
    if (! file.existsAsFile() || file.getSize() < 1 || file.getSize() > 65 * 1024 * 1024)
    {
        error = "State file missing, empty, or exceeds 65 MiB";
        return false;
    }
    juce::MemoryBlock data;
    if (! file.loadFileAsData(data))
    {
        error = "Could not read state file";
        return false;
    }
    const auto* first = static_cast<const uint8_t*>(data.getData());
    bytes.assign(first, first + data.getSize());
    return true;
}

// These two minimal host objects implement only documented VST3 interfaces. They are used by a
// separate factory-created controller; no JUCE private host pointers or plugin internals are read.
class ProbeHostApplication final : public Steinberg::Vst::IHostApplication
{
public:
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** result) override
    {
        if (result == nullptr) return Steinberg::kInvalidArgument;
        *result = nullptr;
        if (! Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid.toTUID())
            && ! Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::Vst::IHostApplication::iid.toTUID()))
            return Steinberg::kNoInterface;
        *result = static_cast<Steinberg::Vst::IHostApplication*>(this);
        addRef();
        return Steinberg::kResultOk;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return ++references_; }
    Steinberg::uint32 PLUGIN_API release() override
    {
        const auto remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }
    Steinberg::tresult PLUGIN_API getName(Steinberg::Vst::String128 name) override
    {
        constexpr char text[] = "Sonora public controller probe";
        std::fill(name, name + 128, Steinberg::char16{});
        for (size_t index = 0; index + 1 < sizeof(text); ++index)
            name[index] = static_cast<Steinberg::char16>(text[index]);
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API createInstance(Steinberg::TUID, Steinberg::TUID, void** result) override
    {
        if (result == nullptr) return Steinberg::kInvalidArgument;
        *result = nullptr;
        return Steinberg::kNoInterface;
    }
private:
    std::atomic<Steinberg::uint32> references_{1};
};

class ProbeStateStream final : public Steinberg::IBStream
{
public:
    explicit ProbeStateStream(const std::vector<uint8_t>& bytes) : bytes_(bytes) {}
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** result) override
    {
        if (result == nullptr) return Steinberg::kInvalidArgument;
        *result = nullptr;
        if (! Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::FUnknown::iid.toTUID())
            && ! Steinberg::FUnknownPrivate::iidEqual(iid, Steinberg::IBStream::iid.toTUID()))
            return Steinberg::kNoInterface;
        *result = static_cast<Steinberg::IBStream*>(this);
        addRef();
        return Steinberg::kResultOk;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return ++references_; }
    Steinberg::uint32 PLUGIN_API release() override
    {
        const auto remaining = --references_;
        if (remaining == 0) delete this;
        return remaining;
    }
    Steinberg::tresult PLUGIN_API read(void* destination, Steinberg::int32 requested, Steinberg::int32* actual) override
    {
        if (actual != nullptr) *actual = 0;
        if (requested < 0 || (requested > 0 && destination == nullptr)) return Steinberg::kInvalidArgument;
        const auto count = std::min(static_cast<size_t>(requested), bytes_.size() - position_);
        if (count > 0) std::memcpy(destination, bytes_.data() + position_, count);
        position_ += count;
        if (actual != nullptr) *actual = static_cast<Steinberg::int32>(count);
        return count == static_cast<size_t>(requested) ? Steinberg::kResultOk : Steinberg::kResultFalse;
    }
    Steinberg::tresult PLUGIN_API write(void*, Steinberg::int32, Steinberg::int32* actual) override
    {
        if (actual != nullptr) *actual = 0;
        return Steinberg::kResultFalse;
    }
    Steinberg::tresult PLUGIN_API seek(Steinberg::int64 offset, Steinberg::int32 mode, Steinberg::int64* result) override
    {
        const auto size = static_cast<Steinberg::int64>(bytes_.size());
        const auto base = mode == kIBSeekSet ? 0 : mode == kIBSeekCur ? static_cast<Steinberg::int64>(position_) : size;
        if (mode < kIBSeekSet || mode > kIBSeekEnd || offset < -base || offset > size - base)
            return Steinberg::kInvalidArgument;
        position_ = static_cast<size_t>(base + offset);
        if (result != nullptr) *result = static_cast<Steinberg::int64>(position_);
        return Steinberg::kResultOk;
    }
    Steinberg::tresult PLUGIN_API tell(Steinberg::int64* result) override
    {
        if (result == nullptr) return Steinberg::kInvalidArgument;
        *result = static_cast<Steinberg::int64>(position_);
        return Steinberg::kResultOk;
    }
private:
    std::atomic<Steinberg::uint32> references_{1};
    std::vector<uint8_t> bytes_;
    size_t position_ = 0;
};

juce::File bundleExecutable(const juce::File& plugin)
{
    if (plugin.existsAsFile()) return plugin;
    const auto plist = juce::XmlDocument::parse(plugin.getChildFile("Contents/Info.plist"));
    if (plist != nullptr)
        if (const auto* dictionary = plist->getChildByName("dict"))
            for (const auto* entry : dictionary->getChildIterator())
                if (entry->hasTagName("key") && entry->getAllSubText() == "CFBundleExecutable")
                    if (const auto* value = entry->getNextElement(); value != nullptr && value->hasTagName("string"))
                    {
                        const auto name = value->getAllSubText();
                        if (name.isNotEmpty() && ! name.containsAnyOf("/\\"))
                            return plugin.getChildFile("Contents/MacOS").getChildFile(name);
                    }
    return {};
}

juce::String vstString(const Steinberg::char16* text)
{
    return juce::String(juce::CharPointer_UTF16(reinterpret_cast<const juce::CharPointer_UTF16::CharType*>(text)));
}

// A component can expose its active controller directly. Otherwise create a distinct controller
// from the same already-loaded artifact, initialize it with a public host, and apply our captured
// initial fresh default state through public IBStream methods. A disconnected controller can lack
// context for modified routing state, so its conversion/label scope is explicitly kept separate.
class PlainController
{
public:
    PlainController(juce::AudioPluginInstance& instance, const juce::String& pluginPath,
                    const model::PluginInfo& initialState)
    {
        if (const auto* client = instance.getVST3Client())
            if (auto* component = client->getIComponentPtr())
            {
                component->queryInterface(Steinberg::Vst::IEditController::iid.toTUID(), reinterpret_cast<void**>(&controller_));
                if (controller_ != nullptr)
                    source_ = "active component IEditController";
                else
                    createSeparate(*component, juce::File(pluginPath), initialState);
            }
        if (controller_ != nullptr)
            for (Steinberg::int32 index = 0; index < controller_->getParameterCount(); ++index)
            {
                Steinberg::Vst::ParameterInfo info{};
                if (controller_->getParameterInfo(index, info) == Steinberg::kResultOk)
                    infos_.emplace(info.id, info);
            }
    }
    ~PlainController()
    {
        if (controller_ != nullptr)
        {
            if (initialised_) controller_->terminate();
            controller_->release();
        }
        if (factory_ != nullptr) factory_->release();
        if (host_ != nullptr) host_->release();
        // library_ closes last, after every object and interface obtained from it is released.
    }
    bool available() const { return controller_ != nullptr; }
    juce::var provenance() const
    {
        auto* result = new juce::DynamicObject();
        result->setProperty("source", source_);
        result->setProperty("error", error_);
        result->setProperty("separateController", initialised_);
        result->setProperty("parameterCount", static_cast<juce::int64>(infos_.size()));
        result->setProperty("controllerClassId", controllerClassId_);
        result->setProperty("moduleExecutable", executable_);
        result->setProperty("componentStateApplied", componentStateApplied_);
        result->setProperty("controllerStateApplied", controllerStateApplied_);
        result->setProperty("conversionStateScope", initialised_ ? "initial fresh default Comp/Cont before restore, editor changes or host overrides"
                                                                 : controller_ != nullptr ? "active component controller" : "unavailable");
        result->setProperty("sampleRateNote", "Separate controller is initialized without a processor connection; conversion may use constructor sample-rate defaults rather than the audio render rate");
        return juce::var(result);
    }
    juce::var values(const juce::String& hostId, double hostedCurrent,
                     const juce::String& zeroText, const juce::String& halfText, const juce::String& oneText) const
    {
        Steinberg::Vst::ParamID id = 0;
        const auto text = hostId.toStdString();
        const auto parsed = std::from_chars(text.data(), text.data() + text.size(), id);
        const auto found = infos_.find(id);
        if (controller_ == nullptr || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || found == infos_.end())
            return {};
        const auto& info = found->second;
        const auto number = [](double value) -> juce::var { return std::isfinite(value) ? juce::var(value) : juce::var(); };
        const auto current = controller_->getParamNormalized(id);
        auto* result = new juce::DynamicObject();
        result->setProperty("unitId", info.unitId);
        result->setProperty("flags", info.flags);
        result->setProperty("stepCount", info.stepCount);
        result->setProperty("title", vstString(info.title));
        result->setProperty("shortTitle", vstString(info.shortTitle));
        result->setProperty("units", vstString(info.units));
        result->setProperty("defaultNormalised", number(info.defaultNormalizedValue));
        result->setProperty("controllerCurrentNormalised", number(current));
        result->setProperty("hostedCurrentNormalised", number(hostedCurrent));
        result->setProperty("defaultPlain", number(controller_->normalizedParamToPlain(id, info.defaultNormalizedValue)));
        result->setProperty("hostedCurrentPlain", number(controller_->normalizedParamToPlain(id, hostedCurrent)));
        result->setProperty("plainAtZero", number(controller_->normalizedParamToPlain(id, 0)));
        result->setProperty("plainAtHalf", number(controller_->normalizedParamToPlain(id, 0.5)));
        result->setProperty("plainAtOne", number(controller_->normalizedParamToPlain(id, 1)));
        bool correlated = true;
        int index = 0;
        for (const auto& expected : {zeroText, halfText, oneText})
        {
            Steinberg::Vst::String128 display{};
            const auto success = controller_->getParamStringByValue(id, static_cast<double>(index) * 0.5, display) == Steinberg::kResultOk;
            const auto actual = success ? vstString(display) : juce::String();
            result->setProperty(index == 0 ? "textAtZero" : index == 1 ? "textAtHalf" : "textAtOne",
                                success ? juce::var(actual) : juce::var());
            correlated = correlated && success && actual.trim() == expected.trim();
            ++index;
        }
        result->setProperty("displayCorrelatesWithHostedEndpoints", correlated);
        return juce::var(result);
    }
private:
    void createSeparate(Steinberg::Vst::IComponent& component, const juce::File& plugin,
                        const model::PluginInfo& initialState)
    {
        Steinberg::TUID cid{};
        if (component.getControllerClassId(cid) != Steinberg::kResultOk || ! Steinberg::FUID(cid).isValid())
        {
            error_ = "Component did not expose a separate controller class identity";
            return;
        }
        char identity[33]{};
        Steinberg::FUID(cid).toString(identity);
        controllerClassId_ = identity;
        const auto file = bundleExecutable(plugin);
        executable_ = file.getFullPathName();
        if (! file.existsAsFile() || ! library_.open(executable_))
        {
            error_ = "Could not open the already-hosted plugin executable";
            return;
        }
        using GetFactory = Steinberg::IPluginFactory* PLUGIN_API ();
        const auto getFactory = library_.getFunction<GetFactory>("GetPluginFactory");
        factory_ = getFactory != nullptr ? getFactory() : nullptr; // public SDK entry returns one owned reference
        if (factory_ == nullptr || factory_->createInstance(cid, Steinberg::Vst::IEditController::iid.toTUID(),
                                                           reinterpret_cast<void**>(&controller_)) != Steinberg::kResultOk
            || controller_ == nullptr)
        {
            error_ = "Public factory could not create the declared IEditController";
            if (controller_ != nullptr) controller_->release();
            controller_ = nullptr;
            return;
        }
        host_ = new ProbeHostApplication();
        if (controller_->initialize(host_) != Steinberg::kResultOk)
        {
            error_ = "Separate controller rejected the minimal public host context";
            controller_->release();
            controller_ = nullptr;
            return;
        }
        initialised_ = true;
        source_ = "separate public factory IEditController from the same artifact";
        if (! initialState.state.empty())
        {
            auto* componentStream = new ProbeStateStream(initialState.state);
            componentStateApplied_ = controller_->setComponentState(componentStream) == Steinberg::kResultOk;
            componentStream->release();
            if (! initialState.controllerState.empty())
            {
                auto* controllerStream = new ProbeStateStream(initialState.controllerState);
                controllerStateApplied_ = controller_->setState(controllerStream) == Steinberg::kResultOk;
                controllerStream->release();
            }
        }
    }
    juce::DynamicLibrary library_;
    Steinberg::IPluginFactory* factory_ = nullptr;
    ProbeHostApplication* host_ = nullptr;
    Steinberg::Vst::IEditController* controller_ = nullptr;
    bool initialised_ = false, componentStateApplied_ = false, controllerStateApplied_ = false;
    juce::String source_ = "unavailable", error_, controllerClassId_, executable_;
    std::map<Steinberg::Vst::ParamID, Steinberg::Vst::ParameterInfo> infos_;
};

juce::var object(std::initializer_list<std::pair<juce::Identifier, juce::var>> properties)
{
    auto* result = new juce::DynamicObject();
    for (const auto& [name, value] : properties)
        result->setProperty(name, value);
    return juce::var(result);
}

juce::var inspectNativeInterfaces(juce::AudioPluginInstance& instance)
{
#if JUCE_MAC
    const auto* client = instance.getVST3Client();
    auto* component = client != nullptr ? client->getIComponentPtr() : nullptr;
    if (component == nullptr)
        return object({{"available", false}, {"error", "No public hosted VST3 IComponent"}});

    // FUnknown declares exactly queryInterface/addRef/release and no virtual destructor. The
    // local official SDK appends eight IAudioProcessor methods, in COM order, to those three.
    // This opt-in diagnostic reads only the macOS C++ ABI vptr slots for these public interfaces.
    // It does not invoke the inspected method pointers or read/export their instruction bytes.
    static_assert(! std::has_virtual_destructor_v<Steinberg::Vst::IAudioProcessor>);
    constexpr std::array<const char*, 11> processorMethods{
        "queryInterface", "addRef", "release", "setBusArrangements", "getBusArrangement",
        "canProcessSampleSize", "getLatencySamples", "setupProcessing", "setProcessing",
        "process", "getTailSamples"};
    constexpr std::array<const char*, 14> componentMethods{
        "queryInterface", "addRef", "release", "initialize", "terminate", "getControllerClassId",
        "setIoMode", "getBusCount", "getBusInfo", "getRoutingInfo", "activateBus", "setActive",
        "setState", "getState"};
    const auto describe = [](const void* interfaceObject, const auto& names)
    {
        const void* const* table = nullptr;
        std::memcpy(&table, interfaceObject, sizeof(table));
        juce::Array<juce::var> methods;
        for (size_t index = 0; index < names.size(); ++index)
        {
            Dl_info location{};
            const void* method = table != nullptr ? table[index] : nullptr;
            const bool found = method != nullptr && dladdr(method, &location) != 0
                               && location.dli_fbase != nullptr && location.dli_fname != nullptr;
            auto detail = object({{"name", names[index]}, {"vtableSlot", static_cast<int>(index)},
                                  {"moduleResolved", found}});
            if (found)
            {
                const auto address = reinterpret_cast<std::uintptr_t>(method);
                const auto base = reinterpret_cast<std::uintptr_t>(location.dli_fbase);
                if (address >= base && address - base <= static_cast<std::uintptr_t>(std::numeric_limits<juce::int64>::max()))
                {
                    const juce::File module(juce::String(location.dli_fname));
                    auto* fields = detail.getDynamicObject();
                    fields->setProperty("modulePath", module.getFullPathName());
                    fields->setProperty("moduleFileBytes", module.getSize());
                    fields->setProperty("moduleRelativeOffsetHex", "0x" + juce::String::toHexString(static_cast<juce::int64>(address - base)));
                    fields->setProperty("nearestExportedSymbol", location.dli_sname != nullptr
                        ? juce::var(juce::String(location.dli_sname).substring(0, 512)) : juce::var());
                }
                else
                    detail.getDynamicObject()->setProperty("moduleResolved", false);
            }
            methods.add(detail);
        }
        return methods;
    };
    void* queried = nullptr;
    const auto queriedResult = component->queryInterface(Steinberg::Vst::IAudioProcessor::iid.toTUID(), &queried);
    struct ReleaseProcessor
    {
        void operator()(Steinberg::Vst::IAudioProcessor* processor) const
        {
            if (processor != nullptr) processor->release();
        }
    };
    std::unique_ptr<Steinberg::Vst::IAudioProcessor, ReleaseProcessor> processor(
        queriedResult == Steinberg::kResultOk ? static_cast<Steinberg::Vst::IAudioProcessor*>(queried) : nullptr);
#if defined(__arm64__) || defined(__aarch64__)
    constexpr const char* architecture = "arm64";
#elif defined(__x86_64__)
    constexpr const char* architecture = "x86_64";
#else
    constexpr const char* architecture = "other macOS architecture";
#endif
    return object({{"available", processor != nullptr}, {"scope", "Actual hosted IComponent and queried IAudioProcessor; separate controller metadata is not used"},
                   {"abi", "macOS C++ ABI; FUnknown 3 methods plus IAudioProcessor 8; IComponent adds IPluginBase 2 plus component 9"},
                   {"sdkDeclarations", "JUCE bundled VST3_SDK pluginterfaces/base/funknown.h, base/ipluginbase.h, vst/ivstcomponent.h, vst/ivstaudioprocessor.h"},
                   {"architecture", architecture}, {"pointerBits", static_cast<int>(sizeof(void*) * 8)},
                   {"audioProcessorQueryResult", queriedResult}, {"componentMethods", describe(component, componentMethods)},
                   {"audioProcessorMethods", processor != nullptr ? juce::var(describe(processor.get(), processorMethods)) : juce::var()},
                   {"absoluteAddressesIncluded", false}, {"codeBytesIncluded", false}});
#else
    juce::ignoreUnused(instance);
    return object({{"available", false}, {"error", "Public interface module-offset diagnostics are implemented for macOS only"}});
#endif
}

class ReferenceEditorWindow final : public juce::DocumentWindow, private juce::Timer
{
public:
    ReferenceEditorWindow(juce::AudioPluginInstance& instance, juce::AudioProcessorEditor* editor)
        : DocumentWindow("Sonora reference: " + instance.getName(), juce::Colours::black, closeButton),
          instance_(instance), silent_(juce::jmax(2, instance.getTotalNumInputChannels(), instance.getTotalNumOutputChannels()), inspectBlockSize)
    {
        setUsingNativeTitleBar(true);
        setResizable(editor->isResizable(), false);
        setContentOwned(editor, true);
        centreWithSize(editor->getWidth(), editor->getHeight());
        setVisible(true);
        juce::Process::makeForegroundProcess();
        toFront(true);
        noNotes_.ensureSize(256);
        started_ = juce::Time::getMillisecondCounter();
        startTimer(10); // service the plugin without an audio device; timing is for editor inspection only
    }
    void closeButtonPressed() override
    {
        setVisible(false);
        stopTimer();
        juce::MessageManager::getInstance()->stopDispatchLoop();
    }
private:
    void timerCallback() override
    {
        // Leave ten seconds for state capture and orderly destruction under the parent watchdog.
        if (juce::Time::getMillisecondCounter() - started_ >= 170000u)
        {
            closeButtonPressed();
            return;
        }
        silent_.clear();
        noNotes_.clear();
        instance_.processBlock(silent_, noNotes_);
    }
    juce::AudioPluginInstance& instance_;
    juce::AudioBuffer<float> silent_;
    juce::MidiBuffer noNotes_;
    juce::uint32 started_ = 0;
};

bool showReferenceEditor(juce::AudioPluginInstance& instance, juce::String& error)
{
    auto layout = instance.getBusesLayout();
    if (! layout.outputBuses.isEmpty()) layout.outputBuses.getReference(0) = juce::AudioChannelSet::stereo();
    instance.setBusesLayout(layout);
    instance.setRateAndBufferSizeDetails(inspectSampleRate, inspectBlockSize);
    instance.prepareToPlay(inspectSampleRate, inspectBlockSize);
    juce::AudioBuffer<float> silent(juce::jmax(2, instance.getTotalNumInputChannels(), instance.getTotalNumOutputChannels()), inspectBlockSize);
    silent.clear();
    juce::MidiBuffer noNotes;
    instance.processBlock(silent, noNotes); // flush explicit host writes before the first visible editor frame
    auto* editor = instance.createEditorAndMakeActive();
    if (editor == nullptr)
    {
        error = "Plugin has no editor";
        instance.releaseResources();
        return false;
    }
    {
        ReferenceEditorWindow window(instance, editor);
        juce::MessageManager::getInstance()->runDispatchLoop();
    }
    instance.releaseResources();
    return true;
}

// Read the vendor container with Sonora's native decoder, without instantiating a plugin.
int decodeSerumStateFile(const juce::File& file)
{
    constexpr auto maxBytes = plugins::SerumState::maxDecodedBytes + plugins::SerumState::maxMetadataBytes + 25;
    if (! file.existsAsFile() || file.getSize() < 1 || static_cast<uint64_t>(file.getSize()) > maxBytes)
    {
        std::fprintf(stderr, "State file missing, empty, or exceeds the decoder limit\n");
        return 1;
    }
    juce::MemoryBlock bytes;
    if (! file.loadFileAsData(bytes)) return 1;
    const auto result = plugins::decodeSerumState({static_cast<const uint8_t*>(bytes.getData()), bytes.getSize()});
    if (! result)
    {
        std::fprintf(stderr, "Native state decoder: %s\n", result.error.c_str());
        return 1;
    }
    const auto& state = *result.state;
    const auto textField = [&](std::string_view key) -> juce::var
    {
        const auto* value = state.metadata().find(key);
        return value != nullptr && value->text() != nullptr ? juce::var(juce::String(*value->text())) : juce::var();
    };
    const auto report = object({{"product", textField("product")}, {"component", textField("component")},
                                {"productVersion", textField("productVersion")},
                                {"containerBytes", static_cast<juce::int64>(state.originalBytes().size())},
                                {"decodedBytes", static_cast<juce::int64>(state.decodedBytes().size())},
                                {"rootFields", static_cast<juce::int64>(state.root().map()->size())},
                                {"checksumVerified", false},
                                {"nativeSynthesisSupported", false}});
    std::printf("%s\n", juce::JSON::toString(report).toRawUTF8());
    return 0;
}

// Supply deterministic transport without opening an audio device or plugin editor.
class ProbePlayHead final : public juce::AudioPlayHead
{
public:
    juce::Optional<PositionInfo> getPosition() const override
    {
        PositionInfo position;
        position.setTimeInSamples(sample_);
        position.setTimeInSeconds(static_cast<double>(sample_) / inspectSampleRate);
        position.setPpqPosition(static_cast<double>(sample_) / inspectSampleRate * 2.0);
        position.setBpm(120.0);
        position.setTimeSignature(TimeSignature{4, 4});
        position.setIsPlaying(true);
        return position;
    }

    juce::int64 sample_ = 0;
};

juce::var renderProbe(juce::AudioPluginInstance& instance, bool instrument,
                      const InspectionOptions& options, const juce::File& audioFile = {})
{
    const int totalSamples = options.renderSamples;
    constexpr int noteOnSample = 17;
    constexpr int noteOffSample = 24000;
    constexpr double twoPi = 6.2831853071795864769;
    const int blockSize = options.renderBlockSize;
    auto layout = instance.getBusesLayout();
    if (! layout.inputBuses.isEmpty())
        layout.inputBuses.getReference(0) = instrument ? juce::AudioChannelSet::disabled()
                                                     : juce::AudioChannelSet::stereo();
    if (! layout.outputBuses.isEmpty())
        layout.outputBuses.getReference(0) = juce::AudioChannelSet::stereo();
    const bool stereoLayoutAccepted = instance.setBusesLayout(layout);
    instance.setRateAndBufferSizeDetails(inspectSampleRate, blockSize);
    ProbePlayHead playHead;
    instance.setPlayHead(&playHead);
    instance.prepareToPlay(inspectSampleRate, blockSize);
    const int reportedLatency = instance.getLatencySamples();

    const int inputs = instance.getTotalNumInputChannels();
    const int outputs = instance.getTotalNumOutputChannels();
    juce::AudioBuffer<float> buffer(juce::jmax(2, inputs, outputs), blockSize);
    juce::AudioBuffer<float> recorded;
    if (audioFile != juce::File() && outputs > 0)
    {
        recorded.setSize(outputs, totalSamples);
        recorded.clear();
    }
    juce::MidiBuffer midi;
    midi.ensureSize(2048);
    // Settle queued parameter changes and plugin startup before the recorded stimulus. The same
    // silence is processed for fresh and restored instances so reference exports share a protocol.
    const int prerollSamples = options.prerollSamples;
    for (int start = 0; start < prerollSamples; start += blockSize)
    {
        buffer.clear();
        midi.clear();
        instance.processBlock(buffer, midi);
    }
    double peak = 0.0, energy = 0.0;
    juce::int64 values = 0, nonFiniteValues = 0;
    constexpr juce::uint32 noiseSeed = 0x5eed1234u;
    juce::uint32 noiseState = noiseSeed;
    juce::int64 hostTimeOffset = 0;
    for (int start = 0; start < totalSamples; start += blockSize)
    {
        const int count = juce::jmin(blockSize, totalSamples - start);
        buffer.clear();
        if (! instrument)
            for (int sample = 0; sample < count; ++sample)
            {
                const int position = start + sample;
                double value = 0.0;
                if (options.stimulus == "impulse")
                    value = position == noteOnSample ? options.amplitude : 0.0;
                else if (options.stimulus == "step")
                    value = position >= noteOnSample ? options.amplitude : 0.0;
                else if (options.stimulus == "noise" || options.stimulus == "gatednoise")
                {
                    noiseState ^= noiseState << 13;
                    noiseState ^= noiseState >> 17;
                    noiseState ^= noiseState << 5;
                    value = options.amplitude * (2.0 * static_cast<double>(noiseState) / 4294967295.0 - 1.0);
                    if (options.stimulus == "gatednoise" && position % (options.gateHigh + options.gateLow) >= options.gateHigh) value *= options.gateScale;
                }
                else
                {
                    value = options.amplitude * std::sin(twoPi * options.frequency * static_cast<double>(position) / inspectSampleRate);
                    if (options.stimulus == "gated" && position % (options.gateHigh + options.gateLow) >= options.gateHigh) value *= options.gateScale;
                }
                for (int channel = 0; channel < inputs; ++channel)
                    buffer.setSample(channel, sample, static_cast<float>(value * (channel == 1 ? options.rightScale : 1.0)));
            }
        midi.clear();
        if (instrument && options.midiEvents.empty() && start <= noteOnSample && noteOnSample < start + count)
            midi.addEvent(juce::MidiMessage::noteOn(1, options.note, static_cast<juce::uint8>(options.velocity)), noteOnSample - start);
        if (instrument && options.midiEvents.empty() && start <= noteOffSample && noteOffSample < start + count)
            midi.addEvent(juce::MidiMessage::noteOff(1, options.note), noteOffSample - start);
        for (const auto& event : options.midiEvents)
            if (start <= event.sample && event.sample < start + count)
                midi.addEvent(event.data.data(), event.size, event.sample - start);
        for (const auto& jump : options.playHeadJumps)
            if (jump.sample == start) hostTimeOffset += jump.delta;
        playHead.sample_ = static_cast<juce::int64>(start) + hostTimeOffset;
        juce::AudioBuffer<float> view(buffer.getArrayOfWritePointers(), buffer.getNumChannels(), count);
        instance.processBlock(view, midi);
        if (recorded.getNumChannels() > 0)
            for (int channel = 0; channel < outputs; ++channel)
                recorded.copyFrom(channel, start, buffer, channel, 0, count);
        for (int channel = 0; channel < outputs; ++channel)
            for (int sample = 0; sample < count; ++sample)
            {
                const auto value = static_cast<double>(buffer.getSample(channel, sample));
                if (! std::isfinite(value))
                    ++nonFiniteValues;
                else
                {
                    peak = juce::jmax(peak, std::abs(value));
                    energy += value * value;
                    ++values;
                }
            }
    }
    instance.releaseResources();
    instance.setPlayHead(nullptr);
    bool audioWritten = false;
    if (audioFile != juce::File() && recorded.getNumChannels() > 0 && nonFiniteValues == 0)
    {
        juce::TemporaryFile temporary(audioFile);
        juce::WavAudioFormat format;
        std::unique_ptr<juce::OutputStream> stream(temporary.getFile().createOutputStream());
        if (stream != nullptr)
        {
            const auto writerOptions = juce::AudioFormatWriterOptions{}.withSampleRate(inspectSampleRate)
                .withNumChannels(outputs).withBitsPerSample(32)
                .withSampleFormat(juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
            if (auto writer = format.createWriterFor(stream, writerOptions))
            {
                audioWritten = writer->writeFromAudioSampleBuffer(recorded, 0, totalSamples);
                writer.reset();
                audioWritten = audioWritten && temporary.overwriteTargetFileWithTemporary();
            }
        }
    }
    juce::Array<juce::var> midiFixture;
    for (const auto& event : options.midiEvents)
        midiFixture.add(object({{"sample", event.sample}, {"hex", event.hex}}));
    juce::Array<juce::var> playHeadFixture;
    for (const auto& jump : options.playHeadJumps)
        playHeadFixture.add(object({{"sample", jump.sample}, {"deltaSamples", jump.delta}}));
    const bool defaultNotes = instrument && options.midiEvents.empty();
    return object({{"sampleRate", inspectSampleRate}, {"blockSize", blockSize},
                   {"durationSamples", totalSamples}, {"durationSeconds", totalSamples / inspectSampleRate},
                   {"prerollSamples", prerollSamples}, {"tempoBpm", 120.0},
                   {"prerollPlayHeadClock", "Fixed time0 during preroll; recorded time progresses at48kHz plus cumulative declared host jumps"},
                   {"playHeadJumps", playHeadFixture},
                   {"stimulus", defaultNotes ? "MIDI channel 1, note " + juce::String(options.note) + ", velocity " + juce::String(options.velocity)
                                            : instrument ? juce::String("Explicit channel MIDI fixture")
                                            : options.stimulus + ", amplitude " + juce::String(options.amplitude, 9)},
                   {"inputStimulus", instrument ? juce::var() : juce::var(options.stimulus)},
                   {"inputAmplitude", instrument ? juce::var() : juce::var(options.amplitude)},
                   {"inputRightScale", instrument ? juce::var() : juce::var(options.rightScale)},
                   {"inputGate", instrument || (options.stimulus != "gated" && options.stimulus != "gatednoise") ? juce::var()
                                 : juce::var(juce::String(options.gateHigh) + ":" + juce::String(options.gateLow) + ":" + juce::String(options.gateScale, 9))},
                   {"inputFrequencyHertz", ! instrument && options.stimulus == "sine" ? juce::var(options.frequency) : juce::var()},
                   {"inputOnSample", instrument ? juce::var() : juce::var(options.stimulus == "impulse" || options.stimulus == "step" ? noteOnSample : 0)},
                   {"noiseSeed", ! instrument && options.stimulus == "noise" ? juce::var(static_cast<juce::int64>(noiseSeed)) : juce::var()},
                   {"reportedLatencySamples", reportedLatency},
                   {"midiEvents", midiFixture},
                   {"midiNote", defaultNotes ? juce::var(options.note) : juce::var()},
                   {"midiVelocity", defaultNotes ? juce::var(options.velocity) : juce::var()},
                   {"noteOnSample", defaultNotes ? juce::var(noteOnSample) : juce::var()},
                   {"noteOffSample", defaultNotes ? juce::var(noteOffSample) : juce::var()},
                   {"stereoLayoutAccepted", stereoLayoutAccepted}, {"inputChannels", inputs}, {"outputChannels", outputs},
                   {"peak", peak}, {"rms", values > 0 ? std::sqrt(energy / static_cast<double>(values)) : 0.0},
                   {"audioFile", audioFile != juce::File() ? juce::var(audioFile.getFullPathName()) : juce::var()},
                   {"audioWritten", audioWritten}, {"audioFormat", "WAV IEEE float32; unclipped plugin output"},
                   {"nonFiniteValues", nonFiniteValues}});
}

juce::var inspectInstance(juce::AudioPluginInstance& instance, const juce::PluginDescription& description,
                         const InspectionOptions& options, const model::PluginInfo& initialState, bool& failed)
{
    const auto& overrides = options.overrides;
    const juce::File stateDirectory = options.stateDirectory.isEmpty() ? juce::File() : juce::File(options.stateDirectory);
    const juce::String classId(plugins::vst3ClassIdString(initialState.vst3Uid));
    bool restored = false;
    if (options.componentState.isNotEmpty())
    {
        model::PluginInfo saved;
        juce::String error;
        if (! readStateBytes(options.componentState, saved.state, error)
            || ! readStateBytes(options.controllerState, saved.controllerState, error))
        {
            failed = true;
            return object({{"name", description.name}, {"error", error}});
        }
        for (const auto& part : {std::pair{&saved.state, "processor"}, std::pair{&saved.controllerState, "controller"}})
        {
            if (! options.rawState && part.first->size() >= 9 && std::equal(part.first->begin(), part.first->begin() + 9, "XferJson\0"))
            {
                const auto decoded = plugins::decodeSerumState(*part.first);
                const auto* product = decoded ? decoded.state->metadata().find("product") : nullptr;
                const auto* component = decoded ? decoded.state->metadata().find("component") : nullptr;
                const auto expectedProduct = classId == "56534558667350736572756D20320000" ? "Serum2"
                    : classId == "56534558667351736572756D20322066" ? "Serum2FX" : "";
                if (! decoded || product == nullptr || component == nullptr || product->text() == nullptr
                    || component->text() == nullptr || *product->text() != expectedProduct || *component->text() != part.second)
                {
                    failed = true;
                    return object({{"name", description.name}, {"error", "Serum component/controller identity does not match the selected VST3 class"}});
                }
            }
        }
        const auto preset = plugins::buildVstPreset(initialState.vst3Uid, saved.state, saved.controllerState);
        auto* client = instance.getVST3Client();
        if (client == nullptr || ! client->setPreset(juce::MemoryBlock(preset.data(), preset.size())))
        {
            failed = true;
            return object({{"name", description.name}, {"error", "VST3 component/controller state restore failed"}});
        }
        // The preset client updates VST3 state directly. Refresh JUCE's cached hosted parameter values
        // through its ordinary state API before applying explicit overrides.
        juce::MemoryBlock refreshed;
        instance.getStateInformation(refreshed);
        instance.setStateInformation(refreshed.getData(), static_cast<int>(refreshed.getSize()));
        if (options.settleMilliseconds > 0) juce::Thread::sleep(options.settleMilliseconds);
        restored = true;
    }
    std::vector<juce::HostedAudioProcessorParameter*> resolved;
    for (const auto& requested : overrides)
    {
        juce::HostedAudioProcessorParameter* target = nullptr;
        for (auto* parameter : instance.getParameters())
            if (auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*>(parameter);
                hosted != nullptr && hosted->getParameterID() == requested.hostId)
            {
                target = hosted;
                break;
            }
        if (target == nullptr)
        {
            failed = true;
            return object({{"name", description.name}, {"error", "Unknown host parameter ID: " + requested.hostId}});
        }
        resolved.push_back(target);
    }
    for (const auto& sweep : options.textSweeps)
    {
        bool found = false;
        for (auto* parameter : instance.getParameters())
            if (const auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*>(parameter);
                hosted != nullptr && hosted->getParameterID() == sweep.hostId) found = true;
        if (! found)
        {
            failed = true;
            return object({{"name", description.name}, {"error", "Unknown text-sweep host parameter ID: " + sweep.hostId}});
        }
    }
    for (size_t index = 0; index < overrides.size(); ++index)
        resolved[index]->setValueNotifyingHost(overrides[index].normalised);
    // Hosting wrappers can queue writes until processing; capture controlled state only after flushing.
    if (options.editor)
    {
        juce::String error;
        if (! showReferenceEditor(instance, error))
        {
            failed = true;
            return object({{"name", description.name}, {"error", error}});
        }
    }
    const bool renderFirst = ! overrides.empty() || restored || options.editor || options.renderDirectory.isNotEmpty();
    const juce::File audioFile = options.renderDirectory.isEmpty() ? juce::File()
        : juce::File(options.renderDirectory).getChildFile(classId + "-reference.wav");
    const auto controlledRender = renderFirst ? renderProbe(instance, description.isInstrument, options, audioFile) : juce::var();
    if (audioFile != juce::File() && ! static_cast<bool>(controlledRender.getProperty("audioWritten", false)))
        failed = true;
    PlainController plain(instance, description.fileOrIdentifier, initialState);
    juce::Array<juce::var> parameters;
    for (auto* parameter : instance.getParameters())
    {
        const auto* hosted = dynamic_cast<juce::HostedAudioProcessorParameter*>(parameter);
        const auto current = parameter->getValue();
        const auto defaultValue = parameter->getDefaultValue();
        const int steps = parameter->getNumSteps();
        const bool discrete = parameter->isDiscrete();
        juce::Array<juce::var> textSweeps;
        for (const auto& sweep : options.textSweeps)
            if (hosted != nullptr && hosted->getParameterID() == sweep.hostId)
            {
                juce::Array<juce::var> values, transitions;
                float previousValue = 0.0f;
                juce::String previousText;
                for (int point = 0; point < sweep.points; ++point)
                {
                    const float value = static_cast<float>(point) / static_cast<float>(sweep.points - 1);
                    const auto label = parameter->getText(value, 256);
                    values.add(object({{"normalised", value}, {"text", label}}));
                    if (point > 0 && label != previousText && transitions.size() < 128)
                    {
                        float lower = previousValue, upper = value;
                        for (int iteration = 0; iteration < 32; ++iteration)
                        {
                            const float middle = lower + (upper - lower) * 0.5f;
                            if (middle <= lower || middle >= upper) break;
                            if (parameter->getText(middle, 256) == previousText) lower = middle;
                            else upper = middle;
                        }
                        transitions.add(object({{"lowerNormalised", lower}, {"upperNormalised", upper},
                                                {"lowerText", previousText}, {"upperText", parameter->getText(upper, 256)},
                                                {"adjacentFloatBoundary", std::nextafter(lower, 1.0f) >= upper}}));
                    }
                    previousValue = value;
                    previousText = label;
                }
                textSweeps.add(object({{"scope", "Active hosted parameter getText; sampled values do not change the parameter"},
                                       {"sampleCount", sweep.points}, {"values", values}, {"transitions", transitions},
                                       {"transitionLimit", 128},
                                       {"boundaryNote", "First text change in each sampled interval; a coarse grid can miss additional changes"}}));
            }
        juce::Array<juce::var> choices;
        if (discrete && steps >= 2 && steps <= 512)
            for (int step = 0; step < steps; ++step)
            {
                const float normalised = static_cast<float>(step) / static_cast<float>(steps - 1);
                choices.add(object({{"normalised", normalised}, {"text", parameter->getText(normalised, 256)}}));
            }
        parameters.add(object({{"index", parameter->getParameterIndex()},
                               {"hostId", hosted != nullptr ? juce::var(hosted->getParameterID()) : juce::var()},
                               {"name", parameter->getName(256)}, {"label", parameter->getLabel()},
                               {"defaultNormalised", defaultValue}, {"currentNormalised", current},
                               {"defaultText", parameter->getText(defaultValue, 256)},
                               {"currentText", parameter->getCurrentValueAsText()},
                               {"textAtZero", parameter->getText(0.0f, 256)},
                               {"textAtHalf", parameter->getText(0.5f, 256)},
                               {"textAtOne", parameter->getText(1.0f, 256)},
                               {"textSweeps", textSweeps},
                               {"numSteps", steps}, {"discrete", discrete}, {"boolean", parameter->isBoolean()},
                               {"automatable", parameter->isAutomatable()}, {"valueStrings", choices},
                               {"vst3", hosted != nullptr ? plain.values(hosted->getParameterID(), current,
                                   parameter->getText(0.0f, 256), parameter->getText(0.5f, 256), parameter->getText(1.0f, 256)) : juce::var()}}));
    }
    juce::Array<juce::var> groups;
    for (const auto* group : instance.getParameterTree().getSubgroups(true))
    {
        juce::Array<juce::var> ids;
        for (const auto* parameter : group->getParameters(false))
            if (const auto* hosted = dynamic_cast<const juce::HostedAudioProcessorParameter*>(parameter))
                ids.add(hosted->getParameterID());
        groups.add(object({{"id", group->getID()}, {"name", group->getName()}, {"parameterHostIds", ids}}));
    }

    juce::MemoryBlock state;
    instance.getStateInformation(state);
    const auto stateXml = state.getSize() <= static_cast<size_t>(std::numeric_limits<int>::max())
                              ? juce::AudioProcessor::getXmlFromBinary(state.getData(), static_cast<int>(state.getSize()))
                              : nullptr;
    auto stateReport = object({{"juceStateBytes", static_cast<juce::int64>(state.getSize())},
                               {"juceContainerTag", stateXml != nullptr ? juce::var(stateXml->getTagName()) : juce::var()},
                               {"source", restored ? "explicit component/controller restore" : "fresh plugin instance"},
                               {"overridesApplied", ! overrides.empty()}, {"editorShown", options.editor},
                               {"componentStateRestored", restored}, {"restoreClassIdChecked", restored},
                               {"sourceComponentFile", restored ? juce::var(options.componentState) : juce::var()},
                               {"sourceControllerFile", restored ? juce::var(options.controllerState) : juce::var()},
                               {"payloadIncluded", false}});
    if (const auto* client = instance.getVST3Client())
    {
        const auto preset = client->getPreset();
        const auto* bytes = static_cast<const uint8_t*>(preset.getData());
        model::PluginInfo decoded;
        const bool parsed = ! preset.isEmpty() && plugins::readVstPreset({bytes, bytes + preset.getSize()}, decoded);
        auto* result = stateReport.getDynamicObject();
        result->setProperty("vstPresetBytes", static_cast<juce::int64>(preset.getSize()));
        result->setProperty("vstPresetParsed", parsed);
        if (parsed)
        {
            const juce::String capturedClassId(plugins::vst3ClassIdString(decoded.vst3Uid));
            result->setProperty("vstPresetMagic", "VST3");
            result->setProperty("vstPresetVersion", 1);
            result->setProperty("classId", capturedClassId);
            result->setProperty("componentStateBytes", static_cast<juce::int64>(decoded.state.size()));
            result->setProperty("controllerStateBytes", static_cast<juce::int64>(decoded.controllerState.size()));
            if (stateDirectory != juce::File())
            {
                const auto componentFile = stateDirectory.getChildFile(capturedClassId + "-component.bin");
                const auto controllerFile = stateDirectory.getChildFile(capturedClassId + "-controller.bin");
                const bool saved = componentFile.replaceWithData(decoded.state.data(), decoded.state.size())
                                   && controllerFile.replaceWithData(decoded.controllerState.data(), decoded.controllerState.size());
                result->setProperty("stateExported", saved);
                if (saved)
                {
                    result->setProperty("componentStateFile", componentFile.getFullPathName());
                    result->setProperty("controllerStateFile", controllerFile.getFullPathName());
                }
                else
                    failed = true;
            }
        }
    }

    return object({{"name", description.name}, {"format", description.pluginFormatName},
                   {"manufacturer", description.manufacturerName}, {"version", description.version},
                   {"category", description.category}, {"identifier", description.fileOrIdentifier},
                   {"juceUniqueId", description.uniqueId}, {"instrument", description.isInstrument},
                   {"vst3PlainControllerAvailable", plain.available()}, {"vst3Controller", plain.provenance()}, {"parameterGroups", groups},
                   {"parameterCount", parameters.size()}, {"parameters", parameters}, {"state", stateReport},
                   {"nativeInterfaces", options.nativeInterfaces ? inspectNativeInterfaces(instance) : juce::var()},
                   {"render", renderFirst ? controlledRender : renderProbe(instance, description.isInstrument, options)}});
}

int inspectFileWorker(plugins::PluginHost& host, const juce::String& path, const juce::File& output,
                      const InspectionOptions& options)
{
    juce::Array<juce::var> reports;
    bool failed = false;
    for (const auto& directory : {options.stateDirectory, options.renderDirectory})
        if (directory.isNotEmpty() && juce::File(directory).createDirectory().failed())
            return 1;
    uint32_t wantedSignature = 0;
    if (options.classId.isNotEmpty())
        for (int word = 0; word < 4; ++word)
        {
            const auto text = options.classId.substring(word * 8, word * 8 + 8).toStdString();
            uint32_t value = 0;
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value, 16);
            if (parsed.ec != std::errc{}) return 2;
            wantedSignature = wantedSignature * 31u + value;
        }
    for (int formatIndex = 0; formatIndex < host.formats().getNumFormats(); ++formatIndex)
    {
        auto* format = host.formats().getFormat(formatIndex);
        if (! format->fileMightContainThisPluginType(path))
            continue;
        juce::OwnedArray<juce::PluginDescription> descriptions;
        format->findAllTypesForFile(descriptions, path);
        for (const auto* description : descriptions)
        {
            if (options.classId.isNotEmpty() && description->uniqueId != static_cast<int32_t>(wantedSignature))
                continue;
            juce::String error;
            auto instance = host.formats().createPluginInstance(*description, inspectSampleRate, inspectBlockSize, error);
            if (instance == nullptr)
            {
                reports.add(object({{"name", description->name}, {"error", error}}));
                failed = true;
            }
            else
            {
                model::PluginInfo initialState;
                const bool hasClass = readClassState(*instance, initialState);
                if (options.classId.isNotEmpty()
                    && (! hasClass || options.classId != juce::String(plugins::vst3ClassIdString(initialState.vst3Uid))))
                    continue;
                if ((! hasClass) && (options.renderDirectory.isNotEmpty() || options.componentState.isNotEmpty()))
                {
                    reports.add(object({{"name", description->name}, {"error", "State/audio export requires a VST3 class identity"}}));
                    failed = true;
                    continue;
                }
                reports.add(inspectInstance(*instance, *description, options, initialState, failed));
            }
        }
    }
    if (reports.isEmpty())
        return 1;
    juce::Array<juce::var> requested;
    for (const auto& parameter : options.overrides)
        requested.add(object({{"hostId", parameter.hostId}, {"normalised", parameter.normalised}}));
    const auto report = object({{"schemaVersion", 3}, {"source", "isolated plugin instances; explicit state/host overrides/editor changes are identified per class"},
                                {"normalisedValueNote", "JUCE host values/text are supplemented by exact public VST3 normalizedParamToPlain; controller provenance and endpoint display correlation are reported"},
                                {"stateNote", "Opaque component/controller payloads are reported by size only; no native DSP or preset conversion is implied"},
                                {"requestedOverrides", requested}, {"classes", reports}});
    if (! output.replaceWithText(juce::JSON::toString(report)))
        return 1;
    return failed ? 1 : 0;
}

int inspectFile(const juce::String& path, const InspectionOptions& options)
{
    juce::TemporaryFile reportFile(".json");
    juce::ChildProcess child;
    const auto executable = juce::File::getSpecialLocation(juce::File::currentExecutableFile);
    juce::StringArray arguments{executable.getFullPathName(), "--inspect-file-worker", path,
                                "--output", reportFile.getFile().getFullPathName()};
    for (const auto& option : {std::pair{"--state-dir", options.stateDirectory},
                              std::pair{"--render-out", options.renderDirectory},
                              std::pair{"--class-id", options.classId},
                              std::pair{"--component-state", options.componentState},
                              std::pair{"--controller-state", options.controllerState}})
    {
        if (option.second.isNotEmpty())
        {
            arguments.add(option.first);
            arguments.add(option.second);
        }
    }
    if (options.editor) arguments.add("--editor");
    if (options.nativeInterfaces) arguments.add("--native-interfaces");
    if (options.rawState) arguments.add("--raw-state");
    arguments.add("--note");
    arguments.add(juce::String(options.note));
    arguments.add("--velocity");
    arguments.add(juce::String(options.velocity));
    arguments.add("--stimulus");
    arguments.add(options.stimulus);
    arguments.add("--amplitude");
    arguments.add(juce::String(options.amplitude, 17));
    arguments.add("--frequency");
    arguments.add(juce::String(options.frequency, 17));
    arguments.add("--right-scale");
    arguments.add(juce::String(options.rightScale, 17));
    if (options.stimulus == "gated" || options.stimulus == "gatednoise")
    {
        arguments.add("--gate");
        arguments.add(juce::String(options.gateHigh) + ":" + juce::String(options.gateLow) + ":" + juce::String(options.gateScale, 17));
    }
    arguments.add("--preroll-samples");
    arguments.add(juce::String(options.prerollSamples));
    arguments.add("--settle-ms");
    arguments.add(juce::String(options.settleMilliseconds));
    arguments.add("--render-block-size");
    arguments.add(juce::String(options.renderBlockSize));
    arguments.add("--render-samples");
    arguments.add(juce::String(options.renderSamples));
    for (const auto& jump : options.playHeadJumps)
    {
        arguments.add("--playhead-jump");
        arguments.add(juce::String(jump.sample) + ":" + juce::String(jump.delta));
    }
    for (const auto& event : options.midiEvents)
    {
        arguments.add("--midi-event");
        arguments.add(juce::String(event.sample) + ":" + event.hex);
    }
    for (const auto& sweep : options.textSweeps)
    {
        arguments.add("--text-sweep");
        arguments.add(sweep.hostId + "=" + juce::String(sweep.points));
    }
    for (const auto& parameter : options.overrides)
    {
        arguments.add("--set");
        arguments.add(parameter.hostId + "=" + juce::String(parameter.normalised, 9));
    }
    if (! child.start(arguments, 0))
    {
        std::fprintf(stderr, "Could not start the plugin inspection worker\n");
        return 1;
    }
    const auto start = juce::Time::getMillisecondCounter();
    while (child.isRunning())
    {
        const juce::uint32 timeout = options.editor ? 180000u : 60000u;
        if (juce::Time::getMillisecondCounter() - start > timeout)
        {
            child.kill();
            std::fprintf(stderr, "Plugin inspection timed out after %u s\n", timeout / 1000u);
            return 124;
        }
        juce::Thread::sleep(20);
    }
    const int result = static_cast<int>(child.getExitCode());
    if (reportFile.getFile().existsAsFile())
    {
        const auto report = reportFile.getFile().loadFileAsString();
        if (options.outputPath.isEmpty())
            std::printf("%s\n", report.toRawUTF8());
        else if (! juce::File(options.outputPath).replaceWithText(report))
        {
            std::fprintf(stderr, "Could not write inspection report\n");
            return 1;
        }
        else
            std::printf("Inspection report: %s\n", options.outputPath.toRawUTF8());
    }
    else
    {
        std::fprintf(stderr, "Plugin inspection failed (exit %d)\n", result);
        return result != 0 ? result : 1;
    }
    return result;
}

void collect(const std::vector<model::Device>& devices, std::vector<const model::Device*>& out)
{
    for (const auto& d : devices)
    {
        if (d.plugin)
            out.push_back(&d);
        for (const auto& c : d.chains)
            collect(c.devices, out);
    }
}

void probeSet(plugins::PluginHost& host, const juce::File& file, bool render)
{
    auto r = io::loadAls(file);
    if (! r)
    {
        std::printf("ERROR %s\n", r.error.c_str());
        return;
    }
    std::vector<const model::Device*> plugs;
    for (const auto& t : r.song->tracks)
        collect(t.devices, plugs);
    for (const auto& t : r.song->returns)
        collect(t.devices, plugs);
    collect(r.song->main.devices, plugs);

    int ok = 0;
    for (const auto* d : plugs)
    {
        const auto match = host.findMatch(*d->plugin, d->kind);
        std::printf("%-5s %-28s state=%7zu B  ", model::toString(d->kind), d->plugin->name.c_str(), d->plugin->state.size());
        if (! match)
        {
            std::printf("-> not in cache\n");
            continue;
        }
        juce::String error;
        auto inst = host.instantiate(*match, *d->plugin, d->kind, 48000.0, 512, error);
        if (inst == nullptr)
        {
            std::printf("-> %s FAILED: %s\n", match->pluginFormatName.toRawUTF8(), error.toRawUTF8());
            continue;
        }
        juce::MemoryBlock after;
        inst->getStateInformation(after);
        std::printf("-> %s OK  params=%d  stateAfterRestore=%zu B %s", match->pluginFormatName.toRawUTF8(),
                    inst->getParameters().size(), after.getSize(), error.isEmpty() ? "" : ("(" + error + ")").toRawUTF8());
        if (render)
        {
            inst->prepareToPlay(48000.0, 512);
            juce::AudioBuffer<float> buf(juce::jmax(2, inst->getTotalNumOutputChannels(), inst->getTotalNumInputChannels()), 512);
            juce::MidiBuffer midi;
            float peak = 0;
            for (int i = 0; i < 94; ++i)
            {
                buf.clear();
                midi.clear();
                if (i == 0)
                    midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
                inst->processBlock(buf, midi);
                peak = juce::jmax(peak, buf.getMagnitude(0, 512));
            }
            inst->releaseResources();
            std::printf(" rendered peak=%.3f", (double) peak);
        }
        std::printf("\n");
        ++ok;
    }
    std::printf("\n%d / %zu plugins instantiated\n", ok, plugs.size());
}
} // namespace

int main(int argc, char** argv)
{
    // --data-folder <name> picks the data folder (and so the plug-in cache) before anything reads it; strip it from argv.
    for (int index = 1; index + 1 < argc; ++index)
    {
        if (std::strcmp(argv[index], "--data-folder") != 0)
            continue;
        if (! sonora::core::setAppDataFolderName(argv[index + 1]))
        {
            std::fprintf(stderr, "plugin_probe: invalid --data-folder name \"%s\"\n", argv[index + 1]);
            return 2;
        }
        for (int from = index + 2; from <= argc; ++from)
            argv[from - 2] = argv[from]; // argv[argc] is the terminating nullptr
        argc -= 2;
        break;
    }
    bool editorWorker = argc > 1 && std::strcmp(argv[1], "--inspect-file-worker") == 0;
    bool editorRequested = false;
    for (int index = 2; index < argc; ++index)
        editorRequested = editorRequested || std::strcmp(argv[index], "--editor") == 0;
    if (editorWorker && editorRequested)
    {
        editorArgc = argc;
        editorArgv = argv;
        juce::JUCEApplicationBase::createInstance = []() -> juce::JUCEApplicationBase* { return new ReferenceProbeApplication(); };
#if defined(_WIN32)
        return juce::JUCEApplicationBase::main(); // JUCE reads the Windows command line itself; no (argc, argv) overload
#else
        std::vector<const char*> arguments;
        arguments.reserve(static_cast<size_t>(argc));
        for (int index = 0; index < argc; ++index) arguments.push_back(argv[index]);
        return juce::JUCEApplicationBase::main(argc, arguments.data());
#endif
    }
    return runProbeCommand(argc, argv);
}

static int runProbeCommand(int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI juceInit; // plugins expect a message thread (this one)
    plugins::PluginHost host;

    if (argc < 2)
    {
        std::fprintf(stderr, "usage: plugin_probe [--data-folder <name>] --scan | --scan-file <plugin> | --list | --decode-serum-state <state.bin> | --inspect-file <plugin> [--output <json>] [--state-dir <dir>] [--render-out <dir>] [--class-id <CID>] [--component-state <bin>] [--controller-state <bin>] [--set <hostId>=<normalised>] ... [--note <0..127>] [--velocity <1..127>] [--stimulus sine|impulse|step|noise] [--amplitude <0..1>] [--frequency <Hz>] [--right-scale <-1..1>] [--midi-event <sample>:<channel-message-hex>] ... [--render-block-size <128|256|512|1024|2048>] [--render-samples <48000..480000>] [--preroll-samples <0..65536, block aligned>] [--settle-ms <0..30000>] [--playhead-jump <sample>:<delta-samples>] ... [--text-sweep <hostId>=<2..4097 points>] ... [--native-interfaces] [--raw-state] [--editor] | <set.als> [--render]\n");
        return 2;
    }
    const juce::String a = argv[1];
    if (a == "--decode-serum-state")
        return argc == 3 ? decodeSerumStateFile(juce::File(juce::String(argv[2]))) : 2;
    if (a == "--inspect-file" || a == "--inspect-file-worker")
    {
        if (argc < 3)
            return 2;
        const juce::String path = argv[2];
        InspectionOptions options;
        bool renderSamplesSpecified = false;
        for (int index = 3; index < argc;)
        {
            const juce::String option = argv[index++];
            if (option == "--editor")
            {
                if (options.editor) return 2;
                options.editor = true;
                continue;
            }
            if (option == "--native-interfaces")
            {
                if (options.nativeInterfaces) return 2;
                options.nativeInterfaces = true;
                continue;
            }
            if (option == "--raw-state")
            {
                if (options.rawState) return 2;
                options.rawState = true;
                continue;
            }
            if (index == argc) return 2;
            const juce::String argument = argv[index++];
            if (option == "--text-sweep")
            {
                const int separator = argument.indexOfChar('=');
                if (separator <= 0 || options.textSweeps.size() >= 16) return 2;
                const auto text = argument.substring(separator + 1).toStdString();
                ParameterTextSweep sweep{argument.substring(0, separator), 0};
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), sweep.points);
                if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
                    || sweep.points < 2 || sweep.points > 4097) return 2;
                options.textSweeps.push_back(std::move(sweep));
                continue;
            }
            if (option == "--settle-ms")
            {
                const auto text = argument.toStdString();
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), options.settleMilliseconds);
                if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
                    || options.settleMilliseconds < 0 || options.settleMilliseconds > 30000) return 2;
                continue;
            }
            if (option == "--preroll-samples")
            {
                const auto text = argument.toStdString();
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), options.prerollSamples);
                if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
                    || options.prerollSamples < 0 || options.prerollSamples > 65536) return 2;
                continue;
            }
            if (option == "--render-block-size")
            {
                const auto text = argument.toStdString();
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), options.renderBlockSize);
                if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
                    || options.renderBlockSize < 128 || options.renderBlockSize > 2048
                    || (options.renderBlockSize & (options.renderBlockSize - 1)) != 0) return 2;
                continue;
            }
            if (option == "--render-samples")
            {
                if (renderSamplesSpecified) return 2;
                renderSamplesSpecified = true;
                const auto text = argument.toStdString();
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), options.renderSamples);
                if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
                    || options.renderSamples < 48000 || options.renderSamples > 480000) return 2;
                continue;
            }
            if (option == "--playhead-jump")
            {
                const int separator = argument.indexOfChar(':');
                if (separator <= 0 || options.playHeadJumps.size() >= 16) return 2;
                const auto position = argument.substring(0, separator).toStdString();
                const auto delta = argument.substring(separator + 1).toStdString();
                ProbePlayHeadJump jump;
                const auto parsedPosition = std::from_chars(position.data(), position.data() + position.size(), jump.sample);
                const auto parsedDelta = std::from_chars(delta.data(), delta.data() + delta.size(), jump.delta);
                if (parsedPosition.ec != std::errc{} || parsedPosition.ptr != position.data() + position.size()
                    || parsedDelta.ec != std::errc{} || parsedDelta.ptr != delta.data() + delta.size()
                    || jump.sample < 0 || jump.sample >= 480000
                    || jump.delta < -480000 || jump.delta > 480000) return 2;
                options.playHeadJumps.push_back(jump);
                continue;
            }
            if (option == "--midi-event")
            {
                const int separator = argument.indexOfChar(':');
                if (separator <= 0 || options.midiEvents.size() >= 128) return 2;
                const auto position = argument.substring(0, separator).toStdString();
                ProbeMidiEvent event;
                const auto parsed = std::from_chars(position.data(), position.data() + position.size(), event.sample);
                event.hex = argument.substring(separator + 1).toUpperCase();
                if (parsed.ec != std::errc{} || parsed.ptr != position.data() + position.size()
                    || event.sample < 0 || event.sample >= 480000 || (event.hex.length() != 4 && event.hex.length() != 6)
                    || ! event.hex.containsOnly("0123456789ABCDEF")) return 2;
                event.size = event.hex.length() / 2;
                for (int byte = 0; byte < event.size; ++byte)
                    event.data[static_cast<size_t>(byte)] = static_cast<juce::uint8>(event.hex.substring(byte * 2, byte * 2 + 2).getHexValue32());
                const int status = event.data[0];
                const int required = (status & 0xf0) == 0xc0 || (status & 0xf0) == 0xd0 ? 2 : 3;
                if (status < 0x80 || status >= 0xf0 || event.size != required
                    || event.data[1] > 0x7f || (event.size == 3 && event.data[2] > 0x7f)) return 2;
                options.midiEvents.push_back(std::move(event));
                continue;
            }
            if (option == "--stimulus")
            {
                if (argument != "sine" && argument != "impulse" && argument != "step" && argument != "noise" && argument != "gated" && argument != "gatednoise")
                    return 2;
                options.stimulus = argument;
                continue;
            }
            if (option == "--gate")
            {
                const auto parts = juce::StringArray::fromTokens(argument, ":", "");
                if (parts.size() != 3) return 2;
                const int high = parts[0].getIntValue(), low = parts[1].getIntValue();
                const double scale = parts[2].getDoubleValue();
                if (high < 1 || low < 1 || high > 480000 || low > 480000 || ! std::isfinite(scale) || scale < 0.0 || scale > 1.0) return 2;
                options.gateHigh = high; options.gateLow = low; options.gateScale = scale;
                continue;
            }
            if (option == "--amplitude" || option == "--frequency" || option == "--right-scale")
            {
                char* end = nullptr;
                const auto value = std::strtod(argument.toRawUTF8(), &end);
                if (end == argument.toRawUTF8() || *end != 0 || ! std::isfinite(value)
                    || (option == "--amplitude" ? value < 0.0 || value > 1.0
                        : option == "--right-scale" ? value < -1.0 || value > 1.0
                        : value <= 0.0 || value >= inspectSampleRate * 0.5))
                    return 2;
                (option == "--amplitude" ? options.amplitude : option == "--right-scale" ? options.rightScale : options.frequency) = value;
                continue;
            }
            if (option == "--note" || option == "--velocity")
            {
                const auto text = argument.toStdString();
                int value = 0;
                const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
                if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()
                    || value < (option == "--velocity" ? 1 : 0) || value > 127)
                    return 2;
                (option == "--note" ? options.note : options.velocity) = value;
                continue;
            }
            if (option == "--set")
            {
                const juce::String assignment = argument;
                const auto separator = assignment.indexOfChar('=');
                if (separator <= 0 || separator == assignment.length() - 1)
                    return 2;
                const auto text = assignment.substring(separator + 1);
                char* end = nullptr;
                const auto value = std::strtod(text.toRawUTF8(), &end);
                if (end == text.toRawUTF8() || *end != 0 || ! std::isfinite(value) || value < 0.0 || value > 1.0)
                    return 2;
                options.overrides.push_back({assignment.substring(0, separator), static_cast<float>(value)});
                continue;
            }
            auto* destination = option == "--output" ? &options.outputPath
                : option == "--state-dir" ? &options.stateDirectory
                : option == "--render-out" ? &options.renderDirectory
                : option == "--class-id" ? &options.classId
                : option == "--component-state" ? &options.componentState
                : option == "--controller-state" ? &options.controllerState : nullptr;
            if (destination == nullptr || destination->isNotEmpty() || argument.isEmpty())
                return 2;
            *destination = argument;
        }
        if (options.prerollSamples % options.renderBlockSize != 0) return 2;
        for (const auto& jump : options.playHeadJumps)
            if (jump.sample >= options.renderSamples || jump.sample % options.renderBlockSize != 0) return 2;
        for (const auto& event : options.midiEvents)
            if (event.sample >= options.renderSamples) return 2;
        options.classId = options.classId.toUpperCase();
        if (options.classId.isNotEmpty() && (options.classId.length() != 32 || ! options.classId.containsOnly("0123456789ABCDEF")))
            return 2;
        if ((options.editor || options.componentState.isNotEmpty()) && options.classId.isEmpty())
        {
            std::fprintf(stderr, "--editor and state restore require --class-id to select an exact VST3 class\n");
            return 2;
        }
        if (options.controllerState.isNotEmpty() && options.componentState.isEmpty()) return 2;
        if (a == "--inspect-file-worker")
            return options.outputPath.isEmpty() ? 2 : inspectFileWorker(host, path, juce::File(options.outputPath), options);
        return inspectFile(path, options);
    }
    host.loadCache();
    if (a == "--scan")
    {
        host.scan(false, [](float p, const juce::String& n) { std::printf("\r%3d%% %-60s", (int) (p * 100), n.toRawUTF8()); std::fflush(stdout); });
        std::printf("\n%d plugins in cache\n", host.knownPlugins().getNumTypes());
    }
    else if (a == "--scan-file" && argc > 2)
    {
        const auto before = host.knownPlugins().getNumTypes();
        juce::String error;
        for (int i = 0; i < host.formats().getNumFormats(); ++i)
        {
            auto* format = host.formats().getFormat(i);
            if (format->fileMightContainThisPluginType(argv[2]))
                if (! host.scanFile(format->getName(), argv[2], error))
                    std::printf("FAILED (blacklisted): %s\n", error.toRawUTF8());
        }
        std::printf("%d plugin(s) added\n", host.knownPlugins().getNumTypes() - before);
        host.saveCache();
    }
    else if (a == "--list")
    {
        for (const auto& d : host.knownPlugins().getTypes())
            std::printf("%-6s %-30s %s\n", d.pluginFormatName.toRawUTF8(), d.name.toRawUTF8(), d.fileOrIdentifier.toRawUTF8());
    }
    else
    {
        probeSet(host, juce::File(juce::String::fromUTF8(argv[1])), argc > 2 && juce::String(argv[2]) == "--render");
    }
    return 0;
}

#pragma once
// The one interface every audio/MIDI device implements: native devices, hosted plugins,
// racks and placeholders. The engine only talks to this.
//
// Threading contract (IMPORTANT, see docs/ARCHITECTURE.md "Threads"):
//   - Constructor, prepare(), parameters() and destruction: message thread.
//   - process(): audio thread only. Must not allocate, lock, or log.
//   - setParameter()/getParameter(): any thread (implementations use atomics).

#include <juce_audio_basics/juce_audio_basics.h>

#include <array>
#include <atomic>
#include <cmath>
#include <string>
#include <vector>

namespace sonora::devices
{

// UI-55: a device's output level, shown by the meter in the gap after it in the device chain (like the reference).
// The engine measures every block after process(); each value keeps the loudest block until the UI takes it.
struct OutputMeter
{
    std::array<std::atomic<float>, 2> peak{}, rms{};

    // Audio thread: no allocation or locks. A mono buffer meters both sides.
    void measure(const juce::AudioBuffer<float>& audio, int n) noexcept
    {
        if (n <= 0 || audio.getNumChannels() <= 0) return;
        for (size_t ch = 0; ch < 2; ++ch)
        {
            const float* x = audio.getReadPointer(std::min((int) ch, audio.getNumChannels() - 1));
            float top = 0.0f, sum = 0.0f;
            for (int i = 0; i < n; ++i)
            {
                const float v = std::abs(x[i]);
                top = v > top ? v : top;
                sum += v * v;
            }
            if (! std::isfinite(top) || ! std::isfinite(sum)) continue;
            const float level = std::sqrt(sum / (float) n);
            if (top > peak[ch].load(std::memory_order_relaxed)) peak[ch].store(top, std::memory_order_relaxed);
            if (level > rms[ch].load(std::memory_order_relaxed)) rms[ch].store(level, std::memory_order_relaxed);
        }
    }
    // Message thread: the loudest peak / RMS since the last call, per channel.
    std::array<float, 2> takePeak() noexcept { return {peak[0].exchange(0.0f), peak[1].exchange(0.0f)}; }
    std::array<float, 2> takeRms() noexcept { return {rms[0].exchange(0.0f), rms[1].exchange(0.0f)}; }
};

struct ParameterInfo
{
    std::string id;   // stable id, for native devices the .als parameter path
    std::string name; // short label shown on the device UI
    float minValue = 0.0f;
    float maxValue = 1.0f;
    float defaultValue = 0.0f;
    std::string unit; // "dB", "Hz", "%", ""
    bool isToggle = false;
    std::vector<std::string> choices; // non-empty for enum parameters
    bool plainValue = false; // Remoteable/user value leaf, without Manual/automation children
};

inline ParameterInfo makeParam(std::string id, std::string name, float minValue, float maxValue, float defaultValue,
                               std::string unit = {}, bool isToggle = false, std::vector<std::string> choices = {})
{
    ParameterInfo p;
    p.id = std::move(id);
    p.name = std::move(name);
    p.minValue = minValue;
    p.maxValue = maxValue;
    p.defaultValue = defaultValue;
    p.unit = std::move(unit);
    p.isToggle = isToggle;
    p.choices = std::move(choices);
    return p;
}

inline ParameterInfo makePlainParam(ParameterInfo parameter)
{
    parameter.plainValue = true;
    return parameter;
}

// What a device may want to know about the song while it processes (tempo-synced delays and LFOs).
// Written by the engine once per block, before process(), on the audio thread.
struct HostInfo
{
    double tempo = 120.0; // BPM
    double beats = 0.0;   // timeline position at the start of the block
    bool playing = false;
    double sampleRate = 44100.0;
    // PLG-10: the rest of the transport, for hosted plugins' AudioPlayHead (tempo sync, bar position, mastering assistants
    // that only listen while the host plays).
    int timeSigNumerator = 4, timeSigDenominator = 4;
    bool looping = false;
    double loopStart = 0.0, loopEnd = 0.0; // beats
};

class DeviceProcessor
{
public:
    virtual ~DeviceProcessor() = default;

    virtual std::string name() const = 0;
    virtual bool isInstrument() const { return false; }
    // Hosted third-party plugin: its parameters may change by themselves, so they cannot be read back to detect hand edits.
    virtual bool isHostedPlugin() const { return false; }

    virtual void prepare(double sampleRate, int maxBlockSize) = 0;
    // In-place. `audio` always has 2 channels. Instruments add their output to `audio`.
    virtual void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) = 0;
    virtual void reset() {}
    virtual void setHostInfo(const HostInfo& /*info*/) {}
    // External sidechain: the engine points the device at another track's tapped audio for this block
    // (nullptr == use the device's own input). Audio thread, before process().
    virtual void setSidechain(const juce::AudioBuffer<float>* /*audio*/, float /*gain*/) {}
    virtual int latencySamples() const { return 0; }

    // ---- parameters (plain values, i.e. in the units of ParameterInfo)
    virtual const std::vector<ParameterInfo>& parameters() const
    {
        static const std::vector<ParameterInfo> none;
        return none;
    }
    // Which parameters the generic device UI shows (indices into parameters(), in display order).
    // Default: the first 12. Devices with a custom UI ignore this.
    virtual std::vector<int> displayParameters() const
    {
        std::vector<int> out;
        for (int i = 0; i < (int) parameters().size() && i < 12; ++i)
            out.push_back(i);
        return out;
    }
    // Devices with many parameters describe a sectioned panel (Live-style boxes with a caption). Each group lists
    // indices into parameters(); toggles become buttons, enums cycle buttons, the rest knobs. Empty == use
    // displayParameters(). Message thread.
    struct UiGroup
    {
        std::string title;
        std::vector<int> params;
        int knobColumns = 0; // 0 == as many columns as needed for two rows
    };
    virtual std::vector<UiGroup> uiGroups() const { return {}; }
    // For uiGroups(): false hides a control that does not apply to the current mode (e.g. the ms time while synced).
    virtual bool parameterShown(int /*index*/) const { return true; }
    // Hosted plugins: index into parameters() of the parameter with the plugin's own id (or, failing that,
    // the given name); -1 when unknown. Message thread.
    virtual int findParameterByHostId(int /*id*/, const std::string& /*name*/) const { return -1; }
    // UI-65, hosted plugins: the plugin's own id (what the set stores as ParameterId) and full name of parameter `index`;
    // false for native devices and parameters the plugin does not let a host automate. Message thread.
    virtual bool hostParameter(int /*index*/, int& /*id*/, std::string& /*name*/) const { return false; }
    // UI-65, hosted plugins: edits made in the plugin's own window (VST3 performEdit / AU parameter events), so automation
    // can be recorded and overridden although the value cannot be read back. Lock-free; any thread.
    struct HandEdit
    {
        uint32_t serial = 0;   // changes with every reported edit of the parameter
        float value = 0.0f;    // the value it reported last
        float before = 0.0f;   // the value when the gesture began (or before the last edit outside a gesture)
        bool gesture = false;  // the user holds the control now
    };
    virtual uint32_t handEditTotal() const { return 0; } // changes with every reported edit of any parameter
    virtual HandEdit handEdit(int /*index*/) const { return {}; }
    // Audio thread (automation): true when `index` was edited by hand since the previous call for it.
    virtual bool takeHandEdit(int /*index*/) { return false; }
    virtual float getParameter(int /*index*/) const { return 0.0f; }
    virtual void setParameter(int /*index*/, float /*value*/) {}

    // ---- plugin editor (only hosted plugins have one)
    virtual bool hasEditor() const { return false; }

    void setBypassed(bool b) { bypassed.store(b); }
    bool isBypassed() const { return bypassed.load(); }

    OutputMeter outputMeter; // written by whoever runs process() (engine node or rack chain), read by the chain meters

protected:
    std::atomic<bool> bypassed{false};
};

} // namespace sonora::devices

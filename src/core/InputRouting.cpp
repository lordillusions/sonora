#include "core/InputRouting.h"
#include <algorithm>
#include <charconv>

namespace sonora::model
{
namespace
{
int integer(const std::string& text, int fallback = -1)
{
    int value = fallback;
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size() ? value : fallback;
}
}
InputSelection parseInput(const Track& track)
{
    const auto& target = track.inputRouting;
    if (target.empty()) return {track.kind == TrackKind::Midi ? InputKind::Midi : InputKind::AudioStereo, 0, {}};
    if ((track.kind == TrackKind::Midi && target.starts_with("AudioIn/"))
        || (track.kind == TrackKind::Audio && target.starts_with("MidiIn/"))) return {};
    if (target == "AudioIn/None" || target == "MidiIn/None") return {InputKind::None, 0, {}};
    if (target == "AudioIn/Main" || target == "AudioIn/Master") return {InputKind::Resampling, 0, {}};
    const std::string audio = "AudioIn/External/";
    if (target.starts_with(audio) && target.size() > audio.size() + 1)
    {
        const char type = target[audio.size()];
        const int index = integer(target.substr(audio.size() + 1));
        if (index < 0 || index > 255 || (type != 'M' && type != 'S')) return {};
        int channel = type == 'M' ? index : index * 2;
        // Display names can identify the physical pair even with device-specific stereo indexing.
        const auto slash = track.inputLowerDisplay.find('/');
        if (type == 'S' && slash != std::string::npos)
        {
            const int left = integer(track.inputLowerDisplay.substr(0, slash)), right = integer(track.inputLowerDisplay.substr(slash + 1));
            if (left > 0 && right == left + 1) channel = left - 1;
        }
        if (channel > (type == 'M' ? 63 : 62)) return {};
        return {type == 'M' ? InputKind::AudioMono : InputKind::AudioStereo, channel, {}};
    }
    const std::string midi = "MidiIn/External.";
    if (target.starts_with(midi))
    {
        const auto slash = target.find_last_of('/');
        if (slash == std::string::npos || slash <= midi.size()) return {};
        const int channel = integer(target.substr(slash + 1), -2);
        if (channel < -1 || channel > 15) return {};
        const auto device = target.substr(midi.size(), slash - midi.size());
        return {InputKind::Midi, channel + 1, device == "All" ? std::string() : device};
    }
    return {};
}
uint32_t midiPortToken(const std::string& identifier)
{
    if (identifier.empty()) return 0;
    uint32_t hash = 2166136261u;
    for (const char byte : identifier) hash = (hash ^ (uint8_t) byte) * 16777619u;
    return hash == 0 ? 1 : hash;
}
uint64_t packInput(const InputSelection& input)
{
    return (uint64_t) input.kind | ((uint64_t) (uint32_t) std::clamp(input.channel, 0, 65535) << 8)
           | ((uint64_t) midiPortToken(input.device) << 32);
}
void setInputSelection(Track& track, const InputSelection& input)
{
    track.inputRoutingEdited = true;
    const int channel = std::clamp(input.channel, 0, 255);
    switch (input.kind)
    {
        case InputKind::AudioMono:
        case InputKind::AudioStereo:
            track.inputRouting = "AudioIn/External/" + std::string(input.kind == InputKind::AudioMono ? "M" : "S")
                                 + std::to_string(input.kind == InputKind::AudioMono ? channel : channel / 2);
            track.inputUpperDisplay = "Ext. In";
            track.inputLowerDisplay = std::to_string(channel + 1);
            if (input.kind == InputKind::AudioStereo) track.inputLowerDisplay += "/" + std::to_string(channel + 2);
            break;
        case InputKind::Midi:
            track.inputRouting = "MidiIn/External." + (input.device.empty() ? "All" : input.device) + "/" + std::to_string(input.channel - 1);
            track.inputUpperDisplay = input.device.empty() ? "Ext: All Ins" : input.device;
            track.inputLowerDisplay = input.channel == 0 ? "" : std::to_string(input.channel);
            break;
        case InputKind::None:
            track.inputRouting = track.kind == TrackKind::Midi ? "MidiIn/None" : "AudioIn/None";
            track.inputUpperDisplay = "No Input"; track.inputLowerDisplay.clear(); break;
        case InputKind::Resampling:
            track.inputRouting = "AudioIn/Main"; track.inputUpperDisplay = "Resampling"; track.inputLowerDisplay.clear(); break;
        case InputKind::Unset:
        case InputKind::Unsupported: break;
    }
}
} // namespace sonora::model

#include "core/Model.h"
#include <algorithm>
#include <atomic>
#include <unordered_set>

namespace sonora::model
{

double segmentShape(const AutomationPoint& start, double fraction)
{
    fraction = std::clamp(fraction, 0.0, 1.0);
    if (! start.curved()) return fraction;
    const double x1 = std::clamp(start.x1, 0.0, 1.0), x2 = std::clamp(start.x2, 0.0, 1.0);
    auto bezier = [](double a, double b, double u) { const double v = 1.0 - u; return 3 * v * v * u * a + 3 * v * u * u * b + u * u * u; };
    // x(u) is monotonic for control x in [0, 1]: bisect for the parameter at this time.
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 40; ++i)
    {
        const double mid = 0.5 * (lo + hi);
        (bezier(x1, x2, mid) < fraction ? lo : hi) = mid;
    }
    return bezier(start.y1, start.y2, 0.5 * (lo + hi));
}
namespace
{
std::atomic<int> nextClipUid{1000000};
thread_local ClipUidScope* activeClipUidScope = nullptr;

void reserveClipUid(int uid)
{
    int current = nextClipUid.load(std::memory_order_relaxed);
    while (current <= uid && ! nextClipUid.compare_exchange_weak(current, uid + 1, std::memory_order_relaxed)) {}
}
}

int allocateClipUid()
{
    if (auto* scope = activeClipUidScope)
    {
        if (scope->mode_ == ClipUidScope::Mode::Replay)
        {
            if (scope->next_ < scope->uids_.size())
            {
                const int uid = scope->uids_[scope->next_++];
                reserveClipUid(uid); // later local allocations never collide with a replayed identity
                return uid;
            }
            scope->overrun_ = true;
        }
        else
        {
            const int uid = nextClipUid.fetch_add(1, std::memory_order_relaxed);
            scope->uids_.push_back(uid);
            return uid;
        }
    }
    return nextClipUid.fetch_add(1, std::memory_order_relaxed);
}

void reserveClipUidsFrom(int base)
{
    if (base > 0) reserveClipUid(base - 1);
}

ClipUidScope::ClipUidScope(Mode mode, std::vector<int> replay)
    : mode_(mode), uids_(mode == Mode::Replay ? std::move(replay) : std::vector<int>{}), previous_(activeClipUidScope)
{
    activeClipUidScope = this;
}

ClipUidScope::~ClipUidScope()
{
    activeClipUidScope = previous_;
}

void assignNoteKeys(std::vector<MidiNote>& notes)
{
    uint32_t highest = 0;
    for (const auto& note : notes) highest = std::max(highest, note.key);
    std::unordered_set<uint32_t> seen;
    seen.reserve(notes.size());
    for (auto& note : notes)
        if (note.key == 0 || ! seen.insert(note.key).second)
        {
            note.key = ++highest;
            seen.insert(note.key);
        }
}

std::string displayTrackName(const Song& song, int index)
{
    if (index < 0) return {};
    const auto n = (int) song.tracks.size(), r = (int) song.returns.size();
    const Track* track = index < n ? &song.tracks[(size_t) index]
                       : index < n + r ? &song.returns[(size_t) (index - n)] : index == n + r ? &song.main : nullptr;
    if (! track) return {};
    if (track->kind == TrackKind::Main) return "Main";
    if (track->kind == TrackKind::Return) return track->name;
    const auto number = std::to_string(index + 1);
    if (track->name.starts_with('#'))
    {
        const auto end = track->name.find_first_not_of('#');
        const auto count = end == std::string::npos ? track->name.size() : end;
        return std::string(count > number.size() ? count - number.size() : 0, '0') + number + track->name.substr(count);
    }
    if (! track->nameIsAutomatic) return track->name;
    auto stem = track->name;
    const auto separator = stem.find_first_not_of("0123456789");
    if (separator != std::string::npos && separator > 0 && (stem[separator] == '-' || stem[separator] == ' '))
        stem.erase(0, separator + 1);
    if (stem.empty()) stem = track->kind == TrackKind::Midi ? "MIDI" : track->kind == TrackKind::Group ? "Group" : "Audio";
    return number + " " + stem;
}

const char* toString(TrackKind k)
{
    switch (k)
    {
        case TrackKind::Audio: return "Audio";
        case TrackKind::Midi: return "Midi";
        case TrackKind::Group: return "Group";
        case TrackKind::Return: return "Return";
        case TrackKind::Main: return "Main";
    }
    return "?";
}

const char* toString(DeviceKind k)
{
    switch (k)
    {
        case DeviceKind::Native: return "Native";
        case DeviceKind::PluginVst2: return "VST2";
        case DeviceKind::PluginVst3: return "VST3";
        case DeviceKind::PluginAu: return "AU";
        case DeviceKind::Rack: return "Rack";
        case DeviceKind::MaxForLive: return "M4L";
    }
    return "?";
}

const char* toString(WarpMode m)
{
    switch (m)
    {
        case WarpMode::Beats: return "Beats";
        case WarpMode::Tones: return "Tones";
        case WarpMode::Texture: return "Texture";
        case WarpMode::RePitch: return "Re-Pitch";
        case WarpMode::Complex: return "Complex";
        case WarpMode::ComplexPro: return "Complex Pro";
    }
    return "?";
}

} // namespace sonora::model

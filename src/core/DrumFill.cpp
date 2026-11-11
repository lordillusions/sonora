#include "core/DrumFill.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

namespace sonora::model
{
namespace
{
using R = DrumRole;

struct Rng
{
    uint32_t state;
    explicit Rng(uint32_t seed) : state(seed * 2654435761u + 0x6d2b79f5u) {}
    uint32_t next()
    {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    }
    double unit() { return (double) (next() >> 8) / 16777216.0; }
    bool chance(double p) { return unit() < p; }
    int range(int n) { return n <= 1 ? 0 : (int) (next() % (uint32_t) n); }
};

std::string lower(std::string s)
{
    for (auto& c : s) c = (char) std::tolower((unsigned char) c);
    return s;
}

bool word(const std::string& s, const char* w)
{
    const std::string needle(w);
    for (size_t at = s.find(needle); at != std::string::npos; at = s.find(needle, at + 1))
    {
        const bool before = at == 0 || ! std::isalnum((unsigned char) s[at - 1]);
        const bool after = at + needle.size() >= s.size() || ! std::isalnum((unsigned char) s[at + needle.size()]);
        if (before && after) return true;
    }
    return false;
}

bool has(const std::string& s, const char* w) { return s.find(w) != std::string::npos; }

DrumRole gmRole(int pitch, bool& known)
{
    known = true;
    switch (pitch)
    {
        case 35: case 36: return R::Kick;
        case 37: case 38: case 40: return R::Snare;
        case 39: return R::Clap;
        case 42: case 44: return R::ClosedHat;
        case 46: return R::OpenHat;
        case 41: case 43: return R::TomLow;
        case 45: case 47: return R::TomMid;
        case 48: case 50: return R::TomHigh;
        case 49: case 52: case 55: case 57: return R::Crash;
        case 51: case 53: case 59: return R::Ride;
        default: known = false; return R::Kick;
    }
}

float median(std::vector<float> v, float fallback)
{
    if (v.empty()) return fallback;
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
}
} // namespace

int DrumKit::toms() const
{
    return (has(R::TomHigh) ? 1 : 0) + (has(R::TomMid) ? 1 : 0) + (has(R::TomLow) ? 1 : 0);
}

DrumRole roleForName(const std::string& padName, bool& known)
{
    const auto s = lower(padName);
    known = true;
    if (has(s, "tom"))
    {
        if (has(s, "floor") || word(s, "low") || word(s, "lo")) return R::TomLow;
        if (word(s, "hi") || has(s, "high")) return R::TomHigh;
        return R::TomMid;
    }
    if (has(s, "kick") || word(s, "bd") || has(s, "bass drum") || word(s, "kik")) return R::Kick;
    if (has(s, "clap") || word(s, "cp")) return R::Clap;
    if (has(s, "snare") || word(s, "sd") || has(s, "rim")) return R::Snare;
    if (has(s, "crash") || has(s, "cymbal") || has(s, "splash") || has(s, "china")) return R::Crash;
    if (has(s, "ride")) return R::Ride;
    if (has(s, "hat") || word(s, "hh") || word(s, "oh") || word(s, "ch"))
        return has(s, "open") || word(s, "oh") ? R::OpenHat : R::ClosedHat;
    known = false;
    return R::Kick;
}

DrumKit drumKitFor(const Track& track, const std::vector<MidiNote>& notes)
{
    DrumKit kit;
    bool named = false;
    std::vector<int> unnamedToms; // "Tom" pads: low / mid / high by pitch
    for (const auto& device : track.devices)
    {
        if (device.type != "DrumGroupDevice") continue;
        for (const auto& chain : device.chains)
        {
            if (chain.receivingNote < 0) continue;
            std::string name = chain.name;
            if (name.empty() && ! chain.devices.empty()) name = chain.devices.front().displayName;
            bool known = false;
            const auto role = roleForName(name, known);
            if (! known) continue;
            named = true;
            if (role == R::TomMid && ! has(lower(name), "mid")) { unnamedToms.push_back(chain.receivingNote); continue; }
            if (! kit.has(role)) kit.pitch[(size_t) role] = chain.receivingNote;
        }
        break; // the first Drum Rack is the kit
    }
    std::sort(unnamedToms.begin(), unnamedToms.end());
    for (int pitch : unnamedToms) // fill low, mid, high in pitch order
        for (const auto role : {R::TomLow, R::TomMid, R::TomHigh})
            if (! kit.has(role)) { kit.pitch[(size_t) role] = pitch; break; }
    // Pitches the clip plays that no pad name explained: General MIDI.
    for (const auto& note : notes)
    {
        bool known = false;
        const auto role = gmRole(note.pitch, known);
        if (known && ! kit.has(role) && std::none_of(kit.pitch.begin(), kit.pitch.end(), [&](int p) { return p == note.pitch; }))
            kit.pitch[(size_t) role] = note.pitch;
    }
    if (! named) // no Drum Rack names to trust: General MIDI defaults for the rest
    {
        constexpr int defaults[(size_t) R::Count] = {36, 38, 39, 42, 46, 50, 47, 43, 49, 51};
        for (size_t r = 0; r < kit.pitch.size(); ++r)
            if (kit.pitch[r] < 0) kit.pitch[r] = defaults[r];
    }
    return kit;
}

Groove analyseGroove(const std::vector<MidiNote>& notes, const DrumKit& kit, double from, double to)
{
    Groove g;
    std::vector<float> kicks, snares, hats;
    int triplets = 0, offGrid = 0, swung = 0, counted = 0;
    double swingSum = 0;
    for (const auto& n : notes)
    {
        if (n.start < from - 1e-9 || n.start >= to - 1e-9 || ! n.enabled) continue;
        ++counted;
        if (n.pitch == kit[R::Kick]) kicks.push_back(n.velocity);
        else if (n.pitch == kit[R::Snare] || n.pitch == kit[R::Clap]) snares.push_back(n.velocity);
        else if (n.pitch == kit[R::ClosedHat] || n.pitch == kit[R::OpenHat]) hats.push_back(n.velocity);
        const double sixteenth = n.start * 4.0, nearest = std::round(sixteenth);
        const double third = n.start * 3.0;
        if (std::abs(sixteenth - nearest) > 0.05)
        {
            ++offGrid;
            if (std::abs(third - std::round(third)) < 0.05) ++triplets;
        }
        const double delta = n.start - nearest / 4.0;
        if (((long long) nearest) % 2 != 0 && delta > 0.005 && delta < 0.1) { swingSum += delta; ++swung; }
    }
    g.triplet = offGrid >= 3 && triplets * 2 >= offGrid;
    g.swing = swung >= 2 ? swingSum / swung : 0.0;
    g.kickVelocity = median(kicks, 105);
    g.snareVelocity = median(snares, 100);
    g.hatVelocity = median(hats, 80);
    g.notesPerBeat = to > from ? counted / (to - from) : 2.0;
    return g;
}

const char* fillStyleName(FillStyle s)
{
    switch (s)
    {
        case FillStyle::SnareRoll: return "snare roll";
        case FillStyle::TomRun: return "tom run";
        case FillStyle::KickSnare: return "kick and snare";
        case FillStyle::HatBuild: return "hat build";
        case FillStyle::Triplet: return "triplets";
        case FillStyle::Break: return "break";
        case FillStyle::Count: break;
    }
    return "fill";
}

FillResult generateFill(const std::vector<MidiNote>& notes, double regionStart, double regionEnd, const DrumKit& kit,
                        const Groove& groove, double tempo, uint32_t seed, int style)
{
    FillResult result;
    Rng rng(seed);
    const double length = regionEnd - regionStart;
    for (const auto& n : notes)
        if (n.start < regionStart - 1e-9 || n.start >= regionEnd - 1e-9) result.notes.push_back(n);
    if (length < 0.24) return result;

    // ---- the style
    if (style >= 0 && style < (int) FillStyle::Count) result.style = (FillStyle) style;
    else
    {
        const int toms = kit.toms();
        const bool hats = kit.has(R::ClosedHat);
        double weights[(size_t) FillStyle::Count] = {
            3.0 + (tempo >= 140 ? 2.0 : 0.0),                                   // snare roll
            toms >= 2 ? 3.0 + (tempo < 120 ? 1.0 : 0.0) : toms == 1 ? 1.0 : 0.0, // tom run
            2.0,                                                                 // kick and snare
            hats ? 2.0 + (tempo >= 118 && tempo <= 135 ? 2.0 : 0.0) : 0.0,       // hat build
            groove.triplet ? 4.0 : 0.7,                                          // triplets
            1.0 + (tempo < 100 ? 1.0 : 0.0)};                                    // break
        if (length < 1.0) weights[(size_t) FillStyle::TomRun] = weights[(size_t) FillStyle::HatBuild] = weights[(size_t) FillStyle::Break] = 0;
        double total = 0;
        for (double w : weights) total += w;
        double pick = rng.unit() * total;
        for (int s = 0; s < (int) FillStyle::Count; ++s)
        {
            pick -= weights[s];
            if (pick <= 0) { result.style = (FillStyle) s; break; }
        }
    }

    // ---- writing notes: missing pieces fall back to ones the kit has, the clip's swing applies to the off 16ths
    const auto pitchFor = [&](R role) {
        const R chain[][4] = {{R::Kick, R::Kick, R::Kick, R::Kick},
                              {R::Snare, R::Clap, R::Snare, R::Snare},
                              {R::Clap, R::Snare, R::Clap, R::Clap},
                              {R::ClosedHat, R::OpenHat, R::Ride, R::ClosedHat},
                              {R::OpenHat, R::ClosedHat, R::Ride, R::OpenHat},
                              {R::TomHigh, R::TomMid, R::TomLow, R::Snare},
                              {R::TomMid, R::TomHigh, R::TomLow, R::Snare},
                              {R::TomLow, R::TomMid, R::TomHigh, R::Snare},
                              {R::Crash, R::Ride, R::OpenHat, R::Crash},
                              {R::Ride, R::Crash, R::ClosedHat, R::Ride}};
        for (const R r : chain[(size_t) role])
            if (kit.has(r)) return kit[r];
        return -1;
    };
    std::vector<MidiNote> fill;
    const auto add = [&](double at, R role, float velocity, double duration = 0.125) {
        const int pitch = pitchFor(role);
        if (pitch < 0 || at < regionStart - 1e-9 || at >= regionEnd - 1e-9) return;
        double position = at;
        if (! groove.triplet && groove.swing > 0)
        {
            const double k = std::round((at - regionStart) * 4.0);
            if (std::abs((at - regionStart) * 4.0 - k) < 1e-6 && ((long long) std::round(at * 4.0)) % 2 != 0) position += groove.swing;
        }
        MidiNote n;
        n.start = position;
        n.duration = duration;
        n.pitch = pitch;
        n.velocity = std::clamp(velocity + (float) (rng.unit() * 10.0 - 5.0), 1.0f, 127.0f);
        fill.push_back(n);
    };
    const float sv = groove.snareVelocity, kv = groove.kickVelocity, hv = groove.hatVelocity;
    const int steps = (int) std::floor(length * 4.0 + 1e-6); // 16ths in the region
    const double last = regionStart + (steps - 1) * 0.25;    // the last 16th
    const auto ramp = [&](int step, float from, float to) { return from + (to - from) * (float) step / (float) std::max(1, steps - 1); };
    const auto flam = [&](double at, float velocity) {
        if (rng.chance(0.5)) add(at - 1.0 / 48.0, R::Snare, velocity * 0.35f, 0.05);
    };

    switch (result.style)
    {
        case FillStyle::SnareRoll:
        {
            const bool thirtySeconds = tempo < 150 && rng.chance(0.5);
            for (int s = 0; s < steps; ++s)
            {
                const double at = regionStart + s * 0.25;
                const bool firstHalf = steps >= 16 && s < steps / 2;
                if (firstHalf && s % 2 == 1) continue; // 8ths, then 16ths
                if (! firstHalf && s > 0 && s < steps - 2 && rng.chance(0.12)) continue; // a gap here and there
                add(at, R::Snare, ramp(s, sv * 0.5f, sv * 1.05f));
                if (thirtySeconds && s >= steps - 4) add(at + 0.125, R::Snare, ramp(s, sv * 0.6f, sv), 0.06);
            }
            add(regionStart, R::Kick, kv);
            flam(last, sv);
            break;
        }
        case FillStyle::TomRun:
        {
            const R order[3] = {R::TomHigh, R::TomMid, R::TomLow};
            const int group = std::max(1, steps / 3);
            for (int s = 0; s < steps; ++s)
            {
                const double at = regionStart + s * 0.25;
                const R tom = order[std::min(2, s / group)];
                if (s % 4 == 0 && s > 0 && rng.chance(0.4)) add(at, R::Snare, sv * 0.9f); // a snare where a group turns
                else add(at, tom, ramp(s, sv * 0.75f, sv * 1.05f), 0.25);
                if (s % 4 == 0) add(at, R::Kick, kv * 0.85f);
            }
            add(last, R::Kick, kv);
            break;
        }
        case FillStyle::KickSnare:
        {
            for (int s = 0; s < steps; ++s)
            {
                const double at = regionStart + s * 0.25;
                if (s == 0) { add(at, R::Kick, kv); continue; }
                if (s == steps - 1) { add(at, R::Snare, sv * 1.05f); add(at, R::Kick, kv); continue; }
                const double r = rng.unit();
                if (r < 0.42) add(at, R::Snare, s % 2 == 0 ? sv : sv * 0.8f);
                else if (r < 0.70) add(at, R::Kick, kv * 0.9f);
                else if (r < 0.85) add(at, R::Snare, sv * 0.35f); // a ghost
            }
            flam(last, sv);
            break;
        }
        case FillStyle::HatBuild:
        {
            for (int s = 0; s < steps; ++s)
            {
                const double at = regionStart + s * 0.25;
                add(at, R::ClosedHat, ramp(s, hv * 0.6f, std::min(127.0f, hv * 1.3f)), 0.1);
                if (s >= steps - 4) add(at + 0.125, R::ClosedHat, ramp(s, hv * 0.7f, hv * 1.2f), 0.06); // 32nds at the end
                if (s % 4 == 2 && s >= steps / 2) add(at, R::OpenHat, hv, 0.2);
                if (s % 4 == 0 && tempo >= 115) add(at, R::Kick, kv * 0.95f);
            }
            if (steps >= 2) add(last - 0.25, R::Snare, sv);
            add(last, R::Snare, sv * 1.1f);
            break;
        }
        case FillStyle::Triplet:
        {
            const int count = (int) std::floor(length * 3.0 + 1e-6);
            const R order[4] = {R::Snare, R::TomHigh, R::TomMid, R::TomLow};
            for (int t = 0; t < count; ++t)
            {
                const double at = regionStart + t / 3.0;
                const R piece = order[(t / 3) % 4];
                add(at, t % 3 == 0 ? piece : (rng.chance(0.6) ? piece : R::Snare),
                    sv * (0.6f + 0.45f * (float) t / (float) std::max(1, count - 1)), 1.0 / 3.0);
                if (t % 3 == 0) add(at, R::Kick, kv * 0.85f);
            }
            break;
        }
        case FillStyle::Break:
        case FillStyle::Count:
        {
            add(regionStart, R::Kick, kv);
            if (length >= 2.0 && rng.chance(0.5)) add(regionStart, R::Crash, sv * 0.9f, 1.0);
            const double hits = steps >= 4 ? 2 : 1;
            for (int h = 0; h < (int) hits; ++h)
            {
                const double at = last - (hits - 1 - h) * 0.25;
                add(at, R::Snare, sv * 1.05f);
                add(at, R::Kick, kv);
                flam(at, sv);
            }
            break;
        }
    }
    std::sort(fill.begin(), fill.end(), [](const MidiNote& a, const MidiNote& b) { return a.start < b.start - 1e-9 || (std::abs(a.start - b.start) <= 1e-9 && a.pitch < b.pitch); });
    // One note per pitch per moment.
    fill.erase(std::unique(fill.begin(), fill.end(), [](const MidiNote& a, const MidiNote& b) {
        return a.pitch == b.pitch && std::abs(a.start - b.start) < 1e-6;
    }), fill.end());
    result.notes.insert(result.notes.end(), fill.begin(), fill.end());
    std::sort(result.notes.begin(), result.notes.end(), [](const MidiNote& a, const MidiNote& b) { return a.start < b.start; });
    return result;
}

EditCommand drumFill(const Song& song, const ClipLocation& location, double beats, uint32_t seed, std::string& message)
{
    const auto* clip = findClip(song, location);
    if (clip == nullptr || clip->kind != Clip::Kind::Midi || location.track < 0 || (size_t) location.track >= song.tracks.size())
    {
        message = "Drum fills go into MIDI clips";
        return {};
    }
    const double end = clip->loop.loopEnd, begin = clip->loop.loopStart;
    const double start = std::max(begin, end - beats);
    if (end - start < 0.24)
    {
        message = "The clip is too short for a fill";
        return {};
    }
    const auto& track = song.tracks[(size_t) location.track];
    const auto kit = drumKitFor(track, clip->notes);
    const auto groove = analyseGroove(clip->notes, kit, begin, start > begin + 0.5 ? start : end);
    auto result = generateFill(clip->notes, start, end, kit, groove, song.tempo, seed);
    message = std::string("Drum fill: ") + fillStyleName(result.style) + " (ask again for another)";
    return setNotes(song, location, std::move(result.notes), std::string("Drum Fill (") + fillStyleName(result.style) + ")");
}
} // namespace sonora::model

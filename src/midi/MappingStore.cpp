#include "midi/MappingStore.h"
#include "core/AppDataFolder.h"
#include <algorithm>

namespace sonora::midi
{

DeviceOrder deviceOrder(const model::Song& song)
{
    DeviceOrder result;
    auto track = [&](const model::Track& t) {
        result.emplace_back();
        for (const auto& d : t.devices) result.back().emplace_back(d.id, d.type);
    };
    for (const auto& t : song.tracks) track(t);
    for (const auto& t : song.returns) track(t);
    track(song.main);
    return result;
}

void rebindTrackMappings(const std::vector<model::TrackId>& before, const std::vector<model::TrackId>& after,
                         const std::vector<model::TrackId>& beforeReturns, const std::vector<model::TrackId>& afterReturns,
                         std::vector<Mapping>& mappings)
{
    for (auto& mapping : mappings)
    {
        auto& t = mapping.target;
        if (t.kind == ControlTarget::Kind::TransportPlay || t.kind == ControlTarget::Kind::TransportStop
            || t.kind == ControlTarget::Kind::TransportToggle || t.kind == ControlTarget::Kind::Tempo) continue;
        if (! t.trackBound && t.track >= 0 && (size_t) t.track < before.size()) { t.boundTrackId = before[(size_t) t.track]; t.trackBound = true; }
        if (t.trackBound)
        {
            const auto found = std::find(after.begin(), after.end(), t.boundTrackId);
            t.track = found == after.end() ? -1 : (int) (found - after.begin());
        }
        if (t.kind != ControlTarget::Kind::TrackSend) continue;
        if (! t.returnBound && t.index >= 0 && (size_t) t.index < beforeReturns.size()) { t.boundReturnId = beforeReturns[(size_t) t.index]; t.returnBound = true; }
        if (t.returnBound)
        {
            const auto found = std::find(afterReturns.begin(), afterReturns.end(), t.boundReturnId);
            t.index = found == afterReturns.end() ? -1 : (int) (found - afterReturns.begin());
        }
    }
}

void rebindDeviceMappings(const DeviceOrder& before, const DeviceOrder& after, std::vector<Mapping>& mappings)
{
    for (auto& mapping : mappings)
    {
        auto& target = mapping.target;
        if (target.kind != ControlTarget::Kind::DeviceParameter && target.kind != ControlTarget::Kind::DeviceOn) continue;
        if (target.track < 0 || (size_t) target.track >= before.size() || (size_t) target.track >= after.size()) continue;
        const auto& old = before[(size_t) target.track]; const auto& now = after[(size_t) target.track];
        if (target.device < 0 || (size_t) target.device >= old.size()) continue;
        const auto& identity = old[(size_t) target.device];
        const auto found = std::find(now.begin(), now.end(), identity);
        if (found != now.end()) target.device = (int) (found - now.begin());
        // Structural native edits deliberately rebuild one device under a new ID, in the same position.
        else if (old.size() != now.size() || now[(size_t) target.device].second != identity.second) target.device = -1;
    }
}

namespace
{
const char* kindNames[] = {"TrackVolume", "TrackPan",  "TrackSend",       "TrackActivator",  "TrackSolo",
                           "TrackArm",    "DeviceParameter", "DeviceOn", "TransportPlay",   "TransportStop",
                           "TransportToggle", "Tempo", "SessionClip"};
constexpr int kNumKinds = (int) (sizeof(kindNames) / sizeof(kindNames[0]));

int kindFromName(const juce::String& n)
{
    for (int i = 0; i < kNumKinds; ++i)
        if (n == kindNames[i])
            return i;
    return -1;
}
} // namespace

juce::var mappingsToVar(const std::vector<Mapping>& mappings)
{
    juce::Array<juce::var> arr;
    for (const auto& m : mappings)
    {
        if ((int) m.target.kind < 0 || (int) m.target.kind >= kNumKinds) continue;
        auto* t = new juce::DynamicObject();
        t->setProperty("kind", kindNames[(int) m.target.kind]);
        t->setProperty("track", m.target.track);
        t->setProperty("device", m.target.device);
        t->setProperty("index", m.target.index);
        t->setProperty("param", juce::String::fromUTF8(m.target.param.c_str()));
        if (m.target.kind == ControlTarget::Kind::SessionClip)
        {
            t->setProperty("clipUid", m.target.clipUid);
            t->setProperty("trackId", m.target.trackId);
        }
        auto* o = new juce::DynamicObject();
        o->setProperty("channel", m.channel);
        o->setProperty("isNote", m.isNote);
        o->setProperty("number", m.number);
        o->setProperty("mode", (int) m.mode);
        o->setProperty("target", juce::var(t));
        if (m.keyCode > 0) o->setProperty("keyCode", m.keyCode);
        arr.add(juce::var(o));
    }
    return juce::var(arr);
}

std::vector<Mapping> mappingsFromVar(const juce::var& v)
{
    std::vector<Mapping> out;
    const auto* arr = v.getArray();
    if (arr == nullptr)
        return out;
    for (const auto& e : *arr)
    {
        const int kind = kindFromName(e.getProperty("target", {}).getProperty("kind", {}).toString());
        if (kind < 0)
            continue; // unknown target from a newer version: ignore, never fail
        Mapping m;
        m.channel = juce::jlimit(1, 16, (int) e.getProperty("channel", 1));
        m.isNote = (bool) e.getProperty("isNote", false);
        m.number = juce::jlimit(0, 127, (int) e.getProperty("number", 0));
        m.mode = static_cast<MapMode>(juce::jlimit(0, 3, (int) e.getProperty("mode", 0)));
        const auto t = e.getProperty("target", {});
        m.target.kind = static_cast<ControlTarget::Kind>(kind);
        m.target.track = (int) t.getProperty("track", 0);
        m.target.device = (int) t.getProperty("device", 0);
        m.target.index = (int) t.getProperty("index", 0);
        m.target.param = t.getProperty("param", {}).toString().toStdString();
        m.target.clipUid = (int) t.getProperty("clipUid", -1);
        m.target.trackId = (int) t.getProperty("trackId", -1);
        m.keyCode = (int) e.getProperty("keyCode", 0);
        if (m.keyCode != 0)
        {
            m.keyCode = normaliseLaunchKey(m.keyCode);
            if (m.keyCode == 0 || m.target.kind != ControlTarget::Kind::SessionClip) continue;
        }
        out.push_back(std::move(m));
    }
    return out;
}

std::optional<ControlTarget> sessionClipTarget(const model::Song& song, int track, int scene)
{
    if (track < 0 || (size_t) track >= song.tracks.size() || scene < 0) return std::nullopt;
    const auto& t = song.tracks[(size_t) track];
    if ((t.kind != model::TrackKind::Audio && t.kind != model::TrackKind::Midi)
        || (size_t) scene >= t.slots.size() || ! t.slots[(size_t) scene].clip) return std::nullopt;
    ControlTarget target{ControlTarget::Kind::SessionClip, track, 0, scene, {}};
    target.trackId = t.id;
    target.clipUid = t.slots[(size_t) scene].clip->uid;
    return target;
}

std::optional<model::ClipLocation> sessionClipLocation(const model::Song& song, const ControlTarget& target)
{
    if (target.kind != ControlTarget::Kind::SessionClip || target.clipUid < 0) return std::nullopt;
    for (size_t track = 0; track < song.tracks.size(); ++track)
    {
        const auto& t = song.tracks[track];
        if (t.kind != model::TrackKind::Audio && t.kind != model::TrackKind::Midi) continue;
        for (size_t scene = 0; scene < t.slots.size(); ++scene)
            if (t.slots[scene].clip && t.slots[scene].clip->uid == target.clipUid)
                return model::ClipLocation::session((int) track, target.clipUid, (int) scene);
    }
    return std::nullopt;
}

std::vector<Mapping> mappingsForSave(const model::Song& song, std::vector<Mapping> mappings)
{
    for (auto& m : mappings)
        if (m.target.kind == ControlTarget::Kind::SessionClip)
        {
            m.target.index = -1; m.target.trackId = -1;
            if (const auto found = song.savedClips.find(m.target.clipUid); found != song.savedClips.end())
            {
                m.target.index = found->second.sessionSlot;
                m.target.trackId = found->second.trackId;
            }
        }
    return mappings;
}

void bindMappingsToSong(const model::Song& song, std::vector<Mapping>& mappings)
{
    for (auto& m : mappings)
        if (m.target.kind == ControlTarget::Kind::SessionClip)
        {
            m.target.clipUid = -1; // never trust a UID from another load
            for (size_t track = 0; track < song.tracks.size(); ++track)
                if (song.tracks[track].id == m.target.trackId)
                    if (auto target = sessionClipTarget(song, (int) track, m.target.index)) m.target = *target;
        }
}

bool saveMappings(const juce::File& file, const std::vector<Mapping>& mappings)
{
    auto* root = new juce::DynamicObject();
    root->setProperty("midiMappings", mappingsToVar(mappings));
    file.getParentDirectory().createDirectory();
    return file.replaceWithText(juce::JSON::toString(juce::var(root)));
}

bool loadMappings(const juce::File& file, std::vector<Mapping>& out)
{
    out.clear();
    if (! file.existsAsFile())
        return false;
    const auto parsed = juce::JSON::parse(file);
    if (! parsed.getProperty("midiMappings", {}).isArray())
        return false;
    out = mappingsFromVar(parsed.getProperty("midiMappings", {}));
    return true;
}

juce::File sidecarFor(const juce::File& setFile)
{
    return setFile.getSiblingFile(setFile.getFileNameWithoutExtension() + ".sonora.json");
}

juce::File globalMappingsFile()
{
    return core::appDataDirectory().getChildFile("Mappings.json");
}

std::vector<Mapping> importLiveMappings(const model::Song& song)
{
    std::vector<Mapping> out;
    auto add = [&](const model::MidiBinding& b, ControlTarget t) {
        if (! b.mapped())
            return;
        Mapping m;
        m.channel = std::clamp(b.channel, 0, 15) + 1; // the file stores 0-based channels
        m.isNote = b.isNote;
        m.number = std::clamp(b.number, 0, 127);
        m.mode = b.isNote ? MapMode::Absolute : mapModeFromLive(b.mapMode);
        m.target = std::move(t);
        out.push_back(std::move(m));
    };

    int ci = 0;
    auto doTrack = [&](const model::Track& tr) {
        ControlTarget base;
        base.track = ci;
        auto with = [&](ControlTarget::Kind k, int index = 0) {
            auto t = base;
            t.kind = k;
            t.index = index;
            return t;
        };
        add(tr.mixer.volumeMidi, with(ControlTarget::Kind::TrackVolume));
        add(tr.mixer.panMidi, with(ControlTarget::Kind::TrackPan));
        add(tr.mixer.speakerMidi, with(ControlTarget::Kind::TrackActivator));
        for (size_t i = 0; i < tr.mixer.sendMidi.size(); ++i)
            add(tr.mixer.sendMidi[i], with(ControlTarget::Kind::TrackSend, (int) i));
        for (size_t d = 0; d < tr.devices.size(); ++d)
            for (const auto& p : tr.devices[d].parameters)
            {
                auto t = base;
                t.device = (int) d;
                if (p.path == "On")
                    t.kind = ControlTarget::Kind::DeviceOn;
                else
                    t.kind = ControlTarget::Kind::DeviceParameter, t.param = p.path;
                add(p.midi, std::move(t));
            }
        ++ci;
    };
    for (const auto& t : song.tracks)
        doTrack(t);
    for (const auto& t : song.returns)
        doTrack(t);
    doTrack(song.main);
    return out;
}

} // namespace sonora::midi

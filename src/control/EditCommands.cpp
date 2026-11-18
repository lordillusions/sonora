#include "control/ControlInternal.h"
#include "core/Scales.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"
#include <algorithm>
#include <map>
#include <set>

// AGT-7e: the structure and song edits of `edits.apply` (time signature, punch, scale, quantisation, track CRUD, clip
// properties/splits/regions, take lanes). Each builder validates one command object strictly and returns the undoable
// model::EditCommand built against the staged song. They address tracks by ID, resolved against the staged song at that
// point of the batch, so an earlier command that inserted or removed tracks never shifts a later one.
namespace sonora::control::detail
{
namespace
{
using model::OpError;
constexpr int maxId = 1000000000;
constexpr double maxBeat = 1000000.0;

[[noreturn]] void invalid(const std::string& message) { throw OpError("invalid_request", message); }
[[noreturn]] void replaced(const std::string& message) { throw OpError("target_replaced", message); }

std::vector<int> idList(const juce::var& item, const char* name, int minCount, int maxCount)
{
    const auto* list = item[name].getArray();
    if (! list || list->size() < minCount || list->size() > maxCount)
        throw std::runtime_error(std::string("Expected ") + std::to_string(minCount) + "–" + std::to_string(maxCount) + " IDs: " + name);
    std::vector<int> out;
    std::set<int> seen;
    for (const auto& value : *list)
    {
        const double d = static_cast<double>(value);
        if (! (value.isInt() || value.isInt64() || value.isDouble()) || ! std::isfinite(d) || std::floor(d) != d || d < 0 || d > maxId)
            throw std::runtime_error(std::string("Invalid ID in ") + name);
        if (! seen.insert(static_cast<int>(d)).second) throw std::runtime_error(std::string("Duplicate ID in ") + name);
        out.push_back(static_cast<int>(d));
    }
    return out;
}
StagedEdit make(model::EditCommand command)
{
    StagedEdit out;
    out.command = std::move(command);
    return out;
}
// An absolute setter that already holds the value is a successful no-op, not an error.
model::EditCommand orNothing(model::EditCommand command, const char* label)
{
    if (command.apply) return command;
    command.label = label;
    command.apply = [](model::Song&) {};
    command.revert = [](model::Song&) {};
    return command;
}
juce::var trackReceipt(const model::Track& track)
{
    return object({{"trackId", track.id}, {"kind", kindName(track.kind)}, {"name", track.name}});
}
int optionalColor(const juce::var& item, const char* name = "color")
{
    return has(item, name) ? integer(item, name, 0, 69) : -1;
}

// ---- song level
StagedEdit timeSignature(const model::Song& song, const juce::var& item)
{
    fields(item, {"type", "numerator", "denominator"});
    const int numerator = integer(item, "numerator", 1, 99), denominator = integer(item, "denominator", 1, 16);
    if (denominator != 1 && denominator != 2 && denominator != 4 && denominator != 8 && denominator != 16)
        invalid("denominator must be 1, 2, 4, 8 or 16");
    return make(orNothing(model::setTimeSignature(song, numerator, denominator), "Change Time Signature"));
}
StagedEdit punch(const model::Song& song, const juce::var& item)
{
    fields(item, {"type", "in", "out"});
    if (! has(item, "in") && ! has(item, "out")) throw std::runtime_error("Give in and/or out");
    const bool in = has(item, "in") ? boolean(item, "in") : song.punchIn, out = has(item, "out") ? boolean(item, "out") : song.punchOut;
    auto command = model::setPunch(song, in, out);
    if (! command.apply) command = orNothing(std::move(command), "Set Punch");
    return make(std::move(command));
}
StagedEdit scale(const model::Song& song, const juce::var& item)
{
    fields(item, {"type", "root", "name"});
    const int root = integer(item, "root", 0, 11), name = integer(item, "name", 0, static_cast<int>(model::kScales.size()) - 1);
    return make(orNothing(model::setScale(song, root, name), "Change Scale"));
}
StagedEdit quantization(const model::Song& song, const juce::var& item)
{
    fields(item, {"type", "value"});
    return make(orNothing(model::setGlobalQuantisation(song, integer(item, "value", 0, 13)), "Change Launch Quantisation"));
}

// ---- tracks
StagedEdit trackCreate(const model::Song& song, const juce::var& item)
{
    fields(item, {"type", "kind", "name", "color", "afterTrackId"});
    const auto kind = string(item, "kind", 16);
    const int color = optionalColor(item);
    const bool named = has(item, "name");
    const auto name = named ? string(item, "name", 256).toStdString() : std::string();
    StagedEdit out;
    if (kind == "return")
    {
        if (has(item, "afterTrackId")) invalid("Return tracks are appended after the existing returns; omit afterTrackId");
        if (song.returns.size() >= model::maxReturnTracks)
            throw OpError("capacity", "A set can have at most 12 return tracks");
        auto insert = io::createReturnTrack(song);
        if (named) { insert.track.name = name; insert.track.nameIsAutomatic = false; }
        if (color >= 0) insert.track.colorIndex = color;
        out.tracks.add(trackReceipt(insert.track));
        out.command = model::insertReturnTrack(song, std::move(insert));
        if (! out.command.apply) invalid("The return track could not be created");
        return out;
    }
    if (kind != "midi" && kind != "audio") throw std::runtime_error("kind must be midi, audio or return");
    int end = 0;
    int position = static_cast<int>(song.tracks.size());
    const model::Track* parent = nullptr;
    int parentIndex = -1;
    if (has(item, "afterTrackId"))
    {
        parentIndex = regularTrackIndex(song, integer(item, "afterTrackId", 0, maxId));
        parent = &song.tracks[static_cast<size_t>(parentIndex)];
        position = parentIndex + 1;
    }
    const bool midi = kind == "midi";
    auto track = midi ? io::createMidiTrack(song, named ? name : "MIDI", color >= 0 ? color : 4, end)
                      : io::createAudioTrack(song, named ? name : "Audio", color >= 0 ? color : 18, -1, -1, end);
    if (named) { track.name = name; track.nameIsAutomatic = false; }
    if (parent)
    {
        // Like Insert Track: the new track joins the group of the track it follows (or the group it follows).
        track.groupId = parent->kind == model::TrackKind::Group ? parent->id : parent->groupId;
        if (track.groupId >= 0) { track.outputRouting = "AudioOut/GroupTrack"; track.outputRoutingEdited = true; }
    }
    out.tracks.add(trackReceipt(track));
    auto command = model::insertTracks({std::move(track)}, end, midi ? "Insert MIDI Track" : "Insert Audio Track", position);
    if (parent && parent->kind == model::TrackKind::Group && parent->folded)
        command = model::compose(command.label, {model::setTrackFolded(song, parentIndex, false), command});
    out.command = std::move(command);
    return out;
}
void addDescendants(const model::Song& song, int id, std::set<int>& out)
{
    for (const auto& track : song.tracks)
        if (track.groupId == id && out.insert(track.id).second) addDescendants(song, track.id, out);
}
StagedEdit trackDelete(const model::Song& song, const juce::var& item)
{
    fields(item, {"type", "trackIds"});
    const auto ids = idList(item, "trackIds", 1, 256);
    std::vector<int> selection;
    std::set<int> removed;
    for (const int id : ids)
    {
        if (id == model::mainTrackId(song)) invalid("The main track cannot be deleted");
        selection.push_back(controlIndexOf(song, id, false));
        removed.insert(id);
        addDescendants(song, id, removed);
    }
    StagedEdit out;
    out.command = model::deleteTracks(song, selection);
    if (! out.command.apply) invalid("Those tracks cannot be deleted");
    juce::Array<juce::var> list;
    for (const int id : removed) list.add(id);
    out.extra = object({{"deletedTrackIds", list}});
    return out;
}
StagedEdit trackDuplicate(const model::Song& song, const juce::var& item)
{
    fields(item, {"type", "trackIds", "withAutomation"});
    std::vector<int> selection;
    for (const int id : idList(item, "trackIds", 1, 256)) selection.push_back(regularTrackIndex(song, id));
    const bool withAutomation = has(item, "withAutomation") ? boolean(item, "withAutomation") : true;
    auto prepared = io::prepareTrackDuplication(song, selection);
    if (! prepared.error.empty()) invalid(prepared.error);
    if (prepared.tracks.empty()) invalid("Nothing to duplicate");
    if (! withAutomation) for (auto& track : prepared.tracks) track.automation.clear();
    StagedEdit out;
    for (const auto& track : prepared.tracks) out.tracks.add(trackReceipt(track));
    out.command = model::insertTracks(std::move(prepared.tracks), prepared.pointeeEnd, "Duplicate Tracks", prepared.before);
    return out;
}
StagedEdit trackMove(const model::Song& song, const juce::var& item)
{
    fields(item, {"type", "trackIds", "beforeTrackId", "groupId"});
    std::vector<int> selection;
    for (const int id : idList(item, "trackIds", 1, 256)) selection.push_back(regularTrackIndex(song, id));
    int before = static_cast<int>(song.tracks.size()), parent = -1;
    if (has(item, "beforeTrackId"))
    {
        before = regularTrackIndex(song, integer(item, "beforeTrackId", 0, maxId));
        parent = song.tracks[static_cast<size_t>(before)].groupId;
    }
    if (has(item, "groupId"))
    {
        parent = integer(item, "groupId", -1, maxId);
        if (parent >= 0)
        {
            const auto group = std::find_if(song.tracks.begin(), song.tracks.end(), [parent](const auto& t) { return t.id == parent; });
            if (group == song.tracks.end() || group->kind != model::TrackKind::Group) replaced("groupId is not a group track");
        }
    }
    auto command = model::moveTracks(song, selection, before, parent);
    if (! command.apply) invalid("That destination is invalid or does not change the track order");
    return make(std::move(command));
}
StagedEdit trackGroup(const model::Song& song, const juce::var& item)
{
    fields(item, {"type", "trackIds", "name"});
    std::vector<int> selection, members;
    for (const int id : idList(item, "trackIds", 1, 256)) selection.push_back(regularTrackIndex(song, id));
    if (! model::groupableSelection(song, selection, members)) invalid("Those tracks must belong to the same group (or none) to be grouped");
    const auto name = has(item, "name") ? string(item, "name", 256).toStdString() : std::string("Group");
    int end = 0;
    auto group = io::createGroupTrack(song, name, song.tracks[static_cast<size_t>(members.front())].colorIndex, end);
    StagedEdit out;
    out.tracks.add(trackReceipt(group));
    out.command = model::groupTracks(song, members, std::move(group), end);
    return out;
}
StagedEdit trackUngroup(const model::Song& song, const juce::var& item)
{
    fields(item, {"type", "trackId"});
    const int index = regularTrackIndex(song, integer(item, "trackId", 0, maxId));
    if (song.tracks[static_cast<size_t>(index)].kind != model::TrackKind::Group) invalid("trackId is not a group track");
    return make(model::ungroupTrack(song, index));
}
std::vector<int> controlSelection(const model::Song& song, const juce::var& item)
{
    std::vector<int> selection;
    for (const int id : idList(item, "trackIds", 1, 256))
    {
        if (id == model::mainTrackId(song)) invalid("The main track has no such state");
        selection.push_back(controlIndexOf(song, id, false));
    }
    return selection;
}
StagedEdit trackHidden(const model::Song& song, const juce::var& item)
{
    fields(item, {"type", "trackIds", "hidden"});
    return make(orNothing(model::setTracksHidden(song, controlSelection(song, item), boolean(item, "hidden")), "Hide Tracks"));
}
StagedEdit trackDeactivated(const model::Song& song, const juce::var& item)
{
    fields(item, {"type", "trackIds", "deactivated"});
    return make(orNothing(model::setTracksDeactivated(song, controlSelection(song, item), boolean(item, "deactivated")), "Deactivate Tracks"));
}
StagedEdit trackMonitoring(const model::Song& song, const juce::var& item)
{
    fields(item, {"type", "trackId", "mode"});
    const int index = regularTrackIndex(song, integer(item, "trackId", 0, maxId));
    const auto& track = song.tracks[static_cast<size_t>(index)];
    if (track.kind != model::TrackKind::Audio && track.kind != model::TrackKind::Midi) invalid("Monitoring applies to audio and MIDI tracks");
    const auto mode = string(item, "mode", 8);
    const int value = mode == "in" ? 0 : mode == "auto" ? 1 : mode == "off" ? 2 : -1;
    if (value < 0) throw std::runtime_error("mode must be in, auto or off");
    if (value == track.monitoring) return make(orNothing({}, "Set Track Input"));
    return make(model::setTrackInput(song, index, track.inputRouting, track.inputUpperDisplay, track.inputLowerDisplay, value));
}

// ---- clips
model::ClipLocation clipLoc(const model::Song& song, const juce::var& item)
{
    const int track = regularTrackIndex(song, integer(item, "trackId", 0, maxId));
    const int uid = integer(item, "clipId", 0, maxId);
    if (has(item, "scene") && has(item, "laneId")) throw std::runtime_error("Choose scene OR laneId");
    if (has(item, "scene")) return model::ClipLocation::session(track, uid, integer(item, "scene", 0, 1000000));
    if (has(item, "laneId")) return model::ClipLocation::take(track, uid, integer(item, "laneId", 0, maxId));
    return model::ClipLocation::arrangement(track, uid);
}
const model::Clip& existingClip(const model::Song& song, const model::ClipLocation& at)
{
    const auto* clip = model::findClip(song, at);
    if (! clip) replaced("Clip no longer exists at the specified location");
    return *clip;
}
bool occupied(const model::Track& track, int skipUid, double start, double end)
{
    return std::any_of(track.arrangementClips.begin(), track.arrangementClips.end(), [&](const model::Clip& other) {
        return other.uid != skipUid && other.currentStart < end - 1e-9 && other.currentEnd > start + 1e-9;
    });
}
StagedEdit clipUpdate(const model::Song& song, const juce::var& item)
{
    fields(item, {"type", "trackId", "clipId", "scene", "laneId", "name", "color", "muted", "startBeat", "endBeat", "loop", "launchMode",
                  "launchQuantization", "legato", "gainDb", "pitchCoarse", "pitchFine"});
    const auto at = clipLoc(song, item);
    auto clip = existingClip(song, at);
    const bool arrangement = at.kind == model::ClipLocation::Kind::Arrangement, session = at.kind == model::ClipLocation::Kind::Session;
    const bool audio = clip.kind == model::Clip::Kind::Audio;
    bool changed = false;
    if (has(item, "name"))
    {
        if (! item["name"].isString()) throw std::runtime_error("Expected a string: name");
        clip.name = item["name"].toString().substring(0, 256).toStdString(); changed = true;
    }
    if (has(item, "color")) { clip.colorIndex = integer(item, "color", 0, 69); changed = true; }
    if (has(item, "muted")) { clip.disabled = boolean(item, "muted"); changed = true; }
    if (has(item, "startBeat") || has(item, "endBeat"))
    {
        if (! arrangement) invalid("startBeat/endBeat trim Arrangement clips only");
        const double start = has(item, "startBeat") ? number(item, "startBeat", 0, maxBeat) : clip.currentStart;
        const double end = has(item, "endBeat") ? number(item, "endBeat", 0, maxBeat) : clip.currentEnd;
        if (end <= start + 1e-6) invalid("endBeat must be after startBeat");
        if (has(item, "startBeat")) clip = model::withStart(clip, start, song.tempo);
        if (has(item, "endBeat")) clip = model::withEnd(clip, end, song.tempo);
        if (occupied(song.tracks[static_cast<size_t>(at.track)], clip.uid, clip.currentStart, clip.currentEnd))
            throw OpError("region_occupied", "Another clip occupies that region; trim or delete it first");
        changed = true;
    }
    if (has(item, "loop"))
    {
        const auto loop = item["loop"];
        fields(loop, {"enabled", "startBeat", "endBeat"});
        if (! has(loop, "enabled") && ! has(loop, "startBeat") && ! has(loop, "endBeat")) throw std::runtime_error("loop needs enabled, startBeat or endBeat");
        if (has(loop, "enabled")) clip.loop.enabled = boolean(loop, "enabled");
        if (has(loop, "startBeat")) clip.loop.loopStart = number(loop, "startBeat", -maxBeat, maxBeat);
        if (has(loop, "endBeat")) clip.loop.loopEnd = number(loop, "endBeat", -maxBeat, maxBeat);
        if (clip.loop.loopEnd <= clip.loop.loopStart + 1e-6) invalid("loop.endBeat must be after loop.startBeat");
        changed = true;
    }
    if (has(item, "launchMode") || has(item, "launchQuantization") || has(item, "legato"))
    {
        if (! session) invalid("Launch settings apply to Session clips (give scene)");
        if (has(item, "launchMode"))
        {
            const auto mode = string(item, "launchMode", 16);
            const int value = mode == "trigger" ? 0 : mode == "gate" ? 1 : mode == "toggle" ? 2 : mode == "repeat" ? 3 : -1;
            if (value < 0) throw std::runtime_error("launchMode must be trigger, gate, toggle or repeat");
            clip.launchMode = value;
        }
        if (has(item, "launchQuantization")) clip.launchQuantisation = integer(item, "launchQuantization", 0, 14);
        if (has(item, "legato")) clip.legato = boolean(item, "legato");
        changed = true;
    }
    if (has(item, "gainDb") || has(item, "pitchCoarse") || has(item, "pitchFine"))
    {
        if (! audio) throw OpError("unsupported_clip", "gainDb, pitchCoarse and pitchFine apply to audio clips");
        if (has(item, "gainDb")) clip.gain = std::pow(10.0, number(item, "gainDb", -70, 24) / 20.0);
        if (has(item, "pitchCoarse")) clip.pitchCoarse = integer(item, "pitchCoarse", -48, 48);
        if (has(item, "pitchFine")) clip.pitchFine = number(item, "pitchFine", -50, 50);
        changed = true;
    }
    if (! changed) throw std::runtime_error("Specify at least one clip property to change");
    return make(model::modifyClip(song, at, std::move(clip), "Edit Clip"));
}
StagedEdit clipSplit(const model::Song& song, const juce::var& item)
{
    fields(item, {"type", "trackIds", "beat", "endBeat"});
    const double beat = number(item, "beat", 0, maxBeat);
    const double end = has(item, "endBeat") ? number(item, "endBeat", 0, maxBeat) : -1.0;
    if (end >= 0 && end <= beat) invalid("endBeat must be after beat");
    std::vector<model::EditCommand> parts;
    for (const int id : idList(item, "trackIds", 1, 256))
    {
        const int index = regularTrackIndex(song, id);
        auto part = model::splitArrangementClips(song, index, index, beat, end);
        if (part.apply) parts.push_back(std::move(part));
    }
    if (parts.empty()) invalid("No Arrangement clip crosses that position");
    return make(model::compose("Split Clips", std::move(parts)));
}
std::pair<int, int> trackRange(const model::Song& song, const juce::var& item)
{
    const int a = regularTrackIndex(song, integer(item, "firstTrackId", 0, maxId)), b = regularTrackIndex(song, integer(item, "lastTrackId", 0, maxId));
    return {std::min(a, b), std::max(a, b)};
}
StagedEdit regionDuplicate(const model::Song& song, const juce::var& item)
{
    fields(item, {"type", "firstTrackId", "lastTrackId", "startBeat", "endBeat"});
    const auto [first, last] = trackRange(song, item);
    const double start = number(item, "startBeat", 0, maxBeat), end = number(item, "endBeat", 0, maxBeat);
    if (end <= start) invalid("endBeat must be after startBeat");
    auto command = model::duplicateArrangementRegion(song, first, last, start, end);
    if (! command.apply) invalid("Nothing to duplicate in that region");
    return make(std::move(command));
}
StagedEdit regionDelete(const model::Song& song, const juce::var& item)
{
    fields(item, {"type", "firstTrackId", "lastTrackId", "startBeat", "endBeat"});
    const auto [first, last] = trackRange(song, item);
    const double start = number(item, "startBeat", 0, maxBeat), end = number(item, "endBeat", 0, maxBeat);
    if (end <= start) invalid("endBeat must be after startBeat");
    auto command = model::deleteArrangementRegion(song, first, last, start, end);
    if (! command.apply) invalid("No clip content in that region (frozen tracks are left alone)");
    return make(std::move(command));
}
StagedEdit clipDuplicateLinked(const model::Song& song, const juce::var& item)
{
    fields(item, {"type", "trackId", "clipId"});
    const int track = regularTrackIndex(song, integer(item, "trackId", 0, maxId)), uid = integer(item, "clipId", 0, maxId);
    const auto* clip = model::findArrangementClip(song, track, uid);
    if (! clip) replaced("Arrangement clip no longer exists");
    if (clip->kind != model::Clip::Kind::Midi) throw OpError("unsupported_clip", "Only MIDI clips can be linked");
    auto command = model::duplicateClipLinked(song, track, uid);
    if (! command.apply) invalid("The clip could not be duplicated as a linked copy");
    return make(std::move(command));
}
StagedEdit clipUnlink(const model::Song& song, const juce::var& item)
{
    fields(item, {"type", "trackId", "clipId", "scene", "laneId"});
    const auto at = clipLoc(song, item);
    const auto& clip = existingClip(song, at);
    if (clip.linkGroup < 0) invalid("The clip is not linked");
    auto command = model::unlinkClip(song, at);
    if (! command.apply) invalid("The clip could not be unlinked");
    return make(std::move(command));
}
StagedEdit clipCopyToScene(const model::Song& song, const juce::var& item)
{
    fields(item, {"type", "trackId", "clipId", "scene", "toScene", "toTrackId"});
    if (has(item, "laneId")) throw std::runtime_error("Take-lane clips cannot be copied to a scene");
    const auto at = clipLoc(song, item);
    const auto& source = existingClip(song, at);
    const int toScene = integer(item, "toScene", 0, 999);
    const int toTrack = has(item, "toTrackId") ? regularTrackIndex(song, integer(item, "toTrackId", 0, maxId)) : at.track;
    const auto& from = song.tracks[static_cast<size_t>(at.track)];
    const auto& to = song.tracks[static_cast<size_t>(toTrack)];
    if (to.kind != from.kind || (to.kind != model::TrackKind::Midi && to.kind != model::TrackKind::Audio))
        invalid("The destination must be a track of the same kind (audio or MIDI)");
    if (at.kind == model::ClipLocation::Kind::Arrangement && source.kind != model::Clip::Kind::Midi)
        throw OpError("unsupported_clip", "Arrangement audio clips cannot be copied to a scene yet; copy a MIDI clip or a Session clip");
    if (static_cast<size_t>(toScene) < to.slots.size() && to.slots[static_cast<size_t>(toScene)].clip)
        throw OpError("region_occupied", "That Session slot already holds a clip");
    if (to.frozen) invalid("Unfreeze the destination track first");
    auto copy = source;
    copy.cloneOfXmlId = copy.xmlId >= 0 ? copy.xmlId : copy.cloneOfXmlId;
    if (copy.xmlId >= 0) copy.cloneOfTrackId = from.id;
    if (at.kind == model::ClipLocation::Kind::Session) copy.cloneOfSessionSlot = at.scene;
    copy.xmlId = -1;
    copy.linkGroup = -1;
    auto command = model::setSessionClip(song, toTrack, toScene, std::move(copy));
    command.label = "Copy Clip to Scene";
    return make(std::move(command));
}

// ---- take lanes
const model::TakeLane& existingLane(const model::Song& song, int track, int laneId)
{
    const auto* lane = model::findTakeLane(song, track, laneId);
    if (! lane) replaced("Take lane no longer exists");
    return *lane;
}
StagedEdit takeRename(const model::Song& song, const juce::var& item)
{
    fields(item, {"type", "trackId", "laneId", "name"});
    const int track = regularTrackIndex(song, integer(item, "trackId", 0, maxId)), lane = integer(item, "laneId", 0, maxId);
    existingLane(song, track, lane);
    if (! item["name"].isString()) throw std::runtime_error("Expected a string: name");
    return make(model::renameTakeLane(song, track, lane, item["name"].toString().substring(0, 256).toStdString()));
}
StagedEdit takeMove(const model::Song& song, const juce::var& item)
{
    fields(item, {"type", "trackId", "laneId", "delta"});
    const int track = regularTrackIndex(song, integer(item, "trackId", 0, maxId)), lane = integer(item, "laneId", 0, maxId);
    existingLane(song, track, lane);
    const int delta = integer(item, "delta", -64, 64);
    if (delta == 0) throw std::runtime_error("delta must not be 0");
    auto command = model::moveTakeLane(song, track, lane, delta);
    if (! command.apply) invalid("The take lane cannot move that far");
    return make(std::move(command));
}
StagedEdit takeDelete(const model::Song& song, const juce::var& item)
{
    fields(item, {"type", "trackId", "laneId"});
    const int track = regularTrackIndex(song, integer(item, "trackId", 0, maxId)), lane = integer(item, "laneId", 0, maxId);
    existingLane(song, track, lane);
    return make(model::deleteTakeLane(song, track, lane));
}
StagedEdit takeDuplicate(const model::Song& song, const juce::var& item)
{
    fields(item, {"type", "trackId", "laneId"});
    const int track = regularTrackIndex(song, integer(item, "trackId", 0, maxId)), lane = integer(item, "laneId", 0, maxId);
    existingLane(song, track, lane);
    return make(model::duplicateTakeLane(song, track, lane));
}
StagedEdit takeAudition(const model::Song& song, const juce::var& item)
{
    fields(item, {"type", "trackId", "laneId"});
    const int track = regularTrackIndex(song, integer(item, "trackId", 0, maxId));
    const int lane = has(item, "laneId") ? integer(item, "laneId", 0, maxId) : -1; // omitted: back to the main lane
    if (lane >= 0) existingLane(song, track, lane);
    return make(model::auditionTakeLane(song, track, lane));
}
StagedEdit takeComp(const model::Song& song, const juce::var& item)
{
    fields(item, {"type", "trackId", "laneId", "startBeat", "endBeat"});
    const int track = regularTrackIndex(song, integer(item, "trackId", 0, maxId)), lane = integer(item, "laneId", 0, maxId);
    existingLane(song, track, lane);
    const double start = number(item, "startBeat", 0, maxBeat), end = number(item, "endBeat", 0, maxBeat);
    if (end <= start) invalid("endBeat must be after startBeat");
    auto command = model::compTakeRange(song, track, lane, start, end);
    if (! command.apply) invalid("Nothing to comp in that range");
    return make(std::move(command));
}

using Builder = StagedEdit (*)(const model::Song&, const juce::var&);
const std::vector<std::pair<std::string, Builder>>& builders()
{
    static const std::vector<std::pair<std::string, Builder>> table = {
        {"time_signature.set", timeSignature}, {"punch.set", punch}, {"scale.set", scale}, {"quantization.set", quantization},
        {"track.create", trackCreate}, {"track.delete", trackDelete}, {"track.duplicate", trackDuplicate}, {"track.move", trackMove},
        {"track.group", trackGroup}, {"track.ungroup", trackUngroup}, {"track.set_hidden", trackHidden},
        {"track.set_deactivated", trackDeactivated}, {"track.set_monitoring", trackMonitoring},
        {"clip.update", clipUpdate}, {"clip.split", clipSplit}, {"region.duplicate", regionDuplicate}, {"region.delete", regionDelete},
        {"clip.duplicate_linked", clipDuplicateLinked}, {"clip.unlink", clipUnlink}, {"clip.copy_to_scene", clipCopyToScene},
        {"take_lane.rename", takeRename}, {"take_lane.move", takeMove}, {"take_lane.delete", takeDelete},
        {"take_lane.duplicate", takeDuplicate}, {"take_lane.audition", takeAudition}, {"take_lane.comp", takeComp}};
    return table;
}
} // namespace

const std::vector<std::string>& newEditTypeNames()
{
    static const std::vector<std::string> names = [] {
        std::vector<std::string> out;
        for (const auto& entry : builders()) out.push_back(entry.first);
        return out;
    }();
    return names;
}
bool isNewEditType(const juce::String& type)
{
    const auto text = type.toStdString();
    return std::any_of(builders().begin(), builders().end(), [&](const auto& entry) { return entry.first == text; });
}
bool editNeedsLiveSync(const juce::String& type)
{
    return type == "track.delete" || type == "track.duplicate" || type == "track.move" || type == "track.group" || type == "track.ungroup";
}
StagedEdit buildEdit(const model::Song& staged, const juce::var& item)
{
    const auto type = item["type"].toString().toStdString();
    for (const auto& entry : builders())
        if (entry.first == type) return entry.second(staged, item);
    throw OpError("unsupported_operation", "Unsupported edit type: " + type);
}
} // namespace sonora::control::detail

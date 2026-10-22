// als_dump — prints what Sonora understood from a .als file.
//
//   als_dump <file.als> [--notes] [--params] [--automation]
//   als_dump --summary <file1.als> <file2.als> ...    one line per file (bulk regression check)
//   als_dump --roundtrip <files...>                   load, save unchanged to a temp file, reload, compare (ALS-10)
//   als_dump --resave-editor <source> <copy>           disposable loop/warp/probability interoperability probe
//   als_dump --new-set-demo <out.als>                  same, plus authored arrangement/session MIDI clips (interop probe)
//   als_dump --new-set-group <out.als>                 demo project whose first two MIDI tracks are in a group, plus a nested group (interop probe)
//   als_dump --new-set-audio <out.als> <sample.wav>   new project with that sample on the first audio track
//   als_dump --new-set <out.als> [midi audio returns scenes]                       writes Sonora's generated empty project (File > New Project)
//
// Exit code 0 when every file loaded, 1 otherwise.

#include "core/Model.h"
#include "core/Edit.h"
#include "io/als/AlsNewSet.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsTemplates.h"
#include "io/als/AlsWriter.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <cstdio>
#include <iostream>

using namespace sonora;

namespace
{

void printDevices(const std::vector<model::Device>& devices, int indent, bool params)
{
    for (const auto& d : devices)
    {
        std::printf("%*s- [%s] %s (%s)%s\n", indent, "", model::toString(d.kind), d.displayName.c_str(),
                    d.type.c_str(), d.enabled ? "" : " OFF");
        if (d.plugin)
            std::printf("%*s  state=%zu bytes, controllerState=%zu bytes\n", indent, "", d.plugin->state.size(),
                        d.plugin->controllerState.size());
        if (params)
            for (const auto& p : d.parameters)
                std::printf("%*s  %s = %g\n", indent, "", p.path.c_str(), p.value);
        for (const auto& chain : d.chains)
        {
            std::printf("%*s  chain \"%s\"\n", indent, "", chain.name.c_str());
            printDevices(chain.devices, indent + 4, params);
        }
    }
}

// Names what an automation envelope drives, mirroring the engine's binding rules (ENG-11).
std::string describeTarget(const model::Track& t, int target)
{
    const auto& m = t.mixer;
    if (target == m.volumeTargetId)
        return "mixer Volume";
    if (target == m.panTargetId)
        return "mixer Pan";
    if (target == m.speakerTargetId)
        return "mixer Track Activator";
    for (size_t i = 0; i < m.sendTargetIds.size(); ++i)
        if (target == m.sendTargetIds[i])
            return "mixer Send " + std::to_string(i);
    for (const auto& d : t.devices)
        for (const auto& p : d.parameters)
            if (p.automationTargetId == target)
                return d.displayName + " / " + p.path;
    return "(unresolved: not a top-level mixer/device parameter)";
}

void printAutomation(const model::Track& t)
{
    for (const auto& e : t.automation)
    {
        const double first = e.points.empty() ? 0.0 : e.points.front().value;
        const double last = e.points.empty() ? 0.0 : e.points.back().value;
        std::printf("    automation #%d %-9s points=%-4zu first=%g last=%g -> %s\n", e.targetId,
                    e.stepped ? "stepped" : "linear", e.points.size(), first, last,
                    describeTarget(t, e.targetId).c_str());
    }
}

void printTrack(const model::Track& t, bool notes, bool params, bool automation)
{
    int sessionClips = 0;
    for (const auto& s : t.slots)
        sessionClips += s.clip.has_value() ? 1 : 0;

    std::printf("%-6s #%-4d %-28s color=%-2d group=%-4d vol=%.3f pan=%+.2f %s session=%d arrangement=%zu takes=%zu%s%s\n",
                model::toString(t.kind), t.id, t.name.c_str(), t.colorIndex, t.groupId, t.mixer.volume,
                t.mixer.pan, t.mixer.speakerOn ? "on " : "OFF", sessionClips, t.arrangementClips.size(), t.takeLanes.size(),
                t.hidden ? " hidden" : "", t.deactivated ? " deactivated" : "");
    printDevices(t.devices, 4, params);
    if (automation)
        printAutomation(t);

    if (notes)
        for (const auto& c : t.arrangementClips)
        {
            std::printf("    clip @%.3f..%.3f \"%s\" %s", c.currentStart, c.currentEnd, c.name.c_str(),
                        c.kind == model::Clip::Kind::Midi ? "MIDI" : "AUDIO");
            if (c.kind == model::Clip::Kind::Midi)
                std::printf(" notes=%zu\n", c.notes.size());
            else
                std::printf(" warped=%d mode=%s file=%s\n", c.warped, model::toString(c.warpMode),
                            c.sample.relativePath.c_str());
        }
}

// First place where two XML trees differ (for diagnosing --roundtrip failures).
std::string firstDifference(const juce::XmlElement& a, const juce::XmlElement& b, const std::string& path)
{
    const std::string here = path + "/" + a.getTagName().toStdString();
    if (a.getTagName() != b.getTagName())
        return here + ": tag " + a.getTagName().toStdString() + " vs " + b.getTagName().toStdString();
    if (a.getNumAttributes() != b.getNumAttributes())
        return here + ": attribute count";
    for (int i = 0; i < a.getNumAttributes(); ++i)
        if (a.getAttributeName(i) != b.getAttributeName(i) || a.getAttributeValue(i) != b.getAttributeValue(i))
            return here + ": attribute " + a.getAttributeName(i).toStdString() + " '" + a.getAttributeValue(i).substring(0, 60).toStdString()
                   + "' vs '" + b.getAttributeValue(i).substring(0, 60).toStdString() + "'";
    if (a.isTextElement() != b.isTextElement() || (a.isTextElement() && a.getText() != b.getText()))
        return here + ": text '" + a.getText().substring(0, 50).replace("\n", "\\n").replace("\t", "\\t").toStdString() + "' vs '"
               + b.getText().substring(0, 50).replace("\n", "\\n").replace("\t", "\\t").toStdString() + "'";
    if (a.getNumChildElements() != b.getNumChildElements())
        return here + ": child count " + std::to_string(a.getNumChildElements()) + " vs " + std::to_string(b.getNumChildElements());
    const auto* ca = a.getFirstChildElement();
    const auto* cb = b.getFirstChildElement();
    for (; ca != nullptr && cb != nullptr; ca = ca->getNextElement(), cb = cb->getNextElement())
    {
        auto d = firstDifference(*ca, *cb, here);
        if (! d.empty())
            return d;
    }
    return {};
}

int dumpOne(const juce::File& f, bool notes, bool params, bool automation)
{
    auto r = io::loadAls(f);
    if (! r)
    {
        std::fprintf(stderr, "ERROR %s: %s\n", f.getFullPathName().toRawUTF8(), r.error.c_str());
        return 1;
    }
    const auto& s = *r.song;
    std::printf("File:    %s\nCreator: %s (schema %s)\nTempo:   %.3f  %d/%d\n", s.sourcePath.c_str(),
                s.creator.c_str(), s.schemaVersion.c_str(), s.tempo, s.timeSigNumerator, s.timeSigDenominator);
    std::printf("Scenes:  %zu  Locators: %zu  Loop: %s %.2f+%.2f\n\n", s.scenes.size(), s.locators.size(),
                s.loopOn ? "on" : "off", s.loopStart, s.loopLength);
    for (const auto& t : s.tracks)
        printTrack(t, notes, params, automation);
    for (const auto& t : s.returns)
        printTrack(t, notes, params, automation);
    printTrack(s.main, notes, params, automation);

    if (! s.warnings.empty())
    {
        std::printf("\nWarnings:\n");
        for (const auto& w : s.warnings)
            std::printf("  - %s\n", w.c_str());
    }
    return 0;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: als_dump <file.als> [--notes] [--params] [--automation]\n"
                             "       als_dump --summary <files...>\n");
        return 2;
    }

    const std::string first = argv[1];
    if (first == "--new-set-demo" && argc >= 3)
    {
        // New project + authored MIDI clips, saved through the normal writer (interoperability probe).
        const auto out = juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]);
        const auto scratch = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_new_set_demo");
        scratch.deleteRecursively();
        scratch.createDirectory();
        const auto base = scratch.getChildFile("Untitled.als");
        std::string error;
        if (out.existsAsFile() || ! io::writeAlsDom(*io::createNewSetDom(), base, error))
        {
            std::printf("cannot create %s\n", out.getFullPathName().toRawUTF8());
            return 1;
        }
        auto loaded = io::loadAls(base);
        if (! loaded)
            return 1;
        auto& song = *loaded.song;
        song.midiClipTemplate = std::make_shared<const std::string>(io::midiClipTemplate());
        model::EditHistory history;
        model::Clip arrangement, session;
        if (! model::makeEmptyMidiClip(song, 4.0, 8.0, arrangement) || ! model::makeEmptyMidiClip(song, 0.0, 4.0, session))
            return 1;
        arrangement.name = "Sonora Arrangement Clip";
        arrangement.notes = {{0.0, 1.0, 60, 100.0f}, {1.0, 1.0, 64, 100.0f}, {2.0, 2.0, 67, 90.0f}};
        session.name = "Sonora Session Clip";
        session.notes = {{0.0, 0.5, 72, 100.0f}, {1.0, 0.5, 74, 100.0f}};
        history.perform(song, model::insertClip(0, arrangement, "Insert MIDI Clip"));
        history.perform(song, model::setSessionClip(song, 1, 2, session));
        io::AlsSaveOptions options;
        options.portableReferences = options.copyProjectSamples = true;
        const auto saved = io::saveAls(song, out, options);
        std::printf("%s %s\n", saved.ok ? "wrote" : "failed", saved.ok ? out.getFullPathName().toRawUTF8() : saved.error.c_str());
        return saved.ok ? 0 : 1;
    }
    if (first == "--new-set-group" && argc >= 3)
    {
        const auto out = juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]);
        const auto scratch = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_new_set_group");
        scratch.deleteRecursively();
        scratch.createDirectory();
        const auto base = scratch.getChildFile("Untitled.als");
        std::string error;
        if (out.existsAsFile() || ! io::writeAlsDom(*io::createNewSetDom(), base, error))
        {
            std::printf("cannot create %s\n", out.getFullPathName().toRawUTF8());
            return 1;
        }
        auto loaded = io::loadAls(base);
        if (! loaded)
            return 1;
        auto& song = *loaded.song;
        song.midiClipTemplate = std::make_shared<const std::string>(io::midiClipTemplate());
        model::EditHistory history;
        model::Clip clip;
        if (model::makeEmptyMidiClip(song, 0.0, 4.0, clip))
        {
            clip.notes = {{0.0, 1.0, 60, 100.0f}, {1.0, 1.0, 64, 100.0f}};
            history.perform(song, model::insertClip(0, clip, "Insert MIDI Clip"));
        }
        int pointeeEnd = 0;
        history.perform(song, model::groupTracks(song, 0, 2, io::createGroupTrack(song, "Sonora Group", 13, pointeeEnd), pointeeEnd));
        // a second group around the audio tracks, and an outer one around everything so far
        history.perform(song, model::groupTracks(song, 3, 2, io::createGroupTrack(song, "Audio Group", 17, pointeeEnd), pointeeEnd));
        history.perform(song, model::groupTracks(song, 0, (int) song.tracks.size(), io::createGroupTrack(song, "Outer Group", 21, pointeeEnd), pointeeEnd));
        io::AlsSaveOptions options;
        options.portableReferences = options.copyProjectSamples = true;
        const auto saved = io::saveAls(song, out, options);
        std::printf("%s %s\n", saved.ok ? "wrote" : "failed", saved.ok ? out.getFullPathName().toRawUTF8() : saved.error.c_str());
        return saved.ok ? 0 : 1;
    }
    if (first == "--new-set-audio" && argc >= 4)
    {
        // New project with one dropped sample on the first audio track (same path as the browser drop).
        const auto out = juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]);
        const juce::File wav(juce::String::fromUTF8(argv[3]));
        const auto scratch = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("sonora_new_set_audio");
        scratch.deleteRecursively();
        scratch.createDirectory();
        const auto base = scratch.getChildFile("Untitled.als");
        std::string error;
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        const std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(wav));
        if (out.existsAsFile() || ! reader || ! io::writeAlsDom(*io::createNewSetDom(), base, error))
        {
            std::printf("cannot create %s\n", out.getFullPathName().toRawUTF8());
            return 1;
        }
        auto loaded = io::loadAls(base);
        if (! loaded)
            return 1;
        auto& song = *loaded.song;
        model::Clip clip;
        clip.kind = model::Clip::Kind::Audio;
        clip.resetSourceMarkers = true;
        clip.name = wav.getFileNameWithoutExtension().toStdString();
        clip.colorIndex = song.tracks[2].colorIndex;
        clip.sample.absolutePath = wav.getFullPathName().toStdString();
        clip.sampleRate = reader->sampleRate;
        clip.lengthSamples = reader->lengthInSamples;
        const double seconds = (double) clip.lengthSamples / clip.sampleRate;
        clip.loop = {0, seconds, 0, false};
        clip.time = clip.currentStart = 0.0;
        clip.currentEnd = seconds * song.tempo / 60.0;
        clip.sourceXml = std::make_shared<const std::string>(io::audioClipTemplate(clip));
        model::EditHistory history;
        history.perform(song, model::insertClip(2, clip, "Insert Sample"));
        io::AlsSaveOptions options;
        options.portableReferences = options.copyProjectSamples = true;
        const auto saved = io::saveAls(song, out, options);
        std::printf("%s %s\n", saved.ok ? "wrote" : "failed", saved.ok ? out.getFullPathName().toRawUTF8() : saved.error.c_str());
        return saved.ok ? 0 : 1;
    }
    if (first == "--new-set" && argc >= 3)
    {
        const auto out = juce::File::getCurrentWorkingDirectory().getChildFile(argv[2]);
        std::string error;
        io::NewSetOptions options;
        if (argc >= 7) // --new-set out midiTracks audioTracks returnTracks scenes
        {
            options.midiTracks = std::atoi(argv[3]);
            options.audioTracks = std::atoi(argv[4]);
            options.returnTracks = std::atoi(argv[5]);
            options.scenes = std::atoi(argv[6]);
        }
        if (out.existsAsFile() || ! io::writeAlsDom(*io::createNewSetDom(options), out, error))
        {
            std::printf("cannot write %s: %s\n", out.getFullPathName().toRawUTF8(), out.existsAsFile() ? "already exists" : error.c_str());
            return 1;
        }
        std::printf("wrote %s\n", out.getFullPathName().toRawUTF8());
        return 0;
    }
    if ((first == "--resave" || first == "--resave-edited" || first == "--resave-editor" || first == "--resave-takes") && argc >= 4)
    {
        auto a = io::loadAls(juce::File(juce::String::fromUTF8(argv[2])));
        if (! a)
            return 1;
        const auto destination = juce::File::getCurrentWorkingDirectory().getChildFile(argv[3]);
        if (first != "--resave" && destination == juce::File(juce::String::fromUTF8(argv[2])))
        { std::fprintf(stderr, "Edit probes require a separate destination\n"); return 1; }
        if (first == "--resave-takes")
        {
            model::EditHistory history;
            bool found = false;
            for (size_t track = 0; track < a.song->tracks.size() && ! found; ++track)
                if (! a.song->tracks[track].arrangementClips.empty())
                {
                    model::TakeLane lane; lane.name = "Sonora retained take";
                    lane.clips = {a.song->tracks[track].arrangementClips[0]};
                    history.perform(*a.song, model::appendTakeLanes(*a.song, (int) track, {std::move(lane)}));
                    found = true;
                }
            if (! found) { std::fprintf(stderr, "Source needs an Arrangement clip\n"); return 1; }
        }
        if (first == "--resave-edited") // Disposable interoperability probe; source is never overwritten.
        {
            model::EditHistory history;
            bool found = false;
            for (size_t track = 0; track < a.song->tracks.size() && ! found; ++track)
                for (size_t slot = 0; slot < a.song->tracks[track].slots.size(); ++slot)
                    if (const auto& source = a.song->tracks[track].slots[slot].clip)
                    {
                        auto clip = *source;
                        if (clip.kind != model::Clip::Kind::Midi) continue;
                        clip.xmlId = clip.cloneOfXmlId = -1;
                        clip.cloneOfSessionSlot = (int) slot;
                        clip.cloneOfTrackId = a.song->tracks[track].id;
                        clip.currentStart = clip.time = 4;
                        clip.currentEnd = 8;
                        clip.name = "Sonora edited notes";
                        clip.notes = {{0, 1, 60, 100}, {1, 0.5, 64, 90}, {2, 1, 67, 110}};
                        history.perform(*a.song, model::insertClip((int) track, clip, "Interop probe"));
                        history.perform(*a.song, model::setSessionClip(*a.song, (int) track, 1, clip));
                        a.song->tracks[track].name = "Sonora Keys";
                        a.song->scenes[1].name = "Sonora Scene";
                        a.song->locators = {{4, "Sonora Marker"}};
                        model::AutomationEnvelope env;
                        size_t automationTrack = track;
                        for (size_t i = 0; i < a.song->tracks.size(); ++i)
                            if (a.song->tracks[i].kind == model::TrackKind::Audio) { automationTrack = i; break; }
                        env.targetId = a.song->tracks[automationTrack].mixer.volumeTargetId;
                        env.points = {{-63072000, 1}, {4, 1}, {8, 0.25}};
                        history.perform(*a.song, model::setAutomation(*a.song, (int) automationTrack, env));
                        found = true;
                        break;
                    }
            if (! found) { std::fprintf(stderr, "Need a MIDI session clip for the edit probe\n"); return 1; }
        }
        if (first == "--resave-editor")
        {
            model::EditHistory history;
            for (size_t track = 0; track < a.song->tracks.size(); ++track)
            {
                auto edit = [&](const model::Clip& source, int scene) {
                    auto clip = source;
                    clip.resetSourceMarkers = true;
                    if (clip.kind == model::Clip::Kind::Midi)
                    {
                        if (! clip.notes.empty()) clip.notes[0].probability = 0.5f;
                        clip.loop = {0.25, 3.5, 0.25, true};
                    }
                    else
                    {
                        const double duration = clip.sampleRate > 0 ? (double) clip.lengthSamples / clip.sampleRate : 0;
                        if (duration <= 0) return;
                        clip.warped = true;
                        clip.warpMode = model::WarpMode::Beats;
                        clip.warpMarkers = {{0, 0}, {duration * 0.6, 2}, {duration, 4}};
                        clip.loop = {0, 4, 0.5, true};
                    }
                    history.perform(*a.song, model::modifyClip(*a.song, (int) track, source.uid, std::move(clip), "Editor probe", scene));
                };
                const auto clips = a.song->tracks[track].arrangementClips;
                for (const auto& clip : clips) edit(clip, -1);
                const auto slots = a.song->tracks[track].slots;
                for (size_t scene = 0; scene < slots.size(); ++scene)
                    if (slots[scene].clip) edit(*slots[scene].clip, (int) scene);
            }
        }
        const auto saved = io::saveAls(*a.song, destination);
        std::printf("%s %s (added %d, removed %d)\n", saved.ok ? "saved" : "FAILED", saved.error.c_str(), saved.clipsAdded, saved.clipsRemoved);
        return saved.ok ? 0 : 1;
    }
    if (first == "--roundtrip")
    {
        auto countElements = [](const juce::XmlElement& e, auto&& self) -> long {
            long n = 1;
            for (const auto* c : e.getChildIterator())
                n += self(*c, self);
            return n;
        };
        int failures = 0;
        for (int i = 2; i < argc; ++i)
        {
            const juce::File f(juce::String::fromUTF8(argv[i]));
            auto a = io::loadAls(f);
            std::string err;
            auto domA = io::readAlsDom(f, err);
            if (! a || domA == nullptr)
            {
                std::printf("FAIL load  %s\n", argv[i]);
                ++failures;
                continue;
            }
            juce::TemporaryFile out(".als");
            const auto saved = io::saveAls(*a.song, out.getFile());
            auto b = saved.ok ? io::loadAls(out.getFile()) : io::AlsLoadResult{};
            auto domB = saved.ok ? io::readAlsDom(out.getFile(), err) : nullptr;
            bool same = saved.ok && b && domB != nullptr && domA->isEquivalentTo(domB.get(), false) // identical XML, not just the same size
                        && a.song->tracks.size() == b.song->tracks.size() && a.song->returns.size() == b.song->returns.size();
            std::size_t clipsA = 0, clipsB = 0, notesA = 0, notesB = 0;
            if (same)
                for (std::size_t t = 0; t < a.song->tracks.size(); ++t)
                {
                    clipsA += a.song->tracks[t].arrangementClips.size();
                    clipsB += b.song->tracks[t].arrangementClips.size();
                    for (const auto& c : a.song->tracks[t].arrangementClips)
                        notesA += c.notes.size();
                    for (const auto& c : b.song->tracks[t].arrangementClips)
                        notesB += c.notes.size();
                }
            same = same && clipsA == clipsB && notesA == notesB && std::abs(a.song->tempo - b.song->tempo) < 1e-9
                   && saved.clipsAdded == 0 && saved.clipsRemoved == 0;
            if (! same && domA != nullptr && domB != nullptr)
                std::printf("      first difference: %s\n", firstDifference(*domA, *domB, "").c_str());
            std::printf("%s  %s  (identical XML, elements %ld -> %ld, clips %zu, notes %zu)\n", same ? "OK  " : "FAIL", f.getFileName().toRawUTF8(),
                        countElements(*domA, countElements), domB ? countElements(*domB, countElements) : 0L, clipsA, notesA);
            failures += same ? 0 : 1;
        }
        return failures == 0 ? 0 : 1;
    }
    if (first == "--summary")
    {
        int failures = 0;
        for (int i = 2; i < argc; ++i)
        {
            const juce::File f(juce::String::fromUTF8(argv[i]));
            const auto start = juce::Time::getMillisecondCounterHiRes();
            auto r = io::loadAls(f);
            const auto ms = juce::Time::getMillisecondCounterHiRes() - start;
            if (! r)
            {
                ++failures;
                std::printf("FAIL  %s: %s\n", argv[i], r.error.c_str());
                continue;
            }
            size_t clips = 0, devices = 0;
            for (const auto& t : r.song->tracks)
            {
                clips += t.arrangementClips.size();
                for (const auto& sl : t.slots)
                    clips += sl.clip ? 1 : 0;
                devices += t.devices.size();
            }
            std::printf("OK    %6.0fms tracks=%-3zu returns=%zu clips=%-5zu devices=%-3zu warnings=%-3zu %s\n", ms,
                        r.song->tracks.size(), r.song->returns.size(), clips, devices, r.song->warnings.size(),
                        f.getFileName().toRawUTF8());
        }
        return failures == 0 ? 0 : 1;
    }

    bool notes = false, params = false, automation = false;
    for (int i = 2; i < argc; ++i)
    {
        const std::string a = argv[i];
        notes |= a == "--notes";
        params |= a == "--params";
        automation |= a == "--automation";
    }
    return dumpOne(juce::File(juce::String::fromUTF8(argv[1])), notes, params, automation);
}

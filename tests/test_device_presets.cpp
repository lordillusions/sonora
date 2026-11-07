// LIB-2: reference device presets (.adv) indexed from a library folder and loaded as devices of a set.
// The fixtures are written here with Sonora-chosen names and values (no reference preset is copied).
#include "TestFramework.h"
#include "core/DeviceCatalog.h"
#include "core/Edit.h"
#include "io/als/AlsReader.h"
#include "io/als/AlsWriter.h"
#include "io/als/DevicePresets.h"
#include "io/als/LiveCompat.h"

#include <cstdlib>
#include <functional>
#include <set>

using namespace sonora;

namespace
{
void setVariable(const char* name, const char* value) // nullptr unsets
{
#if defined(_WIN32)
    _putenv_s(name, value != nullptr ? value : "");
#else
    if (value != nullptr) ::setenv(name, value, 1); else ::unsetenv(name);
#endif
}

// A parameter element as the reference stores it in a preset: every pointee id 0.
std::string parameter(const std::string& name, const std::string& value)
{
    return "<" + name + "><LomId Value=\"0\" /><Manual Value=\"" + value + "\" /><AutomationTarget Id=\"0\"><LockEnvelope Value=\"0\" />"
           "</AutomationTarget><ModulationTarget Id=\"0\"><LockEnvelope Value=\"0\" /></ModulationTarget></" + name + ">";
}

std::string device(const std::string& tag, const std::string& userName, const std::string& body)
{
    return "<?xml version=\"1.0\" encoding=\"UTF-8\"?><Ableton MajorVersion=\"5\" MinorVersion=\"12.0_12400\" Creator=\"Sonora test\"><" + tag
           + "><LomId Value=\"0\" /><On><LomId Value=\"0\" /><Manual Value=\"true\" /><AutomationTarget Id=\"0\"><LockEnvelope Value=\"0\" />"
             "</AutomationTarget></On><Pointee Id=\"0\" /><UserName Value=\"" + userName + "\" />" + body + "</" + tag + "></Ableton>";
}

void writePreset(const juce::File& file, const std::string& xml)
{
    REQUIRE(file.getParentDirectory().createDirectory());
    file.deleteFile();
    juce::FileOutputStream out(file);
    REQUIRE(out.openedOk());
    juce::GZIPCompressorOutputStream gz(out, 9, juce::GZIPCompressorOutputStream::windowBitsGZIP);
    gz.write(xml.data(), xml.size());
}

std::multiset<int> pointeeIds(const juce::XmlElement& e)
{
    std::multiset<int> ids;
    std::function<void(const juce::XmlElement&)> collect = [&](const juce::XmlElement& x) {
        if (io::isPointeeElement(x)) ids.insert(x.getIntAttribute("Id", -1));
        for (const auto* c : x.getChildIterator()) collect(*c);
    };
    collect(e);
    return ids;
}

struct Library
{
    juce::TemporaryFile folder;
    juce::File root, bass, loose, sampler, analog, rack, eq, table;
    Library()
    {
        root = folder.getFile();
        root.createDirectory();
        const auto instruments = root.getChildFile("Devices/Instruments");
        bass = instruments.getChildFile("Wavetable/Bass/Sonora Test Bass.adv");
        writePreset(bass, device("InstrumentVector", "Sonora Test Bass",
                                 "<SpriteName1 Value=\"Sonora Test Table\" /><UserSprite1><Value><SampleRef><FileRef><RelativePathType Value=\"5\" />"
                                 "<RelativePath Value=\"Samples/Tables/Test Table.wav\" /><Path Value=\"/nowhere/Test Table.wav\" /></FileRef>"
                                 "</SampleRef></Value></UserSprite1>"
                                 + parameter("Voice_Oscillator1_Pitch_Transpose", "-7") + parameter("Voice_Filter1_Frequency", "321.5")));
        loose = instruments.getChildFile("Wavetable/Plain Lead.adv");
        writePreset(loose, device("InstrumentVector", "Plain Lead", parameter("Voice_Oscillator1_Pitch_Transpose", "12")));
        REQUIRE(instruments.getChildFile("Wavetable/Ableton Folder Info").createDirectory());
        REQUIRE(instruments.getChildFile("Wavetable/Ableton Folder Info/Properties.cfg").replaceWithText("not a preset"));
        sampler = instruments.getChildFile("Sampler/Keys/Sonora Keys.adv"); // folder name differs from the catalog's
        writePreset(sampler, device("MultiSampler", "", ""));
        analog = instruments.getChildFile("Analog/Pads/Not Here.adv"); // a catalog device Sonora does not run
        writePreset(analog, device("UltraAnalog", "Not Here", ""));
        rack = instruments.getChildFile("Instrument Rack/Stack.adv");
        writePreset(rack, device("InstrumentGroupDevice", "Stack", ""));
        eq = root.getChildFile("Presets/Audio Effects/EQ Eight/Sonora Air.adv");
        writePreset(eq, device("Eq8", "Sonora Air", ""));
        table = root.getChildFile("Samples/Tables/Test Table.wav");
        REQUIRE(table.getParentDirectory().createDirectory());
        REQUIRE(table.replaceWithText("stands in for a wavetable"));
    }
    ~Library() { root.deleteRecursively(); }
};
} // namespace

TEST_CASE("device presets (LIB-2): a library folder is indexed by device and folder; SONORA_REFERENCE_LIBRARY replaces the defaults")
{
    Library library;
    setVariable("SONORA_REFERENCE_LIBRARY", library.root.getFullPathName().toRawUTF8());
    const auto libraries = io::presetLibraries("/no/such/user/library", {});
    setVariable("SONORA_REFERENCE_LIBRARY", nullptr);
    REQUIRE(libraries.size() == 1);
    CHECK(libraries[0].path == library.root.getFullPathName().toStdString());

    const auto presets = io::scanDevicePresets(libraries);
    REQUIRE(presets.size() == 4); // no folder info, no Analog (not implemented), no rack
    CHECK(presets[0].deviceTag == "Eq8"); CHECK(presets[0].deviceName == "EQ Eight"); CHECK(presets[0].name == "Sonora Air");
    CHECK(presets[1].deviceTag == "InstrumentVector"); CHECK(presets[1].deviceName == "Sonora Wavetable");
    CHECK(presets[1].folder == "Bass"); CHECK(presets[1].name == "Sonora Test Bass");
    CHECK(presets[1].path == library.bass.getFullPathName().toStdString());
    CHECK(presets[2].name == "Plain Lead"); CHECK(presets[2].folder.empty()); // loose presets after the folders
    CHECK(presets[3].deviceTag == "MultiSampler"); CHECK(presets[3].folder == "Keys"); // mapped by the preset's root tag
    CHECK(io::devicePresetTag(library.bass) == "InstrumentVector");
    CHECK(io::devicePresetTag(library.root.getChildFile("Devices/Instruments/Wavetable/Ableton Folder Info/Properties.cfg")).empty());
}

TEST_CASE("device presets (LIB-2): loading keeps the preset's XML, fresh unique ids, the preset name; save, reload and undo")
{
    Library library;
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("minimal_set.xml"));
    REQUIRE(loaded); auto& song = *loaded.song;
    model::EditHistory history;
    const int firstPointee = song.nextPointeeId, firstDevice = song.nextDeviceId;

    auto preset = io::readDevicePreset(library.bass, song, {});
    REQUIRE(preset.error.empty());
    auto& d = preset.device;
    CHECK(d.type == "InstrumentVector");
    CHECK(d.displayName == "Sonora Test Bass");
    CHECK(d.id == firstDevice);
    const auto transpose = std::find_if(d.parameters.begin(), d.parameters.end(), [](const auto& p) { return p.path == "Voice_Oscillator1_Pitch_Transpose"; });
    REQUIRE(transpose != d.parameters.end());
    CHECK_NEAR(transpose->value, -7.0, 1e-9);
    CHECK(transpose->automationTargetId >= firstPointee);
    CHECK(d.enabledTargetId >= firstPointee);
    REQUIRE(d.sourceXml);
    const auto source = juce::parseXML(juce::String(*d.sourceXml));
    REQUIRE(source);
    CHECK(source->getChildByName("SpriteName1") != nullptr); // a leaf Sonora does not model is kept
    CHECK(source->getIntAttribute("Id", -1) == firstDevice);
    // Six pointees (On, Pointee, two parameters x automation + modulation): all different, from nextPointeeId on.
    const auto ids = pointeeIds(*source);
    CHECK(ids.size() == 6);
    CHECK(std::set<int>(ids.begin(), ids.end()).size() == ids.size());
    CHECK(*ids.begin() == firstPointee);
    CHECK(preset.pointeeEnd == firstPointee + 6);
    // The library-relative table points at the local library file.
    const auto* path = source->getChildByName("UserSprite1")->getChildByName("Value")->getChildByName("SampleRef")->getChildByName("FileRef")->getChildByName("Path");
    CHECK(path->getStringAttribute("Value") == library.table.getFullPathName());

    // An instrument cannot go on an audio track.
    int pointeeEnd = 0;
    auto audio = io::createAudioTrack(song, "2-Audio", 3, -1, -1, pointeeEnd);
    history.perform(song, model::insertTracks({audio}, pointeeEnd, "Insert Audio Track", 1));
    REQUIRE(song.tracks.size() == 2);
    CHECK(! model::canHoldDevice(song, 1, d));
    CHECK(! model::insertTrackDevice(song, 1, d).apply);

    // On the MIDI track it replaces the instrument, in one undo step.
    song.nextDeviceId = d.id + 1; song.nextPointeeId = preset.pointeeEnd; song.pointeeIdsAllocated = true;
    model::allocateDeviceIds(song, d);
    const auto before = song.tracks[0].devices.size();
    auto command = model::insertTrackDevice(song, 0, std::move(d));
    REQUIRE(command.apply);
    command.label = "Load Sonora Test Bass";
    history.perform(song, std::move(command));
    const auto& chain = song.tracks[0].devices;
    const auto found = std::find_if(chain.begin(), chain.end(), [&](const auto& x) { return x.id == firstDevice; });
    REQUIRE(found != chain.end());
    CHECK(found == chain.begin()); // the instrument slot

    // A second preset of the same device on the same set: still unique ids.
    auto second = io::readDevicePreset(library.loose, song, {});
    REQUIRE(second.error.empty());
    CHECK(second.device.id == song.nextDeviceId);
    CHECK(second.device.enabledTargetId >= preset.pointeeEnd);
    // The name comes from the file when UserName is empty; racks and devices Sonora does not run are refused.
    auto keys = io::readDevicePreset(library.sampler, song, {});
    REQUIRE(keys.error.empty()); CHECK(keys.device.displayName == "Sonora Keys");
    CHECK(! io::readDevicePreset(library.analog, song, {}).error.empty());
    CHECK(! io::readDevicePreset(library.rack, song, {}).error.empty());
    CHECK(! io::readDevicePreset(library.table, song, {}).error.empty());

    juce::TemporaryFile file(".als");
    auto saved = io::saveAls(song, file.getFile());
    REQUIRE(saved.ok);
    auto read = io::loadAls(file.getFile());
    REQUIRE(read);
    const auto& readChain = read.song->tracks[0].devices;
    const auto again = std::find_if(readChain.begin(), readChain.end(), [](const auto& x) { return x.type == "InstrumentVector"; });
    REQUIRE(again != readChain.end());
    CHECK(again->displayName == "Sonora Test Bass");
    const auto readTranspose = std::find_if(again->parameters.begin(), again->parameters.end(), [](const auto& p) { return p.path == "Voice_Oscillator1_Pitch_Transpose"; });
    REQUIRE(readTranspose != again->parameters.end());
    CHECK_NEAR(readTranspose->value, -7.0, 1e-9);
    CHECK(readTranspose->automationTargetId == transpose->automationTargetId);
    {
        std::string error;
        auto dom = io::readAlsDom(file.getFile(), error);
        REQUIRE(dom);
        const auto all = pointeeIds(*dom);
        bool unique = true;
        for (const int id : all) unique &= id < 0 || all.count(id) == 1;
        CHECK(unique);
        CHECK(dom->getChildByName("LiveSet")->getChildByName("NextPointeeId")->getIntAttribute("Value") > *all.rbegin());
        CHECK(dom->toString().contains("Sonora Test Table"));
    }

    REQUIRE(history.undo(song));
    CHECK(song.tracks[0].devices.size() == before);
    CHECK(std::none_of(song.tracks[0].devices.begin(), song.tracks[0].devices.end(), [](const auto& x) { return x.type == "InstrumentVector"; }));
}

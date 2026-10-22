#pragma once
// Sonora document model.
//
// Plain data structs that describe a loaded Live Set. This layer has NO JUCE and NO
// audio/UI dependencies so it can be unit-tested and reasoned about in isolation.
//
// Units (keep these consistent everywhere):
//   - Musical time is `Beats` = quarter notes, as stored in .als files.
//   - Gain is linear amplitude (1.0 == 0 dB), exactly like Live's Mixer/Volume "Manual" value.
//   - Pan is -1 (left) .. +1 (right).
//
// The field names intentionally mirror the .als XML (see docs/ALS_FORMAT.md) so the
// mapping in src/io/als/AlsReader.cpp stays obvious.

#include "core/MidiControllers.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace sonora::model
{

using Beats = double;
using TrackId = int; // `Id` attribute of a track element in the .als

enum class TrackKind
{
    Audio,
    Midi,
    Group,
    Return,
    Main
};

// Values match the integer stored in <WarpMode Value="..."/>.
enum class WarpMode
{
    Beats = 0,
    Tones = 1,
    Texture = 2,
    RePitch = 3,
    Complex = 4,
    ComplexPro = 6,
};

enum class DeviceKind
{
    Native,     // Ableton built-in device (Eq8, Reverb, OriginalSimpler, ...)
    PluginVst2, // <PluginDevice> + <VstPluginInfo>
    PluginVst3, // <PluginDevice> + <Vst3PluginInfo>
    PluginAu,   // <AuPluginDevice>
    Rack,       // Instrument/Audio/MIDI/Drum rack: has `chains`
    MaxForLive, // MxDevice*
};

// A MIDI mapping stored in the set (<KeyMidi> inside a parameter). channel == -1: not mapped.
// `channel` is the raw 0-based value from the file (0..15 == MIDI channels 1..16). Live uses the
// extra value 16 internally for rack macros: CC number == macro index, see Parameter::macroIndex.
struct MidiBinding
{
    int channel = -1;
    bool isNote = false;
    int number = 0;
    int mapMode = 0;  // Live's ControllerMapMode (0 absolute)
    bool mapped() const { return channel >= 0 && channel <= 15; }
    bool isMacro() const { return channel == 16 && ! isNote; }
};

struct Parameter
{
    std::string path; // e.g. "Bands.3/ParameterA/Freq" (relative to the device element)
    double value = 0.0;
    int automationTargetId = -1; // links to AutomationEnvelope::targetId
    MidiBinding midi;
    // Rack macro mapping: this parameter follows macro `macroIndex` (0..15) of the enclosing rack,
    // sweeping from rangeMin to rangeMax (the parameter's own units) as the macro goes 0..127.
    int macroIndex = -1;
    double rangeMin = 0.0, rangeMax = 0.0;
    bool booleanValue = false; // XML Manual is true/false (authored device defaults, UI-7)
    bool macroMappingEdited = false;
};

struct AutomationPoint
{
    Beats time = 0.0;
    double value = 0.0;
    // FloatEvent CurveControl1X/1Y/2X/2Y (ENG-11b): a cubic Bezier for the segment that starts at this point, in
    // segment-normalized coordinates (x: time 0..1, y: start value 0 .. next value 1). Equal x/y pairs (the 0.5
    // default) are a straight line.
    double x1 = 0.5, y1 = 0.5, x2 = 0.5, y2 = 0.5;
    bool curved() const { return (x1 - y1) * (x1 - y1) + (x2 - y2) * (x2 - y2) > 1e-18; }
};

// Shape of a segment starting at `start`: the normalized value (0 = start value, 1 = next value) at `fraction` (0..1)
// of its length. Linear for straight segments. Allocation free (audio thread safe).
double segmentShape(const AutomationPoint& start, double fraction);

struct AutomationEnvelope
{
    int targetId = -1; // <EnvelopeTarget><PointeeId Value=...>
    // Bool/Enum envelopes hold their value until the next point instead of ramping.
    // Float envelopes may carry bezier curve controls per point (AutomationPoint::x1...y2, ENG-11b).
    bool stepped = false;
    std::vector<AutomationPoint> points; // sorted by time; the first is usually at -63072000 (pre-roll value)
    std::string eventType = "FloatEvent"; // preserve BoolEvent / EnumEvent when saving edits
    int midiController = -1; // 0 pitch bend, 1 channel pressure, CC n at n+2; -1 parameter envelope
};

// Per-note expression stays outside MidiNote so realtime note capture retains a fixed-size value type.
struct NoteExpression
{
    int noteId = -1;
    int cc = -2; // -2 pitch (semitones), -1 pressure (0..127), 74 slide (0..127)
    struct Point
    {
        double time = 0, value = 0; // note-relative beats
        double x1 = 0, y1 = 0, x2 = 1, y2 = 1;
    };
    std::vector<Point> points;
    bool edited = false;
};

struct MidiNote
{
    Beats start = 0.0; // relative to the clip's content start (not the arrangement)
    Beats duration = 0.25;
    int pitch = 60;
    float velocity = 100.0f; // 0..127 (float in .als)
    float releaseVelocity = 64.0f;
    float probability = 1.0f;
    bool enabled = true;
    int noteId = -1;            // <MidiNoteEvent NoteId>: keeps per-note data of the file attached; -1 for new notes
    float velocityDeviation = 0.0f;
    uint32_t key = 0;           // COL-1: runtime identity, unique within its clip, never saved; 0 = not assigned yet
};

// COL-1: gives every unkeyed note (and every later duplicate of a key) a fresh key above the clip's highest.
// Deterministic for a given note order, idempotent, and keeps existing unique keys unchanged.
void assignNoteKeys(std::vector<MidiNote>& notes);

struct WarpMarker
{
    double seconds = 0.0;
    Beats beats = 0.0;
};

struct FileRef
{
    std::string absolutePath;  // as stored (may be stale/Windows path)
    std::string relativePath;  // relative to the project folder when relativePathType == 3
    int relativePathType = 0;
    std::string sourceDirectory{}; // base of project-relative paths, retained by undo / Save As
};

struct LoopSettings
{
    Beats loopStart = 0.0;
    Beats loopEnd = 4.0;
    Beats startRelative = 0.0;
    bool enabled = true;
};

struct Clip
{
    // Identity for editing and saving. `uid` is unique within a Song (assigned by the reader / by edits);
    // `xmlId` is the clip's position among the clip elements of its track's arrangement in the .als it came
    // from (-1 for clips created in Sonora). (Not the `Id` attribute: old sets repeat those.)
    int uid = -1;
    int takeId = -1; // source identity shared by a recorded lane clip and its main-lane comp copies
    int xmlId = -1;
    int cloneOfXmlId = -1;   // new clip copied from this one: the writer clones that element as a template...
    int cloneOfSessionSlot = -1; // template from a session slot instead of the arrangement
    int cloneOfTrackId = -1; // ...found in the arrangement of the track with this id (a copy may land on another track)
    // Immutable source fragment, shared by undo snapshots and copies. Keeps unknown clip metadata
    // available even after a save removes the source element or replaces an audio sample.
    std::shared_ptr<const std::string> sourceXml;
    bool stripSourceEnvelopes = false; // new empty/recorded clips must stay empty across later saves
    bool resetSourceMarkers = false; // fresh content uses its own end/hidden loop markers

    enum class Kind
    {
        Midi,
        Audio
    };

    Kind kind = Kind::Midi;
    std::string name;
    int colorIndex = 0;
    // TRK-5 linked (alias) clips: MIDI clips sharing a group >= 0 share their notes; an edit to one updates the others in the
    // same undo step. Sonora-only (<SonoraAlias Group>); ordinary copies are never linked.
    int linkGroup = -1;

    // Arrangement: position on the timeline. Session clips have time == 0.
    Beats time = 0.0;
    Beats currentStart = 0.0; // timeline start (arrangement) / clip start (session)
    Beats currentEnd = 4.0;   // timeline end
    LoopSettings loop;
    bool disabled = false;

    // Launch / clip settings (Live's Clip view): shared by MIDI and audio clips.
    int launchMode = 0;           // 0 Trigger, 1 Gate, 2 Toggle, 3 Repeat
    int launchQuantisation = 0;   // 0 Global, 1 None, 2 8 Bars, 3 4 Bars, 4 2 Bars, 5 1 Bar, 6 1/2, 7 1/2T, 8 1/4, 9 1/4T, 10 1/8, 11 1/8T, 12 1/16, 13 1/16T, 14 1/32
    bool legato = false;          // a launched clip continues at the previous clip's position
    // <ScaleInformation Root/Name> and <IsInKey> (UI-30b): Name indexes model::kScales (0 Major, 1 Minor ...).
    int scaleRoot = 0, scaleName = 0;
    bool inKey = false;
    double velocityAmount = 0.0;  // how much launch velocity scales the clip volume (0..1)
    int timeSigNumerator = 4, timeSigDenominator = 4; // the clip's own signature (display / editing grid)

    // --- MIDI clips
    std::vector<MidiNote> notes;
    std::vector<NoteExpression> noteExpressions;
    bool noteExpressionsEdited = false;

    // --- Audio clips
    FileRef sample;
    double sampleRate = 0.0;
    int64_t lengthSamples = 0;
    bool warped = false;
    bool tempoAnalyzed = false; // UI-41: preserve a detected/manual source tempo when Warp is toggled again
    WarpMode warpMode = WarpMode::Beats;
    std::vector<WarpMarker> warpMarkers;
    double gain = 1.0; // <SampleVolume>
    int pitchCoarse = 0;   // semitones
    double pitchFine = 0.0; // cents

    // Warp mode settings (ENG-5). Defaults are Live's.
    int transientResolution = 6;   // Beats mode "Preserve": 0 1 Bar, 1 1/2, 2 1/4, 3 1/8, 4 1/16, 5 1/32, 6 Transients
    int transientLoopMode = 2;     // Beats mode: 0 Off, 1 Loop Forward, 2 Loop Back-and-Forth
    double transientEnvelope = 100.0; // 100 = slices ring out, lower values shorten them
    double granularityTones = 30.0;
    double granularityTexture = 65.0;
    double fluctuationTexture = 25.0;
    double complexProFormants = 100.0; // Complex Pro: formant preservation (0..100)
    double complexProEnvelope = 128.0; // Complex Pro: envelope (8..256)
    // Sample box switches (<Fade>, <Ram>, <HiQ>), saved with the set. Sonora always decodes into RAM and uses one
    // interpolator, so Ram and HiQ are stored but do not change playback (TODO(ENG-16): HiQ / low-quality resampler).
    bool clipFade = true; // Session clips: short edge fades
    bool ram = false;
    bool hiQ = true;
    // Clip fades (<Fades>): lengths in beats from the clip's left / right edge. 0 with the *Default flag set means Live's
    // short automatic edge fade; a longer fade clears the flag.
    double fadeIn = 0.0, fadeOut = 0.0;
    double fadeInCurve = 0.0, fadeOutCurve = 0.0; // bounded curve skew (-1..1)
    bool fadeInDefault = true, fadeOutDefault = true;

    std::vector<AutomationEnvelope> envelopes; // clip envelopes: time in clip beats, played while the clip plays (ALS-9)
};

struct PluginInfo
{
    std::string name;
    std::string vendor;
    std::string path; // VST2 only: path on the machine that saved the set

    int64_t vst2UniqueId = 0;
    std::array<uint32_t, 4> vst3Uid{}; // <Uid><Fields.0..3>
    uint32_t auType = 0, auSubType = 0, auManufacturer = 0;

    bool isInstrument = false;

    // Opaque plugin state. VST2: chunk/fxp body. VST3: IComponent state. AU: ClassInfo plist.
    std::vector<uint8_t> state;
    std::vector<uint8_t> controllerState; // VST3 only (IEditController state)
};

// One sample zone of a Simpler / Sampler (<MultiSamplePart>).
struct SamplePart
{
    std::shared_ptr<const std::string> sourceXml;
    std::string name;
    int keyMin = 0, keyMax = 127;
    int velMin = 1, velMax = 127;
    int rootKey = 60;
    double detuneCents = 0.0;
    double pan = 0.0;
    double volume = 1.0; // linear
    int64_t sampleStart = 0, sampleEnd = 0; // sample frames; end == 0: whole file
    // <SustainLoop>: mode 0 off, 1 forward, 2 back-and-forth. Positions are sample frames.
    int64_t loopStart = 0, loopEnd = 0;
    int loopMode = 0;
    int64_t loopCrossfade = 0;
    FileRef sample;
    double sampleRate = 0.0;
    int64_t lengthSamples = 0;
};

// Sidechain input of a native dynamics device (<SideChain>). Where the detector listens.
struct Sidechain
{
    bool on = false;
    int sourceTrackId = -1;       // <Target Value="AudioIn/Track.N/PreFxOut"/>: N
    enum class Tap { PreFx, PostFx, PostMixer } tap = Tap::PostFx;
    double gain = 1.0;            // SideChain/RoutedInput/Volume (linear)
    bool edited = false;
    bool routed() const { return on && sourceTrackId >= 0; }
};

// A hosted plugin's parameter as Live stores it (<PluginFloatParameter>): lets automation and
// clip envelopes target plugin parameters (ROADMAP PLG-7). Values are normalised 0..1.
struct PluginParam
{
    int parameterId = -1; // <ParameterId>: the plugin's own parameter id
    std::string name;
    double value = 0.0;
    int automationTargetId = -1;
    bool valueEdited = false; // AGT-7b: patch only explicit/captured live writes; preserve unsupported imported manuals
};

// DEV-18d: immutable native-device content, separate from editable scalar controls.
// Source bytes are kept for lossless import/undo; engines decode off the audio thread.
struct NativeContent
{
    static constexpr size_t maxBytes = 64 * 1024 * 1024;
    std::string format;
    std::string sourceDirectory;
    std::vector<uint8_t> data, controllerData;
};

struct Device
{
    DeviceKind kind = DeviceKind::Native;
    std::string type;        // XML element name, e.g. "Eq8", "PluginDevice", "InstrumentGroupDevice"
    std::string displayName; // user name, plugin name, or catalog name
    int id = -1;
    std::shared_ptr<const std::string> sourceXml; // device insertion and undo after save (UI-7)
    std::string sourceDirectory; // base of unparsed sample refs in retained device XML
    std::string maxPatch; // Max for Live: .amxd file name without extension ("Swiss Army Meter"); empty otherwise
    std::string maxPatchPath; // Max for Live: absolute .amxd path (FileRef/Path); authored inserts use a copy found on disk
    bool enabled = true;
    int enabledTargetId = -1; // <On>/<AutomationTarget>: the device on/off switch can be automated

    std::vector<Parameter> parameters; // native devices only (flattened "Manual" values)
    std::map<std::string, double> plain; // native devices: plain <X Value="n"/> leaves under <Globals> (PlaybackMode, NumVoices...)
    std::vector<SamplePart> sampleParts; // Simpler / Sampler zones
    bool samplePartsEdited = false, chainsEdited = false; // author only changed containers, retaining unknown device XML
    Sidechain sidechain;                 // dynamics devices with an external sidechain input
    std::vector<PluginParam> pluginParams; // plugin devices: parameters Live knows about (for automation)
    std::optional<PluginInfo> plugin;  // plugin devices only
    std::shared_ptr<const NativeContent> nativeContent; // Sonora extension; never read by audio callbacks
    bool nativeContentEdited = false;

    struct Chain
    {
        std::shared_ptr<const std::string> sourceXml;
        std::string name;
        std::vector<Device> devices;
        double volume = 1.0;
        double pan = 0.0;
        bool speakerOn = true;
        // Drum Rack pads only. receivingNote is a MIDI note (already converted from
        // Live's "128 - note" encoding, see docs/ALS_FORMAT.md). -1 == receives everything.
        int receivingNote = -1;
        int sendingNote = 60;
        int chokeGroup = 0; // Drum Rack: 0 none, 1..16
        // Zones (Instrument / Audio Effect Racks): the chain only receives notes inside the key and
        // velocity ranges, and is only active while the rack's chain selector is inside [selMin, selMax].
        int keyMin = 0, keyMax = 127;
        int velMin = 1, velMax = 127;
        int selMin = 0, selMax = 127;
    };
    std::vector<Chain> chains; // racks only
    std::vector<std::string> macroNames; // racks: MacroDisplayNames.0..15
    int numVisibleMacros = 8;

    // Convenience lookup for native parameters by path; returns fallback when absent.
    double param(const std::string& path, double fallback) const
    {
        for (const auto& p : parameters)
            if (p.path == path)
                return p.value;
        return fallback;
    }
};

struct Mixer
{
    bool sendsEdited = false;
    std::vector<std::shared_ptr<const std::string>> sendSourceXml;
    double volume = 1.0;
    double pan = 0.0;
    bool speakerOn = true; // track activator ("mute" is speakerOn == false)
    bool solo = false;
    std::vector<double> sends; // linear gain per return track, in return order
    int volumeTargetId = -1;
    int panTargetId = -1;
    int speakerTargetId = -1;         // track activator
    int tempoTargetId = -1;           // main track only: automation target of the song tempo
    int timeSignatureTargetId = -1;  // static signature automation base follows manual signature edits
    std::vector<int> sendTargetIds;   // parallel to `sends`
    MidiBinding volumeMidi, panMidi, speakerMidi;
    std::vector<MidiBinding> sendMidi; // parallel to `sends`
};

struct ClipSlot
{
    std::optional<Clip> clip;
};

struct TakeLane
{
    int id = -1, height = 51;
    std::string name, annotation;
    bool audition = false;
    std::vector<Clip> clips;
    std::shared_ptr<const std::string> sourceXml;
};

struct Track
{
    TrackKind kind = TrackKind::Audio;
    TrackId id = -1;
    std::string name;
    bool nameIsAutomatic = false; // empty ALS Name/UserName: cached EffectiveName is not a fixed label
    int colorIndex = 0;
    TrackId groupId = -1; // parent GroupTrack id, -1 when top level
    std::string outputRouting = "AudioOut/Main"; // <AudioOutputRouting/Target>: Main / Master / GroupTrack / None / External/...
    bool outputRoutingEdited = false;             // the writer patches the routing element only after an edit
    // Authored <GroupTrack> (or other track) element for a track that is not in the source file yet (a new group).
    std::shared_ptr<const std::string> sourceXml;
    double trackDelayMs = 0.0;                    // <TrackDelay> (positive = later)
    bool folded = false;
    std::string inputRouting; // AudioInputRouting or MidiInputRouting Target, according to track kind
    std::string inputUpperDisplay, inputLowerDisplay;
    int monitoring = 1; // MainSequencer/MonitoringEnum: 0 In, 1 Auto, 2 Off
    bool inputRoutingPresent = false, monitoringPresent = false;
    bool inputRoutingEdited = false, monitoringEdited = false; // author missing fields only after an edit

    std::array<int, midiControllerTargetCount> midiControllerTargets = [] {
        std::array<int, midiControllerTargetCount> ids{}; ids.fill(-1); return ids;
    }();
    bool midiControllersPresent = false, midiControllersEdited = false;
    Mixer mixer;
    std::vector<Device> devices;
    std::vector<ClipSlot> slots;        // Session view, one per scene
    std::vector<Clip> arrangementClips; // Arrangement view, sorted by time
    std::vector<TakeLane> takeLanes; // ENG-9b: preserved passes, separate from audible main lane
    std::shared_ptr<const std::string> takeLanesSourceXml;
    bool takeLanesNeedPatch = false; // both apply and undo set this; untouched loaded XML stays untouched
    int nextTakeLaneId = 0; // monotonic across undo
    int nextTakeId = 1, recorderTakeCounter = 0;
    bool takeCounterAdvanced = false;
    std::vector<AutomationEnvelope> automation;
    // Sonora-only (<SonoraTrack>, docs/ALS_FORMAT.md): hidden tracks leave the views but keep playing; deactivated tracks
    // keep their content and devices in the model but the engine builds them empty (no plugins loaded, no CPU).
    bool hidden = false;
    bool deactivated = false;
    bool frozen = false;
    std::vector<Clip> frozenArrangement;
    std::vector<ClipSlot> frozenSlots;
};

struct Scene
{
    std::string name;
    int colorIndex = -1;
    std::optional<double> tempo;
};

struct Locator
{
    Beats time = 0.0;
    std::string name;
};

struct Song
{
    struct SavedClipLocation
    {
        int trackId = -1;
        int ordinal = -1;
        int sessionSlot = -1; // -1 = arrangement
    };
    // Bindings of stable editing uids to the current source file. Refreshed on save, never undone.
    std::map<int, SavedClipLocation> savedClips;
    int nextPointeeId = 1; // automation/device/controller target allocator, never undone
    bool pointeeIdsAllocated = false;
    int nextDeviceId = 0; // insertion ids stay unique even when a saved device is undone
    std::string sourcePath; // absolute path of the .als
    std::string creator;    // e.g. "Ableton Live 12.1.5"
    std::string schemaVersion;

    double tempo = 120.0;
    int timeSigNumerator = 4;
    int timeSigDenominator = 4;

    std::vector<Track> tracks;  // audio/midi/group tracks in display order
    std::vector<Track> returns; // return tracks
    Track main;                 // a.k.a. master
    std::vector<std::shared_ptr<const std::string>> sendPreXml;
    bool sendsPreEdited = false;

    std::vector<Scene> scenes;
    std::vector<Locator> locators;

    Beats loopStart = 0.0;
    Beats loopLength = 16.0;
    bool loopOn = false;
    bool punchIn = false, punchOut = false;
    bool punchInPresent = false, punchOutPresent = false, punchEdited = false;
    Beats currentTime = 0.0;
    // Session launch quantisation (<GlobalQuantisation>): 0 none, 1 8 bars, 2 4 bars, 3 2 bars, 4 1 bar,
    // 5 1/2, 6 1/2T, 7 1/4, 8 1/4T, 9 1/8, 10 1/8T, 11 1/16, 12 1/16T, 13 1/32.
    int globalQuantisation = 4;
    // Set scale (<ScaleInformation Root/Name>, Scale Mode <InKey>; UI-30b). Name indexes model::kScales. New clips
    // take it over; the editors highlight / fold / fit to it.
    int scaleRoot = 0, scaleName = 0;
    bool inKey = false;

    // Human-readable notes about things we could not represent (unsupported devices...).
    std::vector<std::string> warnings;

    // Authored <MidiClip> element used to create the first MIDI clip of a set that has none to copy
    // (a new project). Set by the application; never saved by itself.
    std::shared_ptr<const std::string> midiClipTemplate;
};

std::string displayTrackName(const Song&, int controlIndex); // current order, automatic names and leading # templates
const char* toString(TrackKind);
int allocateClipUid(); // one allocator for background readers and message-thread edits
// COL-2: later allocations start at `base` or above. A collab guest allocates from its own range, so clip UIDs created
// on different machines of one session never collide.
void reserveClipUidsFrom(int base);

// COL-1 (ADR 0009): clip UIDs allocated on this thread while the scope is alive. Record mode keeps the values a
// sequencer allocated while staging an operation; Replay mode hands exactly those values back on a replica, so
// every copy of the song gets the same identities. Other threads (background readers) are unaffected.
class ClipUidScope
{
public:
    enum class Mode { Record, Replay };
    explicit ClipUidScope(Mode mode, std::vector<int> replay = {});
    ~ClipUidScope();
    ClipUidScope(const ClipUidScope&) = delete;
    ClipUidScope& operator=(const ClipUidScope&) = delete;

    const std::vector<int>& uids() const { return uids_; }
    size_t consumed() const { return next_; }
    bool overran() const { return overrun_; }
    // Replay consumed every recorded value and needed no extra one.
    bool replayedExactly() const { return mode_ == Mode::Replay && next_ == uids_.size() && ! overrun_; }

private:
    friend int allocateClipUid();
    Mode mode_;
    std::vector<int> uids_;
    size_t next_ = 0;
    bool overrun_ = false;
    ClipUidScope* previous_ = nullptr;
};
const char* toString(DeviceKind);
const char* toString(WarpMode);

} // namespace sonora::model

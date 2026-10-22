#include "io/als/AlsNewSet.h"

#include <tuple>

namespace sonora::io
{
namespace
{
using juce::XmlElement;

struct Ids
{
    int next = 1;
    int take() { return next++; }
};

XmlElement* value(XmlElement& parent, const char* tag, const juce::String& v)
{
    auto* e = parent.createNewChildElement(tag);
    e->setAttribute("Value", v);
    return e;
}

// Live rejects "1.000000000000" for integer-typed nodes, so print numbers the way Live does: no padding.
juce::String num(double v)
{
    auto text = juce::String(v, 12);
    if (text.containsChar('.'))
        text = text.trimCharactersAtEnd("0").trimCharactersAtEnd(".");
    return text.isEmpty() ? juce::String("0") : text;
}

// Automatable parameter: <Tag><LomId/><Manual/><MidiControllerRange/><AutomationTarget/>[<ModulationTarget/>]</Tag>
XmlElement* timeable(XmlElement& parent, const char* tag, double manual, double min, double max, Ids& ids, bool modulation = true)
{
    auto* e = parent.createNewChildElement(tag);
    value(*e, "LomId", "0");
    value(*e, "Manual", num(manual));
    auto* range = e->createNewChildElement("MidiControllerRange");
    value(*range, "Min", num(min));
    value(*range, "Max", num(max));
    value(*e->createNewChildElement("AutomationTarget"), "LockEnvelope", "0");
    e->getChildByName("AutomationTarget")->setAttribute("Id", ids.take());
    if (modulation)
    {
        auto* m = e->createNewChildElement("ModulationTarget");
        m->setAttribute("Id", ids.take());
        value(*m, "LockEnvelope", "0");
    }
    return e;
}

XmlElement* timeableBool(XmlElement& parent, const char* tag, bool manual, Ids& ids)
{
    auto* e = parent.createNewChildElement(tag);
    value(*e, "LomId", "0");
    value(*e, "Manual", manual ? "true" : "false");
    auto* target = e->createNewChildElement("AutomationTarget");
    target->setAttribute("Id", ids.take());
    value(*target, "LockEnvelope", "0");
    auto* thresholds = e->createNewChildElement("MidiCCOnOffThresholds");
    value(*thresholds, "Min", "64");
    value(*thresholds, "Max", "127");
    return e;
}

XmlElement* pointeeId(XmlElement& parent, const char* tag, Ids& ids)
{
    auto* e = parent.createNewChildElement(tag);
    e->setAttribute("Id", ids.take());
    value(*e, "LockEnvelope", "0");
    return e;
}

void routing(XmlElement& chain, const char* tag, const char* target, const char* upper, const char* lower)
{
    auto* r = chain.createNewChildElement(tag);
    value(*r, "Target", target);
    value(*r, "UpperDisplayString", upper);
    value(*r, "LowerDisplayString", lower);
    auto* mpe = r->createNewChildElement("MpeSettings");
    value(*mpe, "ZoneType", "0");
    value(*mpe, "FirstNoteChannel", "1");
    value(*mpe, "LastNoteChannel", "15");
    value(*r, "MpePitchBendUsesTuning", "true");
}

// Members shared by every device-like object (mixer, sequencers).
void deviceHeader(XmlElement& e, Ids& ids, bool withOn = true)
{
    value(e, "LomId", "0");
    value(e, "LomIdView", "0");
    value(e, "IsExpanded", "true");
    value(e, "BreakoutIsExpanded", "false");
    if (withOn)
        timeableBool(e, "On", true, ids);
    value(e, "ModulationSourceCount", "0");
    e.createNewChildElement("ParametersListWrapper")->setAttribute("LomId", 0);
    e.createNewChildElement("Pointee")->setAttribute("Id", ids.take());
    value(e, "LastSelectedTimeableIndex", "0");
    value(e, "LastSelectedClipEnvelopeIndex", "0");
    e.createNewChildElement("LastPresetRef")->createNewChildElement("Value");
    e.createNewChildElement("LockedScripts");
    value(e, "IsFolded", "false");
    value(e, "ShouldShowPresetName", "false");
    value(e, "UserName", "");
    value(e, "Annotation", "");
    e.createNewChildElement("SourceContext")->createNewChildElement("Value");
    value(e, "MpePitchBendUsesTuning", "true");
    value(e, "ViewData", "{}");
}

enum class Kind { Midi, Audio, Return, Main, PreHear, Group };

void buildMixer(XmlElement& chain, Kind kind, int sends, Ids& ids)
{
    auto& mixer = *chain.createNewChildElement("Mixer");
    deviceHeader(mixer, ids);
    auto* sendList = mixer.createNewChildElement("Sends");
    // Returns carry a holder per return too, disabled until the user enables return-to-return sends (as Live writes them).
    if (kind == Kind::Midi || kind == Kind::Audio || kind == Kind::Group || kind == Kind::Return)
        for (int i = 0; i < sends; ++i)
        {
            auto* holder = sendList->createNewChildElement("TrackSendHolder");
            holder->setAttribute("Id", i);
            timeable(*holder, "Send", 0.0003162277571, 0.0003162277571, 1, ids);
            value(*holder, "EnabledByUser", kind == Kind::Return ? "false" : "true");
        }
    timeableBool(mixer, "Speaker", true, ids);
    value(mixer, "SoloSink", "false");
    value(mixer, "PanMode", "0");
    timeable(mixer, "Pan", 0, -1, 1, ids);
    timeable(mixer, "SplitStereoPanL", -1, -1, 1, ids);
    timeable(mixer, "SplitStereoPanR", 1, -1, 1, ids);
    timeable(mixer, "Volume", 1, 0.0003162277571, 1.99526238, ids);
    value(mixer, "ViewStateSessionTrackWidth", "93");
    timeable(mixer, "CrossFadeState", 1, 0, 2, ids, false);
    mixer.createNewChildElement("SendsListWrapper")->setAttribute("LomId", 0);
    if (kind == Kind::Main)
    {
        timeable(mixer, "Tempo", 120, 60, 200, ids);
        timeable(mixer, "TimeSignature", 201, 0, 494, ids, false);
        timeable(mixer, "GlobalGrooveAmount", 100, 0, 131.25, ids);
        timeable(mixer, "CrossFade", 0, -1, 1, ids);
        value(mixer, "TempoAutomationViewBottom", "60");
        value(mixer, "TempoAutomationViewTop", "200");
    }
}

void arrangerEvents(XmlElement& parent, const char* tag)
{
    auto* holder = parent.createNewChildElement(tag);
    auto* arranger = holder->createNewChildElement("ArrangerAutomation");
    arranger->createNewChildElement("Events");
    auto* view = arranger->createNewChildElement("AutomationTransformViewState");
    value(*view, "IsTransformPending", "false");
    view->createNewChildElement("TimeAndValueTransforms");
}

XmlElement* sequencer(XmlElement& chain, const char* tag, Kind kind, int scenes, Ids& ids, bool freeze)
{
    auto& seq = *chain.createNewChildElement(tag);
    deviceHeader(seq, ids);
    auto* slots = seq.createNewChildElement("ClipSlotList");
    for (int i = 0; i < scenes; ++i)
    {
        auto* slot = slots->createNewChildElement("ClipSlot");
        slot->setAttribute("Id", i);
        value(*slot, "LomId", "0");
        slot->createNewChildElement("ClipSlot")->createNewChildElement("Value");
        value(*slot, "HasStop", "true");
    }
    value(seq, "MonitoringEnum", kind == Kind::Midi ? "1" : "1");
    value(seq, "KeepRecordMonitoringLatency", "true");
    if (kind == Kind::Midi && ! freeze)
        arrangerEvents(seq, "ClipTimeable");
    else
    {
        arrangerEvents(seq, "Sample");
        for (const char* name : {"VolumeModulationTarget", "TranspositionModulationTarget", "TransientEnvelopeModulationTarget",
                                 "GrainSizeModulationTarget", "FluxModulationTarget", "SampleOffsetModulationTarget",
                                 "ComplexProFormantsModulationTarget", "ComplexProEnvelopeModulationTarget"})
            pointeeId(seq, name, ids);
        value(seq, "PitchViewScrollPosition", "-1073741824");
        value(seq, "SampleOffsetModulationScrollPosition", "-1073741824");
    }
    auto* recorder = seq.createNewChildElement("Recorder");
    value(*recorder, "IsArmed", "false");
    value(*recorder, "TakeCounter", "0");
    return &seq;
}

void buildTrack(XmlElement& tracks, Kind kind, int trackId, const juce::String& name, int colour, int scenes, int sends, Ids& ids)
{
    const char* tag = kind == Kind::Midi ? "MidiTrack" : kind == Kind::Audio ? "AudioTrack" : kind == Kind::Return ? "ReturnTrack"
                      : kind == Kind::Group ? "GroupTrack" : kind == Kind::Main ? "MainTrack" : "PreHearTrack";
    const bool outputTrack = kind == Kind::Main || kind == Kind::PreHear;
    auto* t = tracks.createNewChildElement(tag);
    if (! outputTrack)
        t->setAttribute("Id", trackId);
    t->setAttribute("SelectedToolPanel", 7);
    t->setAttribute("SelectedTransformationName", "");
    t->setAttribute("SelectedGeneratorName", "");
    value(*t, "LomId", "0");
    value(*t, "LomIdView", "0");
    value(*t, "IsContentSelectedInDocument", "false");
    value(*t, "PreferredContentViewMode", "0");
    auto* delay = t->createNewChildElement("TrackDelay");
    value(*delay, "Value", "0");
    value(*delay, "IsValueSampleBased", "false");
    auto* n = t->createNewChildElement("Name");
    value(*n, "EffectiveName", name);
    const auto base = kind == Kind::Midi ? juce::String("MIDI") : kind == Kind::Audio ? juce::String("Audio") : juce::String("Group");
    const auto prefix = name.upToFirstOccurrenceOf("-", false, false);
    const bool automatic = (kind == Kind::Midi || kind == Kind::Audio || kind == Kind::Group)
        && (name == base || (prefix.isNotEmpty() && prefix.containsOnly("0123456789") && name == prefix + "-" + base));
    value(*n, "UserName", automatic || kind == Kind::Return || outputTrack ? juce::String() : name);
    value(*n, "Annotation", "");
    value(*n, "MemorizedFirstClipName", "");
    value(*t, "Color", juce::String(colour));
    t->createNewChildElement("AutomationEnvelopes")->createNewChildElement("Envelopes");
    value(*t, "TrackGroupId", "-1");
    value(*t, "TrackUnfolded", outputTrack ? "false" : "true");
    for (const char* wrapper : {"DevicesListWrapper", "ClipSlotsListWrapper", "ArrangementClipsListWrapper", "TakeLanesListWrapper"})
        if (kind != Kind::Group || std::string(wrapper) == "DevicesListWrapper" || std::string(wrapper) == "ClipSlotsListWrapper")
            t->createNewChildElement(wrapper)->setAttribute("LomId", 0);
    value(*t, "ViewData", "{}");
    auto* lanes = t->createNewChildElement("TakeLanes");
    lanes->createNewChildElement("TakeLanes");
    value(*lanes, "AreTakeLanesFolded", "true");
    value(*t, "LinkedTrackGroupId", "-1");
    if (kind == Kind::Group)
    {
        // one GroupTrackSlot per scene (the launch buttons of a group)
        auto* groupSlots = t->createNewChildElement("Slots");
        for (int i = 0; i < scenes; ++i)
        {
            auto* slot = groupSlots->createNewChildElement("GroupTrackSlot");
            slot->setAttribute("Id", i);
            value(*slot, "LomId", "0");
        }
    }
    if (kind == Kind::Midi || kind == Kind::Audio)
    {
        value(*t, "SavedPlayingSlot", "-1");
        value(*t, "SavedPlayingOffset", "0");
        value(*t, "Freeze", "false");
        value(*t, "PostProcessFreezeClips", "0");
    }

    auto& chain = *t->createNewChildElement("DeviceChain");
    auto* automationLanes = chain.createNewChildElement("AutomationLanes");
    auto* laneList = automationLanes->createNewChildElement("AutomationLanes");
    auto* lane = laneList->createNewChildElement("AutomationLane");
    lane->setAttribute("Id", 0);
    value(*lane, "SelectedDevice", "0");
    value(*lane, "SelectedEnvelope", "0");
    value(*lane, "IsContentSelectedInDocument", "false");
    value(*lane, "LaneHeight", "68");
    value(*automationLanes, "AreAdditionalAutomationLanesFolded", "false");
    auto* chooser = chain.createNewChildElement("ClipEnvelopeChooserViewState");
    value(*chooser, "SelectedDevice", "0");
    value(*chooser, "SelectedEnvelope", "0");
    value(*chooser, "PreferModulationVisible", "false");
    routing(chain, "AudioInputRouting", kind == Kind::Main ? "AudioOut/External/S0" : "AudioIn/External/S0",
            kind == Kind::Main ? "Ext. Out" : "Ext. In", "1/2");
    routing(chain, "MidiInputRouting", "MidiIn/External.All/-1", "Ext: All Ins", "");
    routing(chain, "AudioOutputRouting", outputTrack ? "AudioOut/External/S0" : "AudioOut/Main",
            outputTrack ? "Ext. Out" : "Master", outputTrack ? "1/2" : "");
    routing(chain, "MidiOutputRouting", "MidiOut/None", "None", "");
    buildMixer(chain, kind, sends, ids);
    if (kind == Kind::Midi || kind == Kind::Audio)
    {
        sequencer(chain, "MainSequencer", kind, scenes, ids, false);
        sequencer(chain, "FreezeSequencer", Kind::Audio, scenes, ids, true);
    }
    if (kind == Kind::Main)
        sequencer(*chain.createNewChildElement("FreezeSequencer"), "AudioSequencer", Kind::Audio, scenes, ids, true)->setAttribute("Id", 0);
    auto* devices = chain.createNewChildElement("DeviceChain");
    devices->createNewChildElement("Devices");
    devices->createNewChildElement("SignalModulations");
    if (kind == Kind::Return || kind == Kind::Group)
        sequencer(chain, "FreezeSequencer", Kind::Audio, 0, ids, true); // returns and groups keep an empty freeze slot list

    if (kind == Kind::Midi)
    {
        value(*t, "ReWireDeviceMidiTargetId", "0");
        value(*t, "PitchbendRange", "96");
        value(*t, "IsTuned", "true");
        value(*t, "ControllerLayoutRemoteable", "0");
        auto* custom = t->createNewChildElement("ControllerLayoutCustomization");
        value(*custom, "PitchClassSource", "0");
        value(*custom, "OctaveSource", "2");
        value(*custom, "KeyNoteTarget", "60");
        value(*custom, "StepSize", "1");
        value(*custom, "OctaveEvery", "12");
        value(*custom, "AllowedKeys", "0");
        value(*custom, "FillerKeysMapTo", "0");
    }
    else if (kind == Kind::Audio)
        value(*t, "IsTuned", "true");
}

void grid(XmlElement& parent, const char* tag, bool snap)
{
    auto* g = parent.createNewChildElement(tag);
    value(*g, "FixedNumerator", "1");
    value(*g, "FixedDenominator", "16");
    value(*g, "GridIntervalPixel", "20");
    value(*g, "Ntoles", "2");
    value(*g, "SnapToGrid", snap ? "true" : "false");
    value(*g, "Fixed", "false");
}
} // namespace

std::unique_ptr<XmlElement> createNewSetDom(const NewSetOptions& o)
{
    auto root = std::make_unique<XmlElement>("Ableton");
    root->setAttribute("MajorVersion", "5");
    root->setAttribute("MinorVersion", "12.0_12120");
    root->setAttribute("SchemaChangeCount", "4");
    root->setAttribute("Creator", "Sonora");
    root->setAttribute("Revision", "");
    auto& live = *root->createNewChildElement("LiveSet");
    Ids ids;
    ids.next = 20; // low ids are kept free, like Live's own sets
    value(live, "NextPointeeId", "0"); // patched at the end
    value(live, "OverwriteProtectionNumber", "3077");
    value(live, "LomId", "0");
    value(live, "LomIdView", "0");

    auto& tracks = *live.createNewChildElement("Tracks");
    int id = 0;
    static const int midiColours[] = {1, 4, 1, 4};
    static const int audioColours[] = {5, 21, 5, 21};
    static const int returnColours[] = {17, 7, 17, 7};
    for (int i = 0; i < o.midiTracks; ++i, ++id)
        buildTrack(tracks, Kind::Midi, id, juce::String(i + 1) + "-MIDI", midiColours[i % 4], o.scenes, o.returnTracks, ids);
    for (int i = 0; i < o.audioTracks; ++i, ++id)
        buildTrack(tracks, Kind::Audio, id, juce::String(o.midiTracks + i + 1) + "-Audio", audioColours[i % 4], o.scenes, o.returnTracks, ids);
    for (int i = 0; i < o.returnTracks; ++i, ++id)
        buildTrack(tracks, Kind::Return, id, juce::String::charToString((juce::juce_wchar) ('A' + i)) + "-Return", returnColours[i % 4], 0, 0, ids);
    buildTrack(live, Kind::Main, 0, "Main", 16, o.scenes, 0, ids);
    buildTrack(live, Kind::PreHear, 0, "Master", -1, 0, 0, ids);
    // Live stores the initial tempo and time signature as automation envelopes of the Main track.
    if (auto* mainTrack = live.getChildByName("MainTrack"))
    {
        auto* mixer = mainTrack->getChildByName("DeviceChain")->getChildByName("Mixer");
        auto* envelopes = mainTrack->getChildByName("AutomationEnvelopes")->getChildByName("Envelopes");
        int envelopeId = 0;
        for (const auto& [parameter, eventTag, initial] : {std::tuple<const char*, const char*, juce::String>{"TimeSignature", "EnumEvent", "201"},
                                                              {"Tempo", "FloatEvent", num(o.tempo)}})
        {
            auto* envelope = envelopes->createNewChildElement("AutomationEnvelope");
            envelope->setAttribute("Id", envelopeId++);
            envelope->createNewChildElement("EnvelopeTarget")->createNewChildElement("PointeeId")
                ->setAttribute("Value", mixer->getChildByName(parameter)->getChildByName("AutomationTarget")->getIntAttribute("Id"));
            auto* automation = envelope->createNewChildElement("Automation");
            auto* events = automation->createNewChildElement("Events");
            auto* event = events->createNewChildElement(eventTag);
            event->setAttribute("Id", 0);
            event->setAttribute("Time", "-63072000");
            event->setAttribute("Value", initial);
            auto* view = automation->createNewChildElement("AutomationTransformViewState");
            value(*view, "IsTransformPending", "false");
            view->createNewChildElement("TimeAndValueTransforms");
        }
    }

    auto* sendsPre = live.createNewChildElement("SendsPre");
    for (int i = 0; i < o.returnTracks; ++i)
    {
        auto* pre = sendsPre->createNewChildElement("SendPreBool");
        pre->setAttribute("Id", i);
        pre->setAttribute("Value", "false");
    }
    auto* scenes = live.createNewChildElement("Scenes");
    for (int i = 0; i < o.scenes; ++i)
    {
        auto* scene = scenes->createNewChildElement("Scene");
        scene->setAttribute("Id", i);
        auto* follow = scene->createNewChildElement("FollowAction");
        value(*follow, "FollowTime", "4");
        value(*follow, "IsLinked", "true");
        value(*follow, "LoopIterations", "1");
        value(*follow, "FollowActionA", "4");
        value(*follow, "FollowActionB", "0");
        value(*follow, "FollowChanceA", "100");
        value(*follow, "FollowChanceB", "0");
        value(*follow, "JumpIndexA", "0");
        value(*follow, "JumpIndexB", "0");
        value(*follow, "FollowActionEnabled", "false");
        value(*scene, "Name", "");
        value(*scene, "Annotation", "");
        value(*scene, "Color", "-1");
        value(*scene, "Tempo", num(o.tempo));
        value(*scene, "IsTempoEnabled", "false");
        value(*scene, "TimeSignatureId", "201");
        value(*scene, "IsTimeSignatureEnabled", "false");
        value(*scene, "LomId", "0");
        scene->createNewChildElement("ClipSlotsListWrapper")->setAttribute("LomId", 0);
    }

    auto* transport = live.createNewChildElement("Transport");
    value(*transport, "PhaseNudgeTempo", "10");
    value(*transport, "LoopOn", "false");
    value(*transport, "LoopStart", "0");
    value(*transport, "LoopLength", "16");
    value(*transport, "LoopIsSongStart", "false");
    value(*transport, "CurrentTime", "0");
    value(*transport, "PunchIn", "false");
    value(*transport, "PunchOut", "false");
    value(*transport, "MetronomeTickDuration", "0");
    value(*transport, "DrawMode", "false");

    auto* scroll = live.createNewChildElement("SessionScrollPos");
    scroll->setAttribute("X", 0);
    scroll->setAttribute("Y", 0);
    value(live, "SelectedBreakpointValue", "0");
    live.createNewChildElement("SignalModulations");
    value(live, "GlobalQuantisation", "4");
    value(live, "AutoQuantisation", "0");
    grid(live, "Grid", true);
    auto* scale = live.createNewChildElement("ScaleInformation");
    value(*scale, "Root", "0");
    value(*scale, "Name", "0");
    value(live, "InKey", "true");
    value(live, "SmpteFormat", "0");
    auto* selection = live.createNewChildElement("TimeSelection");
    value(*selection, "AnchorTime", "0");
    value(*selection, "OtherTime", "0");
    auto* navigator = live.createNewChildElement("SequencerNavigator");
    value(*navigator->createNewChildElement("BeatTimeHelper"), "CurrentZoom", "0.254945054945054927");
    auto* scrollerPos = navigator->createNewChildElement("ScrollerPos");
    scrollerPos->setAttribute("X", 0);
    scrollerPos->setAttribute("Y", 0);
    auto* client = navigator->createNewChildElement("ClientSize");
    client->setAttribute("X", 862);
    client->setAttribute("Y", 602);
    value(live, "IsContentSplitterOpen", "true");
    value(live, "IsExpressionSplitterOpen", "true");
    auto lane = [](XmlElement& list, int laneId, int type, int size, bool minimized) {
        auto* m = list.createNewChildElement("MidiEditorLaneModel");
        m->setAttribute("Id", laneId);
        value(*m, "Type", juce::String(type));
        value(*m, "Size", juce::String(size));
        value(*m, "IsMinimized", minimized ? "true" : "false");
    };
    auto* expression = live.createNewChildElement("ExpressionLanes");
    lane(*expression, 0, 0, 41, false);
    lane(*expression, 1, 1, 41, false);
    lane(*expression, 2, 2, 41, true);
    lane(*expression, 3, 3, 41, true);
    lane(*expression, 4, 5, 41, true);
    auto* content = live.createNewChildElement("ContentLanes");
    lane(*content, 0, 2, 41, false);
    lane(*content, 2, 3, 25, true);
    lane(*content, 1, 4, 25, true);
    value(live, "ViewStateFxSlotCount", "4");
    value(live, "ViewStateSessionMixerVolumeSectionHeight", "102");
    value(live, "ViewStateArrangerMixerVolumeSectionHeight", "120");
    value(live, "ShouldSceneTempoAndTimeSignatureBeVisible", "false");
    value(live, "WaveformVerticalZoomFactor", "1");
    value(live, "IsWaveformVerticalZoomActive", "true");
    live.createNewChildElement("Locators")->createNewChildElement("Locators");
    live.createNewChildElement("DetailClipKeyMidis");
    for (const char* wrapper : {"TracksListWrapper", "VisibleTracksListWrapper", "ReturnTracksListWrapper", "ScenesListWrapper", "CuePointsListWrapper"})
        live.createNewChildElement(wrapper)->setAttribute("LomId", 0);
    value(live, "SelectedDocumentViewInMainWindow", "1");
    value(live, "Annotation", "");
    value(live, "SoloOrPflSavedValue", "true");
    value(live, "SoloInPlace", "true");
    value(live, "CrossfadeCurve", "2");
    value(live, "LatencyCompensation", "2");
    value(live, "HighlightedTrackIndex", "0");
    auto* grooves = live.createNewChildElement("GroovePool");
    value(*grooves, "LomId", "0");
    grooves->createNewChildElement("Grooves");
    value(*grooves, "DefaultGrooveId", "-1");
    grooves->createNewChildElement("GroovesListWrapper")->setAttribute("LomId", 0);
    value(live, "AutomationMode", "false");
    value(live, "SnapAutomationToGrid", "true");
    value(live, "ArrangementOverdub", "false");
    value(live, "ColorSequenceIndex", "5");
    value(*live.createNewChildElement("AutoColorPickerForPlayerAndGroupTracks"), "NextColorIndex", "4");
    value(*live.createNewChildElement("AutoColorPickerForReturnAndMainTracks"), "NextColorIndex", "1");
    value(live, "ViewData", "{}");
    value(live, "ResetNonautomatedMidiControllersOnClipStarts", "false");
    value(live, "MidiFoldIn", "false");
    value(live, "MidiFoldMode", "-99");
    value(live, "MultiClipFocusMode", "false");
    value(live, "MultiClipLoopBarHeight", "0");
    value(live, "MidiPrelisten", "false");
    live.createNewChildElement("LinkedTrackGroups");
    value(live, "NoteSpellingPreference", "0");
    value(live, "AccidentalSpellingPreference", "3");
    value(live, "PreferFlatRootNote", "false");
    value(live, "UseWarperLegacyHiQMode", "false");
    auto* video = live.createNewChildElement("VideoWindowRect");
    for (const char* side : {"Top", "Left", "Bottom", "Right"})
        video->setAttribute(side, "-2147483648");
    value(live, "ShowVideoWindow", "true");
    live.createNewChildElement("TuningSystems");
    value(live, "TrackHeaderWidth", "93");
    value(live, "ViewStateMainWindowClipDetailOpen", "false");
    value(live, "ViewStateMainWindowHiddenOtherDocViewTypeClipDetailOpen", "false");
    value(live, "ViewStateMainWindowHiddenOtherDocViewTypeDeviceDetailOpen", "true");
    value(live, "ViewStateMainWindowDeviceDetailOpen", "true");
    value(live, "ViewStateSecondWindowClipDetailOpen", "true");
    value(live, "ViewStateSecondWindowDeviceDetailOpen", "false");
    auto* states = live.createNewChildElement("ViewStates");
    for (const auto& [name, v] : std::initializer_list<std::pair<const char*, const char*>>{
             {"MixerInArrangement", "-2147483648"}, {"ArrangerMixerIO", "-2147483648"}, {"ArrangerMixerSends", "-2147483648"},
             {"ArrangerMixerReturns", "-2147483648"}, {"ArrangerMixerVolume", "-2147483648"}, {"ArrangerMixerTrackOptions", "-2147483648"},
             {"ArrangerMixerCrossFade", "-2147483648"}, {"ArrangerMixerTrackPerformanceImpactMeter", "-2147483648"},
             {"MixerInSession", "-2147483648"}, {"SessionIO", "1"}, {"SessionSends", "1"}, {"SessionReturns", "1"}, {"SessionVolume", "1"},
             {"SessionTrackOptions", "0"}, {"SessionCrossFade", "0"}, {"SessionTrackPerformanceImpactMeter", "-2147483648"},
             {"SessionShowOverView", "0"}, {"ArrangerIO", "1"}, {"ArrangerReturns", "1"}, {"ArrangerVolume", "1"},
             {"ArrangerTrackOptions", "0"}, {"ArrangerShowOverView", "1"}})
        value(*states, name, v);

    live.getChildByName("NextPointeeId")->setAttribute("Value", ids.next + 8);
    return root;
}

std::string groupTrackXml(int trackId, const std::string& name, int colour, int scenes, int sends, int firstPointeeId, int& pointeeEnd)
{
    XmlElement holder("Tracks");
    Ids ids;
    ids.next = firstPointeeId;
    buildTrack(holder, Kind::Group, trackId, juce::String::fromUTF8(name.c_str()), colour, scenes, sends, ids);
    pointeeEnd = ids.next;
    return holder.getFirstChildElement()->toString(juce::XmlElement::TextFormat().withoutHeader()).toStdString();
}

std::string audioTrackXml(int trackId, const std::string& name, int colour, int scenes, int sends, int firstPointeeId, int& pointeeEnd)
{
    XmlElement holder("Tracks");
    Ids ids;
    ids.next = firstPointeeId;
    buildTrack(holder, Kind::Audio, trackId, juce::String::fromUTF8(name.c_str()), colour, scenes, sends, ids);
    pointeeEnd = ids.next;
    return holder.getFirstChildElement()->toString(juce::XmlElement::TextFormat().withoutHeader()).toStdString();
}

std::string returnTrackXml(int trackId, const std::string& name, int colour, int sends, int firstPointeeId, int& pointeeEnd)
{
    XmlElement holder("Tracks");
    Ids ids;
    ids.next = firstPointeeId;
    buildTrack(holder, Kind::Return, trackId, juce::String::fromUTF8(name.c_str()), colour, 0, sends, ids);
    pointeeEnd = ids.next;
    return holder.getFirstChildElement()->toString(juce::XmlElement::TextFormat().withoutHeader()).toStdString();
}

std::string sendHolderXml(int index, bool enabled, int firstPointeeId, int& pointeeEnd)
{
    XmlElement holder("TrackSendHolder");
    holder.setAttribute("Id", index);
    Ids ids;
    ids.next = firstPointeeId;
    timeable(holder, "Send", 0.0003162277571, 0.0003162277571, 1, ids);
    value(holder, "EnabledByUser", enabled ? "true" : "false");
    pointeeEnd = ids.next;
    return holder.toString(juce::XmlElement::TextFormat().withoutHeader()).toStdString();
}

std::string midiTrackXml(int trackId, const std::string& name, int colour, int scenes, int sends, int firstPointeeId, int& pointeeEnd)
{
    XmlElement holder("Tracks");
    Ids ids;
    ids.next = firstPointeeId;
    buildTrack(holder, Kind::Midi, trackId, juce::String::fromUTF8(name.c_str()), colour, scenes, sends, ids);
    pointeeEnd = ids.next;
    return holder.getFirstChildElement()->toString(juce::XmlElement::TextFormat().withoutHeader()).toStdString();
}

std::string midiClipTemplate()
{
    XmlElement clip("MidiClip");
    clip.setAttribute("Id", 0);
    clip.setAttribute("Time", 0);
    value(clip, "LomId", "0");
    value(clip, "LomIdView", "0");
    value(clip, "CurrentStart", "0");
    value(clip, "CurrentEnd", "4");
    auto* loop = clip.createNewChildElement("Loop");
    value(*loop, "LoopStart", "0");
    value(*loop, "LoopEnd", "4");
    value(*loop, "StartRelative", "0");
    value(*loop, "LoopOn", "true");
    value(*loop, "OutMarker", "4");
    value(*loop, "HiddenLoopStart", "0");
    value(*loop, "HiddenLoopEnd", "4");
    value(clip, "Name", "");
    value(clip, "Annotation", "");
    value(clip, "Color", "1");
    value(clip, "LaunchMode", "0");
    value(clip, "LaunchQuantisation", "0");
    auto* signature = clip.createNewChildElement("TimeSignature")->createNewChildElement("TimeSignatures")
                          ->createNewChildElement("RemoteableTimeSignature");
    signature->setAttribute("Id", 0);
    value(*signature, "Numerator", "4");
    value(*signature, "Denominator", "4");
    value(*signature, "Time", "0");
    clip.createNewChildElement("Envelopes")->createNewChildElement("Envelopes");
    auto* scroller = clip.createNewChildElement("ScrollerTimePreserver");
    value(*scroller, "LeftTime", "0");
    value(*scroller, "RightTime", "4");
    auto* selection = clip.createNewChildElement("TimeSelection");
    value(*selection, "AnchorTime", "0");
    value(*selection, "OtherTime", "0");
    value(clip, "Legato", "false");
    value(clip, "Ram", "false");
    value(*clip.createNewChildElement("GrooveSettings"), "GrooveId", "-1");
    value(clip, "Disabled", "false");
    value(clip, "VelocityAmount", "0");
    auto* follow = clip.createNewChildElement("FollowAction");
    value(*follow, "FollowTime", "4");
    value(*follow, "IsLinked", "true");
    value(*follow, "LoopIterations", "1");
    value(*follow, "FollowActionA", "4");
    value(*follow, "FollowActionB", "0");
    value(*follow, "FollowChanceA", "100");
    value(*follow, "FollowChanceB", "0");
    value(*follow, "JumpIndexA", "0");
    value(*follow, "JumpIndexB", "0");
    value(*follow, "FollowActionEnabled", "false");
    auto* g = clip.createNewChildElement("Grid");
    value(*g, "FixedNumerator", "1");
    value(*g, "FixedDenominator", "16");
    value(*g, "GridIntervalPixel", "20");
    value(*g, "Ntoles", "2");
    value(*g, "SnapToGrid", "true");
    value(*g, "Fixed", "true");
    value(clip, "FreezeStart", "0");
    value(clip, "FreezeEnd", "0");
    value(clip, "IsWarped", "true");
    value(clip, "TakeId", "-1");
    value(clip, "IsInKey", "true");
    auto* scale = clip.createNewChildElement("ScaleInformation");
    value(*scale, "Root", "0");
    value(*scale, "Name", "0");
    auto* notes = clip.createNewChildElement("Notes");
    notes->createNewChildElement("KeyTracks");
    notes->createNewChildElement("PerNoteEventStore")->createNewChildElement("EventLists");
    notes->createNewChildElement("NoteProbabilityGroups");
    value(*notes->createNewChildElement("ProbabilityGroupIdGenerator"), "NextId", "1");
    value(*notes->createNewChildElement("NoteIdGenerator"), "NextId", "1");
    value(clip, "BankSelectCoarse", "-1");
    value(clip, "BankSelectFine", "-1");
    value(clip, "ProgramChange", "-1");
    value(clip, "NoteEditorFoldInZoom", "-1");
    value(clip, "NoteEditorFoldInScroll", "0");
    value(clip, "NoteEditorFoldOutZoom", "3072");
    value(clip, "NoteEditorFoldOutScroll", "0");
    value(clip, "NoteEditorFoldScaleZoom", "-1");
    value(clip, "NoteEditorFoldScaleScroll", "0");
    value(clip, "NoteSpellingPreference", "0");
    value(clip, "AccidentalSpellingPreference", "3");
    value(clip, "PreferFlatRootNote", "false");
    auto* expression = clip.createNewChildElement("ExpressionGrid");
    value(*expression, "FixedNumerator", "1");
    value(*expression, "FixedDenominator", "16");
    value(*expression, "GridIntervalPixel", "20");
    value(*expression, "Ntoles", "2");
    value(*expression, "SnapToGrid", "false");
    value(*expression, "Fixed", "false");
    return clip.toString().toStdString();
}

} // namespace sonora::io

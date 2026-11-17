// PLG-25: Sonora Phrase's face (full in the VST3 window and its own window, compact in the device chain) driven by real
// mouse events, and the VST3 processor (MIDI out, audition voice, state, Drag MIDI). PNGs: sonora_phrase_<part>.png and
// sonora_phrase_chain.png in the test temp folder.
#include "ProFaceTestKit.h"

#include "devices/native/Phrase.h"
#include "plugin/MidiDragOut.h"
#include "plugin/PhrasePlugin.h"
#include "plugin/PhrasePluginEditor.h"
#include "ui/views/PhraseFace.h"

#include <set>

using namespace sonora;
using namespace sonora::testkit;
using P = devices::Phrase;

namespace
{
void clickAt(ui::RootWidget& root, ui::ProFace& face, float x, float y, bool alt = false, bool shift = false, int clicks = 1)
{
    auto e = designAt(face, x, y, clicks);
    e.alt = alt;
    e.shift = shift;
    root.dispatchMouseDown(e);
    root.dispatchMouseUp(e);
}
void clickRect(ui::RootWidget& root, ui::ProFace& face, const ui::Rect& r, bool alt = false, bool shift = false, int clicks = 1)
{
    clickAt(root, face, r.centreX(), r.centreY(), alt, shift, clicks);
}
void dragFrom(ui::RootWidget& root, ui::ProFace& face, float x0, float y0, float x1, float y1, bool alt = false)
{
    auto e = designAt(face, x0, y0);
    e.alt = alt;
    root.dispatchMouseDown(e);
    for (int i = 1; i <= 8; ++i)
    {
        auto d = designAt(face, x0 + (x1 - x0) * (float) i / 8.0f, y0 + (y1 - y0) * (float) i / 8.0f);
        d.alt = alt;
        root.dispatchMouseDrag(d);
    }
    root.dispatchMouseUp(designAt(face, x1, y1));
}
std::string partFile(model::PhrasePart part)
{
    std::string name = model::phrasePartName(part);
    for (auto& ch : name) ch = (char) std::tolower((unsigned char) ch);
    return "sonora_phrase_" + name + ".png";
}
} // namespace

TEST_CASE("phrase plugin: the full face by mouse; tones, lanes, slots, form, palette, Dice / Answer, presets; PNG per part")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::PhrasePlugin p;
    CHECK(p.acceptsMidi() && p.producesMidi() && ! p.isMidiEffect());
    CHECK(p.getParameters().size() == P::NumParams);
    p.prepareToPlay(48000, 512);
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    auto* window = dynamic_cast<plugin::PhrasePluginEditor*>(editor.get());
    REQUIRE(window != nullptr);
    auto& view = window->view();
    const auto size = ui::PhraseFace::kFull;
    view.setBounds({0, 0, size.w, size.h});
    view.layout();
    auto& face = window->face();
    REQUIRE(face.layoutMode() == ui::ProFace::Layout::Full);
    CHECK(face.onDragMidi != nullptr);
    CHECK(face.controlCentre("Drag MIDI").has_value());
    auto& phrase = p.device();

    // Tone grid: a click places a note on a tone, a click on it removes it, a drag paints along the row under the pointer.
    auto& grid = face.toneGrid();
    auto& slotA = phrase.editState().part(model::PhrasePart::Arp).slots[0];
    for (auto& st : slotA.steps) st.state = model::StepState::Rest;
    phrase.publishState();
    clickRect(view, face, grid.cellRect(1, 2));
    CHECK(slotA.steps[1].state == model::StepState::Note);
    CHECK(slotA.steps[1].tone == 2);
    clickRect(view, face, grid.cellRect(1, 2));
    CHECK(slotA.steps[1].state == model::StepState::Rest);
    const auto from = grid.cellRect(4, 0), to = grid.cellRect(9, 0);
    dragFrom(view, face, from.centreX(), from.centreY(), to.centreX(), to.centreY());
    for (int s = 4; s <= 9; ++s) CHECK(slotA.steps[(size_t) s].state == model::StepState::Note && slotA.steps[(size_t) s].tone == 0);
    // Alt-drag a note upwards: scale steps.
    const auto alt = grid.cellRect(6, 0);
    dragFrom(view, face, alt.centreX(), alt.centreY(), alt.centreX(), alt.centreY() - 22, true);
    CHECK(slotA.steps[6].scaleStep == 2);
    // The audio thread sees the edit.
    juce::AudioBuffer<float> block(2, 512);
    juce::MidiBuffer midi;
    p.processBlock(block, midi);
    // Lanes: velocity by height, Alt-click resets.
    auto& velocity = face.lane("Velocity");
    clickAt(view, face, velocity.cellRect(5).centreX(), velocity.cellRect(5).y + 1);
    CHECK(slotA.steps[5].velocity == 127);
    clickAt(view, face, velocity.cellRect(5).centreX(), velocity.cellRect(5).centreY(), true);
    CHECK(slotA.steps[5].velocity == 90);
    clickRect(view, face, face.lane("Accent").cellRect(7));
    CHECK(slotA.steps[7].accent);
    clickRect(view, face, face.lane("Tie").cellRect(10));
    CHECK(slotA.steps[10].state == model::StepState::Tie);

    // Slot tabs, the form presets and the live Pattern choice.
    auto slotTab = [](int s) { return ui::Rect{100 + 34.0f * (float) s, 166, 30, 24}; }; // "A".."H" also label the Link / Pattern segments
    clickRect(view, face, slotTab(2));
    CHECK(face.selectedSlot() == 2);
    REQUIRE(click(view, face, "A B A C"));
    const auto& arpPart = phrase.state().part(model::PhrasePart::Arp);
    CHECK(arpPart.formLength == 4 && arpPart.form[3] == 2);
    clickRect(view, face, slotTab(0));
    CHECK(face.selectedSlot() == 0);
    CHECK(std::lround(phrase.getParameter(P::LinkChannel)) == 0);

    // Dice and Answer change the roll; Answer selects the next slot.
    const auto notesBefore = face.roll().notes;
    clickRect(view, face, face.button("Dice").bounds);
    face.refreshRoll();
    CHECK(face.roll().notes.size() != notesBefore.size() || face.roll().notes.front().pitch != notesBefore.front().pitch
          || std::abs(face.roll().notes.front().start - notesBefore.front().start) > 1e-9);
    clickRect(view, face, face.button("Answer").bounds);
    CHECK(face.selectedSlot() == 1);
    // A style box: the Shape choice steps on a click.
    const float shapeBefore = face.box("Shape").get();
    clickRect(view, face, face.box("Shape").bounds);
    CHECK(std::abs(face.box("Shape").get() - shapeBefore) > 0.5f);

    // The chord strip and the palette: a click on a chip opens it, a double-click on a chord puts it into the slot.
    auto& strip = face.chordStrip();
    clickRect(view, face, strip.chipRect(1));
    CHECK(face.selectedChord() == 1);
    REQUIRE(face.palette().open);
    CHECK(gfx::renderToPng(view, (int) size.w, (int) size.h, 1.0f, testFile("sonora_phrase_palette.png").getFullPathName().toStdString()));
    const auto& palette = face.palette();
    clickRect(view, face, palette.cellRect(3, 1), false, false, 2); // iv7 in E minor: Am7
    const auto& chord = phrase.state().progression.slots[1].chord;
    CHECK(! chord.chromatic);
    CHECK(chord.root == 3);
    CHECK(chord.extension == model::ChordExtension::Seventh);
    // Borrow: one fifth up (B), Phrygian Dominant's I (B major) is not in E minor: stored as a borrowed chord.
    clickRect(view, face, palette.rightRect());
    while (face.palette().scale != 16) clickRect(view, face, palette.modeRect());
    clickRect(view, face, palette.cellRect(0, 0), false, false, 2);
    CHECK(phrase.state().progression.slots[1].chord.chromatic);
    CHECK(model::chordName(model::chordTones(phrase.state().progression.slots[1].chord, 4, 1)) == "B");
    clickRect(view, face, palette.closeRect());
    CHECK(! face.palette().open);
    // Drag a chip's edge: 8 -> 16 beats.
    const auto chip0 = strip.chipRect(0);
    dragFrom(view, face, chip0.right() - 2, chip0.centreY(), chip0.right() + chip0.w * 0.9f, chip0.centreY());
    CHECK_NEAR(phrase.state().progression.slots[0].beats, 16.0, 1e-9);
    CHECK(phrase.state().keepChords);

    // Presets: next / menu.
    auto& bar = face.presetBar();
    clickRect(view, face, bar.nextRect());
    const int preset = phrase.matchingPreset();
    CHECK(preset >= 0);
    clickRect(view, face, bar.nameRect());
    REQUIRE(bar.menuOpen());
    clickRect(view, face, bar.categoryRow(3)); // Bass
    clickRect(view, face, bar.presetRow(0));
    CHECK(phrase.part() == model::PhrasePart::Bass);
    CHECK(P::presets()[(size_t) phrase.matchingPreset()].category == "Bass");
    CHECK(phrase.state().progression.slots[0].beats > 15.9); // Keep Chords was on: the edited progression stayed

    // Each part's face, rendered.
    for (int part = 0; part < 4; ++part)
    {
        phrase.setParameter(P::Part, (float) part);
        view.tickTree();
        view.layout();
        face.refreshRoll();
        CHECK(! face.roll().notes.empty());
        CHECK(gfx::renderToPng(view, (int) size.w, (int) size.h, 1.0f, testFile(partFile((model::PhrasePart) part)).getFullPathName().toStdString()));
    }
    // The pad face: a value box drag.
    phrase.setParameter(P::Part, (float) model::PhrasePart::Pad);
    view.tickTree();
    auto& voices = face.box("Voices");
    const float before = voices.get();
    dragFrom(view, face, voices.bounds.centreX(), voices.bounds.centreY(), voices.bounds.centreX(), voices.bounds.centreY() - 13);
    CHECK_NEAR(voices.get(), std::min(7.0f, before + 2), 1e-6f);
    p.editorBeingDeleted(editor.get());
    editor.reset();
}

TEST_CASE("phrase plugin: MIDI out, the audition voice, state round trip and the Drag MIDI file")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::PhrasePlugin p;
    p.prepareToPlay(48000, 512);
    juce::AudioBuffer<float> block(2, 512);
    int ons = 0;
    float peak = 0;
    for (int i = 0; i < 200; ++i) // a stopped host and no keys: nothing; the plug-in plays with the transport
    {
        juce::MidiBuffer midi;
        p.processBlock(block, midi);
        for (const auto meta : midi) ons += meta.getMessage().isNoteOn();
        peak = std::max(peak, block.getMagnitude(0, 512));
    }
    CHECK(ons == 0);
    // MIDI In with a held chord: notes come out and the audition voice sounds.
    p.device().setParameter(P::Chords, P::FromMidi);
    juce::MidiBuffer keys;
    for (int pitch : {52, 55, 59}) keys.addEvent(juce::MidiMessage::noteOn(1, pitch, (juce::uint8) 100), 0);
    p.processBlock(block, keys);
    for (const auto meta : keys) ons += meta.getMessage().isNoteOn();
    for (int i = 0; i < 200; ++i)
    {
        juce::MidiBuffer midi;
        p.processBlock(block, midi);
        for (const auto meta : midi) ons += meta.getMessage().isNoteOn();
        peak = std::max(peak, block.getMagnitude(0, 512));
    }
    CHECK(ons > 5);
    CHECK(peak > 0.01f);
    // Audition off: MIDI still comes out, no sound.
    p.device().setParameter(P::Audition, 0);
    float quiet = 0;
    int more = 0;
    for (int i = 0; i < 100; ++i)
    {
        juce::MidiBuffer midi;
        p.processBlock(block, midi);
        for (const auto meta : midi) more += meta.getMessage().isNoteOn();
        quiet = std::max(quiet, block.getMagnitude(0, 512));
    }
    CHECK(more > 0);
    CHECK(quiet < 1e-6f);
    // State.
    p.device().applyPreset(30);
    p.device().setParameter(P::Density, 0.7f);
    juce::MemoryBlock state;
    p.getStateInformation(state);
    plugin::PhrasePlugin restored;
    restored.setStateInformation(state.getData(), (int) state.getSize());
    CHECK(restored.buildLog.empty());
    CHECK(restored.device().state() == p.device().state());
    CHECK_NEAR(restored.device().getParameter(P::Density), 0.7f, 1e-6f);
    // The Drag MIDI file holds the part.
    double length = 0;
    const auto notes = p.partNotes(length);
    CHECK(length >= 4);
    std::string error;
    const auto file = plugin::writeDragMidiFile(notes, length, 138, "Sonora Phrase test", error);
    REQUIRE(file.existsAsFile());
    juce::FileInputStream in(file);
    juce::MidiFile midiFile;
    REQUIRE(midiFile.readFrom(in));
    int fileNotes = 0;
    for (int t = 0; t < midiFile.getNumTracks(); ++t)
        for (const auto* e : *midiFile.getTrack(t)) fileNotes += e->message.isNoteOn();
    CHECK(fileNotes == (int) notes.size());
    file.deleteFile();
}

TEST_CASE("phrase: the compact face in the chain, To Clip from the face, the window view; PNGs")
{
    Chain chain(model::TrackKind::Midi, {devices::nativeDeviceModel("SonoraPhrase")});
    chain.song.midiClipTemplate = std::make_shared<std::string>("<MidiClip/>");
    model::EditHistory history;
    chain.context.applyEdit = [&](model::EditCommand c) { history.perform(chain.song, std::move(c)); };
    ui::MainLayout root(chain.context);
    root.setBounds({0, 0, 1500, 900});
    root.layout();
    std::vector<ui::PhraseFace*> faces;
    collect(root, faces);
    REQUIRE(faces.size() == 1);
    auto& face = *faces[0];
    CHECK(face.layoutMode() == ui::ProFace::Layout::Compact);
    REQUIRE(face.onToClip != nullptr);
    auto* phrase = dynamic_cast<P*>(chain.processor());
    REQUIRE(phrase != nullptr);
    REQUIRE(click(root, face, "Stabs"));
    CHECK(phrase->part() == model::PhrasePart::Stabs);
    REQUIRE(click(root, face, "C")); // a slot pad: play slot C
    CHECK(std::lround(phrase->getParameter(P::Pattern)) == 3);
    REQUIRE(click(root, face, "C")); // again: back to the form
    CHECK(std::lround(phrase->getParameter(P::Pattern)) == 0);
    REQUIRE(drag(root, face, "DENSITY", 60));
    CHECK(phrase->getParameter(P::Density) < 0.9f);
    REQUIRE(click(root, face, "Dice"));
    CHECK(gfx::renderToPng(root, 1500, 900, 1.0f, testFile("sonora_phrase_chain.png").getFullPathName().toStdString()));
    // To Clip with no clip selected: a new clip at the playhead's bar, the device off afterwards.
    face.onToClip();
    REQUIRE(chain.song.tracks[0].arrangementClips.size() == 1);
    CHECK(! chain.song.tracks[0].arrangementClips[0].notes.empty());
    CHECK(phrase->isBypassed());
    CHECK(chain.context.statusText.find("Sonora Phrase wrote") == 0);

    ui::ProFaceWindowView window(chain.context, {1, 10, -1, -1});
    window.setBounds({0, 0, ui::PhraseFace::kFull.w, ui::PhraseFace::kFull.h});
    window.sync();
    REQUIRE(window.face() != nullptr);
    CHECK(window.face()->layoutMode() == ui::ProFace::Layout::Full);
    CHECK(window.face()->controlCentre("To Clip").has_value());
    CHECK(gfx::renderToPng(window, (int) ui::PhraseFace::kFull.w, (int) ui::PhraseFace::kFull.h, 1.0f,
                           testFile("sonora_phrase_window.png").getFullPathName().toStdString()));
}

TEST_CASE("phrase plugin: the site screenshot (a fresh window with Pedal Rider, 2x)")
{
    juce::ScopedJuceInitialiser_GUI init;
    plugin::PhrasePlugin p;
    p.prepareToPlay(48000, 512);
    const auto& list = P::presets();
    for (int i = 0; i < (int) list.size(); ++i)
        if (list[(size_t) i].name == "Pedal Rider") p.device().applyPreset(i);
    std::unique_ptr<juce::AudioProcessorEditor> editor(p.createEditor());
    auto* window = dynamic_cast<plugin::PhrasePluginEditor*>(editor.get());
    REQUIRE(window != nullptr);
    auto& view = window->view();
    const auto size = ui::PhraseFace::kFull;
    view.setBounds({0, 0, size.w, size.h});
    view.layout();
    window->face().refreshRoll();
    CHECK(gfx::renderToPng(view, (int) size.w, (int) size.h, 2.0f, testFile("sonora_phrase_plugin.png").getFullPathName().toStdString()));
    p.editorBeingDeleted(editor.get());
    editor.reset();
}

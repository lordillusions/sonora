// PLG-25a: the generator face parts (ui/views/ProFaceParts) on a test face, driven by real mouse events through the root
// (ProFaceTestKit) and rendered to PNG in both layouts: sonora_face_parts_full.png, sonora_face_parts_compact.png.
#include "ProFaceTestKit.h"
#include "TestGenerator.h"

#include "core/Harmony.h"
#include "ui/views/ProFaceParts.h"

using namespace sonora;
using namespace sonora::testkit;

namespace
{
class PartsFace final : public ui::ProFace
{
public:
    static constexpr Size kFull{1100, 640}, kCompact{740, 170};
    static constexpr SkColor kAccent = 0xFF35C2B0;

    explicit PartsFace(TestGenerator& g) : ProFace(g, kFull, kCompact, kAccent), gen(g)
    {
        velocity.fill(100);
        progression.count = 4;
        progression.slots[0] = {model::Chord::degree(0), 8};
        progression.slots[1] = {model::Chord::degree(5), 4};
        progression.slots[2] = {model::Chord::degree(6), 4};
        progression.slots[3] = {model::Chord::degree(0), 16};
        for (auto* part : std::initializer_list<ui::FacePart*>{&gates, &velocities, &octaves, &modes, &roll, &presetBar, &chords})
        {
            part->accent = kAccent;
            part->changed = [this] { ++changes; refreshRoll(); };
        }
        gates.name = "Gates";
        gates.kind = ui::StepLane::Kind::Toggle;
        gates.label = "STEP";
        gates.get = [this](int s) { return (float) gen.pattern.edited().on[(size_t) s]; };
        gates.set = [this](int s, float v) { gen.pattern.edit().on[(size_t) s] = v >= 0.5f; gen.pattern.publish(); };
        gates.playStep = [this] { return playStep; };
        gates.disabled = [](int s) { return s % 4 == 0; }; // the kick's sixteenth
        velocities.name = "Velocity";
        velocities.label = "VELOCITY";
        velocities.minValue = 1;
        velocities.maxValue = 127;
        velocities.defaultValue = 100;
        velocities.integral = true;
        velocities.get = [this](int s) { return velocity[(size_t) s]; };
        velocities.set = [this](int s, float v) { velocity[(size_t) s] = v; };
        octaves.name = "Octave";
        octaves.label = "OCTAVE";
        octaves.minValue = -2;
        octaves.maxValue = 2;
        octaves.integral = true;
        octaves.bipolar = true;
        octaves.get = [this](int s) { return octave[(size_t) s]; };
        octaves.set = [this](int s, float v) { octave[(size_t) s] = v; };
        modes.name = "Mode";
        modes.kind = ui::StepLane::Kind::Choice;
        modes.label = "STATE";
        modes.choices = {"-", "Tie", "Acc"};
        modes.get = [this](int s) { return mode[(size_t) s]; };
        modes.set = [this](int s, float v) { mode[(size_t) s] = v; };
        for (auto* lane : {&gates, &velocities, &octaves, &modes}) lane->labelWidth = 86;
        roll.name = "Roll";
        roll.lengthBeats = 16;
        roll.playBeat = [this] { return rollBeat; };
        presetBar.name = "Presets";
        presetBar.presets = {{"Arps", "Pedal Sixteenths"}, {"Arps", "Offbeat Rider"}, {"Arps", "Triplet Cascade"},
                             {"Stabs", "Question Answer"}, {"Stabs", "Dotted Push"}, {"Pads", "Wide Bed"}};
        presetBar.current = [this] { return preset; };
        presetBar.select = [this](int i) { preset = i; auto p = gen.pattern.edited(); p.transpose = (int8_t) i; gen.pattern.set(p); };
        presetBar.dice = [this] { ++dice; preset = -1; };
        chords.name = "Chords";
        chords.count = [this] { return progression.count; };
        chords.chip = [this](int i) {
            return ui::ChordStrip::Chip{model::chordName(model::chordTones(progression.slots[(size_t) i].chord, 9, 1)),
                                        progression.slots[(size_t) i].beats};
        };
        chords.playing = [] { return 1; };
        chords.selected = [this] { return selectedChord; };
        chords.select = [this](int i) { selectedChord = i; };
        chords.resize = [this](int i, double beats) { progression.slots[(size_t) i].beats = beats; };
        chords.add = [this] {
            if (progression.count < model::Progression::kMaxSlots) progression.slots[(size_t) progression.count++] = {model::Chord::degree(3), 8};
        };
        refreshRoll();
    }

    void refreshRoll()
    {
        std::vector<model::MidiNote> held;
        for (int pitch : {57, 60, 64})
        {
            model::MidiNote n;
            n.duration = 16;
            n.pitch = pitch;
            held.push_back(n);
        }
        roll.notes.clear();
        for (const auto& n : gen.renderNotes(held, 0, 16, 138)) roll.notes.push_back({n.start, n.duration, n.pitch, n.velocity, 0});
    }

    TestGenerator& gen;
    ui::StepLane gates, velocities, octaves, modes;
    ui::NoteRoll roll;
    ui::PresetBar presetBar;
    ui::ChordStrip chords;
    std::array<float, 16> velocity{}, octave{}, mode{};
    model::Progression progression;
    int preset = 0, dice = 0, selectedChord = -1, changes = 0, playStep = 5;
    double rollBeat = 5.5;

protected:
    void build(Layout layout) override
    {
        if (layout == Layout::Compact)
        {
            addPart(presetBar, {14, 12, 300, 30}, "Presets");
            addPart(gates, {14, 52, 420, 44}, "Steps");
            addPart(roll, {450, 12, 276, 146}, "Roll");
            return;
        }
        addPart(presetBar, {170, 18, 420, 34}, "Presets: previous, next, the menu, the dice");
        addPart(chords, {24, 76, 1052, 56}, "Chords: click to select, drag an edge to change the length");
        addPart(roll, {24, 150, 1052, 170}, "The generated notes");
        addPart(gates, {24, 338, 1052, 40}, "Steps on and off");
        addPart(velocities, {24, 386, 1052, 80}, "Velocity per step");
        addPart(octaves, {24, 474, 1052, 60}, "Octave per step");
        addPart(modes, {24, 542, 1052, 40}, "Step state");
    }
    void paintBack(SkCanvas& c, Layout layout) override
    {
        if (layout == Layout::Full) ui::pro::wordmark(c, "parts", 24, 14, 30, kAccent, "test");
        else ui::pro::panel(c, {6, 6, 728, 158});
    }
};

struct Rig
{
    TestGenerator gen;
    ui::UiContext ctx;
    ui::RootWidget root{ctx};
    PartsFace* face = nullptr;

    Rig(float w, float h)
    {
        gen.prepare(48000, 512);
        face = root.add<PartsFace>(gen);
        root.setBounds({0, 0, w, h});
        face->setBounds({0, 0, w, h});
        face->layout();
    }
    void down(float x, float y, bool alt = false, bool shift = false)
    {
        auto e = designAt(*face, x, y);
        e.alt = alt;
        e.shift = shift;
        root.dispatchMouseDown(e);
    }
    void drag(float x, float y)
    {
        root.dispatchMouseDrag(designAt(*face, x, y));
    }
    void up(float x, float y) { root.dispatchMouseUp(designAt(*face, x, y)); }
    void click(float x, float y, bool alt = false, bool shift = false)
    {
        down(x, y, alt, shift);
        up(x, y);
    }
    void clickCentre(const ui::Rect& r, bool alt = false, bool shift = false) { click(r.centreX(), r.centreY(), alt, shift); }
};
} // namespace

TEST_CASE("face parts: step lanes paint toggles, bars and choices with real mouse drags; Alt-click resets")
{
    juce::ScopedJuceInitialiser_GUI init;
    Rig rig(1100, 640);
    auto& f = *rig.face;
    REQUIRE(f.layoutMode() == ui::ProFace::Layout::Full);
    REQUIRE(f.controlCentre("Gates").has_value());

    // Toggle: a click flips one step; a drag from an off step paints "on" over the cells it passes.
    const bool wasOn = rig.gen.pattern.edited().on[2] != 0;
    rig.clickCentre(f.gates.cellRect(2));
    CHECK((rig.gen.pattern.edited().on[2] != 0) == ! wasOn);
    const auto notesBefore = f.roll.notes.size();
    rig.gen.pattern.edit().on.fill(0);
    rig.gen.pattern.publish();
    const auto r4 = f.gates.cellRect(4), r9 = f.gates.cellRect(9);
    rig.down(r4.centreX(), r4.centreY());
    for (int i = 1; i <= 3; ++i) rig.drag(r4.centreX() + (r9.centreX() - r4.centreX()) * (float) i / 3.0f, r4.centreY()); // skips cells
    rig.up(r9.centreX(), r9.centreY());
    for (int s = 0; s < 16; ++s) CHECK((rig.gen.pattern.edited().on[(size_t) s] != 0) == (s >= 4 && s <= 9));
    CHECK(f.roll.notes.size() != notesBefore); // the roll followed through `changed`
    CHECK(f.roll.notes.size() == 24 && f.changes > 0); // 6 of 16 steps over 64 sixteenths
    // A drag that starts on an on step paints "off".
    const auto r6 = f.gates.cellRect(6), r7 = f.gates.cellRect(7);
    rig.down(r6.centreX(), r6.centreY());
    rig.drag(r7.centreX(), r7.centreY());
    rig.up(r7.centreX(), r7.centreY());
    CHECK(rig.gen.pattern.edited().on[6] == 0 && rig.gen.pattern.edited().on[7] == 0 && rig.gen.pattern.edited().on[8] == 1);
    // The audio side sees the published pattern at its next block.
    rig.gen.pattern.update();
    CHECK(rig.gen.pattern.live().on[8] == 1 && rig.gen.pattern.live().on[6] == 0);

    // Bar: the top of a cell is the maximum; a drag follows the pointer's height across cells; Alt-click resets.
    const auto v2 = f.velocities.cellRect(2);
    rig.click(v2.centreX(), v2.y + 1);
    CHECK_NEAR(f.velocity[2], 127.0f, 1e-6f);
    const auto v3 = f.velocities.cellRect(3), v7 = f.velocities.cellRect(7);
    rig.down(v3.centreX(), v3.y + v3.h * 0.5f);
    rig.drag(v7.centreX(), v7.y + v7.h * 0.5f);
    rig.up(v7.centreX(), v7.y + v7.h * 0.5f);
    for (int s = 3; s <= 7; ++s) CHECK(std::abs(f.velocity[(size_t) s] - 64.0f) <= 1.0f);
    CHECK_NEAR(f.velocity[8], 100.0f, 1e-6f);
    rig.clickCentre(v2, true);
    CHECK_NEAR(f.velocity[2], 100.0f, 1e-6f);
    // Bipolar bar: the bottom is -2.
    const auto o5 = f.octaves.cellRect(5);
    rig.click(o5.centreX(), o5.bottom() - 1);
    CHECK_NEAR(f.octave[5], -2.0f, 1e-6f);

    // Choice: a click steps forward, Shift-click back, a drag paints the chosen value.
    const auto m1 = f.modes.cellRect(1), m4 = f.modes.cellRect(4);
    rig.clickCentre(m1);
    CHECK_NEAR(f.mode[1], 1.0f, 1e-6f);
    rig.clickCentre(m1);
    CHECK_NEAR(f.mode[1], 2.0f, 1e-6f);
    rig.clickCentre(m1, false, true);
    CHECK_NEAR(f.mode[1], 1.0f, 1e-6f);
    rig.down(m1.centreX(), m1.centreY()); // -> 2, then painted over 2..4
    rig.drag(m4.centreX(), m4.centreY());
    rig.up(m4.centreX(), m4.centreY());
    for (int s = 1; s <= 4; ++s) CHECK_NEAR(f.mode[(size_t) s], 2.0f, 1e-6f);
    rig.clickCentre(m4, true);
    CHECK_NEAR(f.mode[4], 0.0f, 1e-6f);

    // Help line over a part: its tip.
    f.hoverDesign(f.roll.bounds.centreX(), f.roll.bounds.centreY());
    CHECK(f.helpLine() == "The generated notes");
}

TEST_CASE("face parts: preset bar arrows, category menu and dice; chord strip select, resize and add; PNGs")
{
    juce::ScopedJuceInitialiser_GUI init;
    Rig rig(1100, 640);
    auto& f = *rig.face;
    auto& bar = f.presetBar;
    rig.clickCentre(bar.nextRect());
    CHECK(f.preset == 1);
    rig.clickCentre(bar.previousRect());
    rig.clickCentre(bar.previousRect()); // wraps
    CHECK(f.preset == 5);
    CHECK(rig.gen.pattern.edited().transpose == 5);
    // The menu: the name opens it on the current preset's category; a category row switches; a preset row selects.
    rig.clickCentre(bar.nameRect());
    REQUIRE(bar.menuOpen());
    CHECK(bar.openCategory() == 2); // Pads
    CHECK(bar.categories() == std::vector<std::string>({"Arps", "Stabs", "Pads"}));
    rig.clickCentre(bar.categoryRow(1));
    CHECK(bar.openCategory() == 1);
    CHECK(gfx::renderToPng(rig.root, 1100, 640, 1.0f, testFile("sonora_face_parts_menu.png").getFullPathName().toStdString()));
    // While open, the menu takes every click: one on a step lane under it does not edit the lane.
    const auto before = rig.gen.pattern.edited().on;
    const auto row1 = bar.presetRow(1);
    rig.clickCentre(row1);
    CHECK(! bar.menuOpen());
    CHECK(f.preset == 4); // Stabs / Dotted Push
    CHECK(rig.gen.pattern.edited().on == before);
    rig.clickCentre(bar.nameRect());
    REQUIRE(bar.menuOpen());
    rig.clickCentre(f.gates.cellRect(12)); // outside: closes, edits nothing
    CHECK(! bar.menuOpen());
    CHECK(rig.gen.pattern.edited().on == before);
    rig.clickCentre(bar.diceRect());
    CHECK(f.dice == 1);
    CHECK(f.preset == -1); // "(edited)"

    // Chord strip: chips by length, select, drag the first chip's edge to 16 beats, add a chip.
    auto& strip = f.chords;
    CHECK(strip.chipRect(3).w > strip.chipRect(1).w * 3.5f);
    rig.clickCentre(strip.chipRect(2));
    CHECK(f.selectedChord == 2);
    const auto c0 = strip.chipRect(0);
    const float perBeat = c0.w / 8.0f;
    rig.down(c0.right() - 2, c0.centreY());
    rig.drag(c0.right() - 2 + perBeat * 4.4f, c0.centreY()); // 8 + 4.4 = 12.4: nearest allowed length 16
    CHECK_NEAR(f.progression.slots[0].beats, 16.0, 1e-9);
    rig.drag(c0.right() - 2 - perBeat * 5.0f, c0.centreY());
    rig.up(c0.right() - 2 - perBeat * 5.0f, c0.centreY());
    CHECK_NEAR(f.progression.slots[0].beats, 2.0, 1e-9); // 8 - 5 = 3 -> 2 (the nearest allowed length)
    CHECK(f.selectedChord == 2); // resizing does not select
    rig.clickCentre(strip.addRect());
    CHECK(f.progression.count == 5);
    CHECK(strip.lengthText(2) == "1/2 bar");
    CHECK(strip.lengthText(16) == "4 bars");
    CHECK(strip.lengthText(4) == "1 bar");

    // The roll: the range covers the notes, the play head at 5.5 beats.
    REQUIRE(! f.roll.notes.empty());
    const auto [low, high] = f.roll.pitchRange();
    for (const auto& n : f.roll.notes) CHECK(n.pitch >= low && n.pitch <= high);
    CHECK(high - low >= 12);
    CHECK(gfx::renderToPng(rig.root, 1100, 640, 1.0f, testFile("sonora_face_parts_full.png").getFullPathName().toStdString()));

    // Compact layout: the lanes still take clicks at the chain's size.
    Rig compact(740 * 0.8f, 170 * 0.8f);
    REQUIRE(compact.face->layoutMode() == ui::ProFace::Layout::Compact);
    const bool on3 = compact.gen.pattern.edited().on[3] != 0;
    compact.clickCentre(compact.face->gates.cellRect(3));
    CHECK((compact.gen.pattern.edited().on[3] != 0) == ! on3);
    compact.clickCentre(compact.face->presetBar.nextRect());
    CHECK(compact.face->preset == 1);
    CHECK(gfx::renderToPng(compact.root, (int) (740 * 0.8f), (int) (170 * 0.8f), 2.0f,
                           testFile("sonora_face_parts_compact.png").getFullPathName().toStdString()));
}

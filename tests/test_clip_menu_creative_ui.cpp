// PLG-19 / PLG-20: the clip menu's "Generate Drum Fill" (MIDI clips) and "Make Riser / Downlifter from Clip" (audio clips).
#include "TestFramework.h"
#include "ui/TrackMenu.h"
#include "ui/UiContext.h"

using namespace sonora;

TEST_CASE("clip menu: Generate Drum Fill writes a fill into a MIDI clip; audio clips offer Make Riser / Downlifter")
{
    model::Song song;
    song.tempo = 124;
    song.tracks.resize(2);
    song.tracks[0].kind = model::TrackKind::Midi;
    song.tracks[0].id = 1;
    song.tracks[1].kind = model::TrackKind::Audio;
    song.tracks[1].id = 2;
    model::Clip beat;
    beat.kind = model::Clip::Kind::Midi;
    beat.uid = 7;
    beat.loop = {0, 4, 0, true};
    for (int b = 0; b < 4; ++b) { model::MidiNote n; n.start = b; n.pitch = 36; n.velocity = 110; beat.notes.push_back(n); }
    song.tracks[0].arrangementClips = {beat};
    model::Clip hit;
    hit.kind = model::Clip::Kind::Audio;
    hit.uid = 8;
    hit.currentStart = 16;
    song.tracks[1].arrangementClips = {hit};

    ui::UiContext ctx;
    ctx.song = &song;
    int edits = 0;
    ctx.applyEdit = [&](model::EditCommand command) { command.apply(song); ++edits; return true; };
    const ui::MenuAction* fill = nullptr;
    const auto midiActions = ui::clipContextActions(ctx, model::ClipLocation::arrangement(0, 7));
    for (const auto& a : midiActions) if (a.label == "Generate Drum Fill") fill = &a;
    REQUIRE(fill != nullptr);
    REQUIRE(fill->submenu.size() == 4);
    CHECK(fill->submenu[2].label == "Last Bar");
    fill->submenu[1].run(); // last 2 beats
    CHECK(edits == 1);
    CHECK(song.tracks[0].arrangementClips[0].notes.size() > 4);
    CHECK(ctx.statusText.find("Drum fill: ") == 0);
    for (const auto& n : song.tracks[0].arrangementClips[0].notes)
        if (n.pitch != 36) CHECK(n.start >= 2.0 - 1e-9);

    bool riser = false;
    double beats = 0;
    ctx.makeSweepFromClip = [&](const model::ClipLocation& where, bool up, double length) { riser = up && where.uid == 8; beats = length; };
    const ui::MenuAction* make = nullptr;
    const auto audioActions = ui::clipContextActions(ctx, model::ClipLocation::arrangement(1, 8));
    for (const auto& a : audioActions) if (a.label == "Make Riser from Clip") make = &a;
    REQUIRE(make != nullptr);
    REQUIRE(make->submenu.size() == 4);
    make->submenu[1].run(); // 2 Bars
    CHECK(riser);
    CHECK_NEAR(beats, 8.0, 1e-9);
    bool lifter = false;
    for (const auto& a : audioActions) if (a.label == "Make Downlifter from Clip") lifter = a.submenu.size() == 4;
    CHECK(lifter);
    for (const auto& a : audioActions) CHECK(a.label != "Generate Drum Fill");
}

// UI-85 / UI-86 / UI-87 through real mouse events on the Arrangement: a shortened audio clip keeps its fades inside it,
// a stretched clip's audio follows its edge, and the clip gain drags from the title-bar handle or Cmd+Option on the body.
#include "TestFramework.h"
#include "gfx/SkiaCanvasComponent.h"
#include "io/als/AlsReader.h"
#include "ui/ClipEditing.h"
#include "ui/views/ArrangementView.h"
#include "ui/views/MainLayout.h"

#include <juce_core/juce_core.h>
#include <cmath>
#include <cstdlib>
#include <juce_gui_basics/juce_gui_basics.h>

using namespace sonora;

namespace
{
struct AudioArrangementRig
{
    model::Song song;
    model::EditHistory history;
    ui::UiContext ctx;
    std::unique_ptr<ui::MainLayout> root;
    float laneTop = 0;
    AudioArrangementRig()
    {
        auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
        song = *loaded.song;
        song.tempo = 120.0;
        song.tracks[0].kind = model::TrackKind::Audio;
        // Track 0 becomes an audio track holding one warped 4-second sample (8 beats at 120 BPM) at beats 4 .. 12.
        auto& clip = song.tracks[0].arrangementClips[0];
        clip.kind = model::Clip::Kind::Audio;
        clip.notes.clear();
        clip.warped = true;
        clip.warpMode = model::WarpMode::Complex;
        clip.sampleRate = 44100;
        clip.lengthSamples = 44100 * 4;
        clip.warpMarkers = {{0.0, 0.0}, {4.0, 8.0}};
        clip.currentStart = clip.time = 4.0;
        clip.currentEnd = 12.0;
        clip.loop = {0.0, 8.0, 0.0, false};
        clip.gain = 1.0;
        ctx.song = &song;
        ctx.ioVisible = ctx.browserVisible = false;
        ctx.detailVisible = false;
        ctx.mainView = ui::MainView::Arrangement;
        ctx.pixelsPerBeat = 40.0; // grid: 1/2 beat
        ctx.applyEdit = [this](model::EditCommand c) { history.perform(song, std::move(c)); };
        ctx.undo = [this] { history.undo(song); };
        root = std::make_unique<ui::MainLayout>(ctx);
        root->setBounds({0, 0, 1200, 700});
        root->layout();
        laneTop = ctx.theme.metrics.controlBarHeight + ui::ArrangementView::rulerHeightFor(! song.locators.empty());
    }
    model::Clip& clip() { return song.tracks[0].arrangementClips[0]; }
    float beatX(double beat) const { return ctx.theme.metrics.outerMargin + (float) (beat * ctx.pixelsPerBeat); }
    static ui::MouseEvent mouse(float x, float y, bool cmd = false, bool alt = false, bool shift = false, int clicks = 1)
    {
        ui::MouseEvent e; e.x = x; e.y = y; e.cmd = cmd; e.alt = alt; e.shift = shift; e.clicks = clicks; return e;
    }
    void drag(float x0, float y0, float x1, float y1, bool cmd = false, bool alt = false, bool shift = false)
    {
        root->dispatchMouseMove(mouse(x0, y0, cmd, alt, shift));
        root->dispatchMouseDown(mouse(x0, y0, cmd, alt, shift));
        root->dispatchMouseDrag(mouse((x0 + x1) * 0.5f, (y0 + y1) * 0.5f, cmd, alt, shift));
        root->dispatchMouseDrag(mouse(x1, y1, cmd, alt, shift));
        root->dispatchMouseUp(mouse(x1, y1, cmd, alt, shift));
    }
    // The gain handle sits in the title bar, left of the clip's right resize zone (9 px).
    float handleX() const
    {
        const auto& c = song.tracks[0].arrangementClips[0];
        return beatX(c.currentEnd) - 9.0f - 3.0f - 9.0f;
    }
    float titleY() const { return laneTop + 7.0f; }
    // Renders the window; with SONORA_CLIP_UI_PNG=<folder> the pictures are kept there as <name>.png to look at.
    void paint(const char* name)
    {
        juce::TemporaryFile png(".png");
        auto file = png.getFile();
        if (const char* folder = std::getenv("SONORA_CLIP_UI_PNG"))
            file = juce::File(juce::String::fromUTF8(folder)).getChildFile(juce::String(name) + ".png");
        REQUIRE(gfx::renderToPng(*root, 1200, 700, 1, file.getFullPathName().toStdString()));
    }
};

double db(double gain) { return 20.0 * std::log10(gain); }
} // namespace

TEST_CASE("clip fades UI (UI-85): shortening a faded audio clip from its title bar keeps both fades inside it")
{
    AudioArrangementRig rig;
    rig.clip().fadeIn = 2.0; rig.clip().fadeOut = 3.0;
    rig.clip().fadeInDefault = rig.clip().fadeOutDefault = false;
    // Right edge 12 -> 6 (title bar): 2 beats left, the 2 + 3 beat fades shrink in proportion.
    rig.drag(rig.beatX(12.0) - 2, rig.titleY(), rig.beatX(6.0) - 2, rig.titleY());
    CHECK_NEAR(rig.clip().currentEnd, 6.0, 1e-9);
    CHECK_NEAR(rig.clip().fadeIn, 0.8, 1e-9);
    CHECK_NEAR(rig.clip().fadeOut, 1.2, 1e-9);
    CHECK(rig.history.undoLabel() == "Resize Clip");
    rig.paint("ui85_shortened");
    // The fades now meet; grabbed just right of where they meet, the fade-out handle (top of the body) drags inside the
    // shortened clip: from 1.2 beats to 0.5, and the fade-in stays.
    const float bodyHandleY = rig.laneTop + 1 + 12 + 4;
    rig.drag(rig.beatX(6.0 - 1.2) + 3, bodyHandleY, rig.beatX(6.0 - 0.5), bodyHandleY);
    CHECK_NEAR(rig.clip().fadeOut, 0.5, 1e-9);
    CHECK_NEAR(rig.clip().fadeIn, 0.8, 1e-9);
    CHECK(rig.clip().fadeIn + rig.clip().fadeOut <= 2.0 + 1e-9);
    CHECK(rig.history.undoLabel() == "Change Fade");
    // Left edge 4 -> 5.5: half a beat left; the fades fit it.
    rig.drag(rig.beatX(4.0) + 2, rig.titleY(), rig.beatX(5.5) + 2, rig.titleY());
    CHECK_NEAR(rig.clip().currentStart, 5.5, 1e-9);
    CHECK(rig.clip().fadeIn + rig.clip().fadeOut <= 0.5 + 1e-9);
    rig.paint("ui85_trimmed");
}

TEST_CASE("clip stretch UI (UI-86): Shift on a title-bar edge stretches a Beats clip and its audio with it")
{
    AudioArrangementRig rig;
    rig.clip().warpMode = model::WarpMode::Beats;
    // Right edge 12 -> 20 with Shift: twice as long, the sample's 4 s now last 16 beats.
    rig.drag(rig.beatX(12.0) - 2, rig.titleY(), rig.beatX(20.0) - 2, rig.titleY(), false, false, true);
    CHECK_NEAR(rig.clip().currentStart, 4.0, 1e-9);
    CHECK_NEAR(rig.clip().currentEnd, 20.0, 1e-9);
    CHECK_NEAR(rig.clip().warpMarkers.back().seconds, 4.0, 1e-9);
    CHECK_NEAR(rig.clip().warpMarkers.back().beats, 16.0, 1e-9);
    CHECK(rig.clip().warpMode == model::WarpMode::Complex); // Beats slices would keep their speed (UI-86)
    CHECK(rig.history.undoLabel() == "Stretch Clip");
    rig.paint("ui86_stretched");
    REQUIRE(rig.history.undo(rig.song));
    CHECK(rig.clip().warpMode == model::WarpMode::Beats);
    CHECK_NEAR(rig.clip().currentEnd, 12.0, 1e-9);
}

TEST_CASE("clip gain UI (UI-87): the title-bar handle drags the clip gain up and down in dB, one undo step each")
{
    AudioArrangementRig rig;
    rig.root->dispatchMouseMove(AudioArrangementRig::mouse(rig.handleX(), rig.titleY()));
    rig.paint("ui87_hover"); // the handle is drawn on the hovered clip
    // 30 px up: +6 dB.
    rig.drag(rig.handleX(), rig.titleY(), rig.handleX(), rig.titleY() - 30);
    CHECK_NEAR(db(rig.clip().gain), 6.0, 1e-6);
    CHECK_NEAR(rig.clip().currentStart, 4.0, 1e-12); // a gain drag does not move the clip
    CHECK_NEAR(rig.clip().currentEnd, 12.0, 1e-12);
    CHECK(rig.history.undoLabel() == "Change Clip Gain");
    CHECK(rig.ctx.statusText == "Clip Gain: +6.0 dB");
    // 120 px up from there: stops at +24 dB, the Gain fader's top.
    rig.drag(rig.handleX(), rig.titleY(), rig.handleX(), rig.titleY() - 120);
    CHECK_NEAR(db(rig.clip().gain), 24.0, 1e-6);
    // Shift: fine steps (0.02 dB per pixel), 50 px down = -1 dB.
    rig.drag(rig.handleX(), rig.titleY(), rig.handleX(), rig.titleY() + 50, false, false, true);
    CHECK_NEAR(db(rig.clip().gain), 23.0, 1e-6);
    // Shift pressed during a drag continues from where the gain is, in fine steps: 10 px coarse (-2 dB), then 50 px fine.
    rig.root->dispatchMouseDown(AudioArrangementRig::mouse(rig.handleX(), rig.titleY()));
    rig.root->dispatchMouseDrag(AudioArrangementRig::mouse(rig.handleX(), rig.titleY() + 10));
    rig.root->dispatchMouseDrag(AudioArrangementRig::mouse(rig.handleX(), rig.titleY() + 10, false, false, true));
    rig.root->dispatchMouseDrag(AudioArrangementRig::mouse(rig.handleX(), rig.titleY() + 60, false, false, true));
    rig.root->dispatchMouseUp(AudioArrangementRig::mouse(rig.handleX(), rig.titleY() + 60, false, false, true));
    CHECK_NEAR(db(rig.clip().gain), 20.0, 1e-6);
    REQUIRE(rig.history.undo(rig.song));
    CHECK_NEAR(db(rig.clip().gain), 23.0, 1e-6);
    // Far down: silence (-inf), like the fader's bottom.
    rig.drag(rig.handleX(), rig.titleY(), rig.handleX(), rig.titleY() + 600);
    CHECK(rig.clip().gain <= 0.0);
    CHECK(rig.ctx.statusText == "Clip Gain: -inf dB");
    // Undo walks the steps back.
    REQUIRE(rig.history.undo(rig.song));
    CHECK_NEAR(db(rig.clip().gain), 23.0, 1e-6);
    // A double-click on the handle puts it back at 0 dB.
    rig.root->dispatchMouseDown(AudioArrangementRig::mouse(rig.handleX(), rig.titleY(), false, false, false, 2));
    rig.root->dispatchMouseUp(AudioArrangementRig::mouse(rig.handleX(), rig.titleY(), false, false, false, 2));
    CHECK_NEAR(rig.clip().gain, 1.0, 1e-12);
    CHECK(rig.history.undoLabel() == "Reset Clip Gain");
    // Away from the handle the title bar still moves the clip.
    rig.drag(rig.beatX(6.0), rig.titleY(), rig.beatX(8.0), rig.titleY());
    CHECK_NEAR(rig.clip().currentStart, 6.0, 1e-9);
    CHECK_NEAR(rig.clip().gain, 1.0, 1e-12);
}

TEST_CASE("clip gain UI (UI-87): Cmd+Option dragged vertically on the clip body changes the gain; Option alone still copies")
{
    AudioArrangementRig rig;
    const float body = rig.laneTop + 30.0f;
    rig.drag(rig.beatX(8.0), body, rig.beatX(8.0), body + 15, true, true);
    CHECK_NEAR(db(rig.clip().gain), -3.0, 1e-6);
    CHECK(rig.song.tracks[0].arrangementClips.size() == 1); // no copy
    CHECK(rig.history.undoLabel() == "Change Clip Gain");
    // The waveform is drawn scaled by the gain while dragging and after (paints without failing).
    rig.root->dispatchMouseDown(AudioArrangementRig::mouse(rig.beatX(8.0), body, true, true));
    rig.root->dispatchMouseDrag(AudioArrangementRig::mouse(rig.beatX(8.0), body - 20, true, true));
    rig.paint("ui87_dragging");
    rig.root->dispatchMouseUp(AudioArrangementRig::mouse(rig.beatX(8.0), body - 20, true, true));
    CHECK_NEAR(db(rig.clip().gain), 1.0, 1e-6);
    // Option alone on the body still drops a copy.
    rig.drag(rig.beatX(8.0), body, rig.beatX(16.0), body, false, true);
    CHECK(rig.song.tracks[0].arrangementClips.size() == 2);
}

TEST_CASE("clip gain (UI-87): the drag law and its readout")
{
    CHECK_NEAR(db(ui::dragClipGain(1.0, -10.0f, false)), 2.0, 1e-9);
    CHECK_NEAR(db(ui::dragClipGain(1.0, 10.0f, true)), -0.2, 1e-9);
    CHECK_NEAR(ui::dragClipGain(0.0, -5.0f, false), std::pow(10.0, -69.0 / 20.0), 1e-12); // from silence: starts at -70 dB
    CHECK(ui::dragClipGain(1.0, 1000.0f, false) <= 0.0);
    CHECK_NEAR(db(ui::dragClipGain(10.0, 0.0f, false)), 20.0, 1e-9);
    CHECK(ui::clipGainText(1.0) == "0.0 dB");
    CHECK(ui::clipGainText(std::pow(10.0, 3.0 / 20.0)) == "+3.0 dB");
    CHECK(ui::clipGainText(std::pow(10.0, -12.5 / 20.0)) == "-12.5 dB");
    CHECK(ui::clipGainText(0.0) == "-inf dB");
}

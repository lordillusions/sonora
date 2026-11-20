// UI-114: the Clip panel's number boxes take typed values like Live (click, type, Return) through the shared value field.
#include "TestFramework.h"
#include "core/Edit.h"
#include "gfx/SkiaCanvasComponent.h"
#include "io/als/AlsReader.h"
#include "ui/AudioClipEdits.h"
#include "ui/RootWidget.h"
#include "ui/views/ClipControls.h"

#include <juce_gui_basics/juce_gui_basics.h>

using namespace sonora;
using Id = ui::ClipControls::Id;

namespace
{
struct FieldRig
{
    std::unique_ptr<model::Song> song;
    model::EditHistory history;
    ui::UiContext ctx;
    std::unique_ptr<ui::RootWidget> root;
    ui::ClipControls* panel = nullptr;
    FieldRig()
    {
        auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
        REQUIRE(loaded);
        song = std::move(loaded.song);
        ctx.song = song.get();
        ctx.mainView = ui::MainView::Session;
        ctx.selectedClipTrack = 0;
        ctx.selectedClipScene = 1;
        ctx.selectedClipUid = song->tracks[0].slots[1].clip->uid;
        ctx.applyEdit = [this](model::EditCommand command) { history.perform(*song, std::move(command)); };
        root = std::make_unique<ui::RootWidget>(ctx);
        panel = root->add<ui::ClipControls>();
        root->setBounds({0, 0, 400, 900});
        panel->setBounds({0, 0, 340, 900});
        panel->columnsFor(900);
    }
    const model::Clip& clip() const { return *ctx.selectedClip(); }
    // Click (no drag) at a fraction of the control's width.
    void click(Id id, float across = 0.5f)
    {
        panel->columnsFor(900); // lays the boxes out again, as a paint would after a header opened
        const auto r = panel->controlBounds(id);
        REQUIRE(! r.isEmpty());
        ui::MouseEvent e; e.x = r.x + r.w * across; e.y = r.centreY();
        root->dispatchMouseDown(e); root->dispatchMouseUp(e);
    }
    void keys(const std::string& text)
    {
        for (char ch : text) CHECK(root->dispatchKey({.textChar = (uint32_t) ch}));
    }
    void enter(Id id, const std::string& text)
    {
        click(id);
        CHECK(ctx.valueEntry.active());
        keys(text); // the value opens selected: typing replaces it
        CHECK(root->dispatchKey({.keyCode = juce::KeyPress::returnKey}));
        CHECK(! ctx.valueEntry.active());
    }
};
} // namespace

TEST_CASE("clip fields: typed bars.beats.sixteenths, seconds and numbers parse like Live")
{
    using namespace ui::audioedit;
    CHECK_NEAR(*parseBeats("5", 4, true), 16, 0);
    CHECK_NEAR(*parseBeats("3.2", 4, true), 9, 0);
    CHECK_NEAR(*parseBeats("1. 1. 2", 4, true), 0.25, 0);
    CHECK_NEAR(*parseBeats("2 3 4", 4, true), 4 + 2 + 0.75, 0);
    CHECK_NEAR(*parseBeats("1.5", 4, true), 4, 0); // overflowing beats carry into the next bar
    CHECK_NEAR(*parseBeats("2", 4, false), 8, 0);  // a length: two bars
    CHECK_NEAR(*parseBeats("0.2.0", 4, false), 2, 0);
    CHECK_NEAR(*parseBeats("2", 3, false), 6, 0);  // 3/4
    CHECK_NEAR(*parseBeats("-2.1.1", 4, true), -4, 0);
    CHECK(! parseBeats("0", 4, true));
    CHECK(! parseBeats("1.2.3.4", 4, true));
    CHECK(! parseBeats("a", 4, true));
    CHECK(! parseBeats("", 4, true));
    CHECK_NEAR(*parseSeconds("1:30.5"), 90.5, 1e-12);
    CHECK_NEAR(*parseSeconds("0: 2.250"), 2.25, 1e-12);
    CHECK_NEAR(*parseSeconds("12,25"), 12.25, 1e-12);
    CHECK(! parseSeconds("1:x"));
    CHECK_NEAR(*parseNumber("1/4"), 0.25, 1e-12);
    CHECK_NEAR(*parseNumber("50 %"), 50, 0);
    CHECK_NEAR(*parseNumber("-3,5"), -3.5, 0);
    CHECK(! parseNumber(""));
    CHECK(! parseNumber("1/0"));
}

TEST_CASE("clip fields: Start End Position Length and Signature take typed values")
{
    FieldRig rig;
    REQUIRE(rig.panel);
    CHECK_NEAR(rig.clip().loop.loopEnd, 4, 0);

    // The field opens over the box with the value as it is shown (paint path; PNG for a look).
    const auto shot = [&](const char* name) {
        const char* dir = std::getenv("SONORA_FIELD_PNG_DIR");
        const juce::File out = dir ? juce::File(dir).getChildFile(name) : juce::File::createTempFile(".png");
        REQUIRE(gfx::renderToPng(*rig.root, 400, 900, 2, out.getFullPathName().toStdString()));
        if (! dir) out.deleteFile();
    };
    rig.click(Id::Start);
    REQUIRE(rig.ctx.valueEntry.active());
    CHECK(rig.ctx.valueEntry.edit.text == "1. 1. 1");
    shot("selected.png");
    rig.keys("3.2");
    shot("typing.png");
    CHECK(rig.root->dispatchKey({.keyCode = juce::KeyPress::escapeKey}));
    CHECK(! rig.ctx.valueEntry.active());
    CHECK(rig.history.version() == 0);

    rig.enter(Id::Length, "2"); // two bars
    CHECK_NEAR(rig.clip().loop.loopEnd - rig.clip().loop.loopStart, 8, 1e-9);
    CHECK(rig.history.undoLabel() == "Edit Clip Settings");

    rig.enter(Id::Position, "2"); // the loop moves to bar 2 and keeps its length
    CHECK_NEAR(rig.clip().loop.loopStart, 4, 1e-9);
    CHECK_NEAR(rig.clip().loop.loopEnd, 12, 1e-9);
    CHECK_NEAR(rig.clip().loop.loopStart + rig.clip().loop.startRelative, 0, 1e-9); // the start marker stays

    rig.enter(Id::Start, "2.3");
    CHECK_NEAR(rig.clip().loop.loopStart + rig.clip().loop.startRelative, 6, 1e-9);
    rig.enter(Id::End, "3 3 2");
    CHECK_NEAR(rig.clip().loop.loopEnd, 10.25, 1e-9);

    // Unparseable text, or the value committed unchanged, changes nothing.
    const int version = rig.history.version();
    rig.enter(Id::Length, "1.2.3.4");
    rig.click(Id::Length);
    CHECK(rig.root->dispatchKey({.keyCode = juce::KeyPress::returnKey}));
    CHECK(rig.history.version() == version);

    rig.enter(Id::SigNum, "7");
    CHECK(rig.clip().timeSigNumerator == 7);
    CHECK(rig.history.undoLabel() == "Change Clip Signature");
    rig.enter(Id::SigDen, "8");
    CHECK(rig.clip().timeSigDenominator == 8);
    rig.enter(Id::SigDen, "3"); // not a denominator Live offers
    CHECK(rig.clip().timeSigDenominator == 8);
    rig.enter(Id::SigNum, "250"); // clamped like Live
    CHECK(rig.clip().timeSigNumerator == 99);

    // A drag still drags (and opens no field); the denominator drags through 1 2 4 8 16.
    const auto drag = [&](Id id, float dy) {
        rig.panel->columnsFor(900);
        const auto r = rig.panel->controlBounds(id);
        ui::MouseEvent e; e.x = r.centreX(); e.y = r.centreY();
        rig.root->dispatchMouseDown(e); e.y -= dy; rig.root->dispatchMouseDrag(e); rig.root->dispatchMouseUp(e);
    };
    drag(Id::SigNum, -12);
    CHECK(rig.clip().timeSigNumerator == 97);
    CHECK(! rig.ctx.valueEntry.active());
    drag(Id::SigDen, 12);
    CHECK(rig.clip().timeSigDenominator == 16);

    // Typing then clicking elsewhere keeps the typed value.
    rig.click(Id::SigNum);
    rig.keys("5");
    rig.click(Id::Loop);
    CHECK(rig.clip().timeSigNumerator == 5);
    CHECK(! rig.ctx.valueEntry.active());
}

TEST_CASE("clip fields: launch velocity, pitch and tool settings take typed values")
{
    FieldRig rig;
    REQUIRE(rig.panel);
    rig.click(Id::LaunchHeader);
    rig.enter(Id::LaunchVelocity, "35");
    CHECK_NEAR(rig.clip().velocityAmount, 0.35, 1e-9);
    CHECK(rig.history.undoLabel() == "Change Launch Velocity");

    rig.click(Id::TransformHeader); // Quantize: Rate and Amount
    const int version = rig.history.version();
    rig.enter(Id::ToolRate, "1/8");
    CHECK_NEAR(rig.panel->toolSettings(false).rate, 0.125, 1e-12);
    rig.enter(Id::ToolAmount, "40");
    CHECK_NEAR(rig.panel->toolSettings(false).amount, 0.4, 1e-12);
    CHECK(rig.history.version() == version); // tool settings are not part of the set

    // Audio clip: the semitone and cent boxes, and minutes:seconds positions while it is not warped.
    auto& audio = rig.song->tracks[1];
    model::Clip clip;
    clip.kind = model::Clip::Kind::Audio;
    clip.warped = false;
    clip.currentStart = 0; clip.currentEnd = 4; clip.sampleRate = 48000; clip.lengthSamples = 96000;
    clip.loop = {0, 2, 0, false};
    clip.uid = 9001;
    audio.arrangementClips.push_back(clip);
    rig.ctx.mainView = ui::MainView::Arrangement;
    rig.ctx.selectedClipTrack = 1;
    rig.ctx.selectedClipScene = -1;
    rig.ctx.selectedClipUid = 9001;
    REQUIRE(rig.ctx.selectedClip());
    rig.enter(Id::PitchStBox, "-5");
    CHECK(rig.clip().pitchCoarse == -5);
    CHECK(rig.history.undoLabel() == "Change Clip Pitch");
    rig.enter(Id::PitchCt, "20");
    CHECK_NEAR(rig.clip().pitchFine, 20, 0);
    rig.enter(Id::End, "0:1.75"); // loop off in the Arrangement: End is the clip's right edge
    CHECK(rig.panel->controlBounds(Id::End).w > 0);
    CHECK(rig.history.undoLabel() == "Edit Clip Settings");
    rig.click(Id::End);
    CHECK(rig.ctx.valueEntry.edit.text == "0: 1.750");
    CHECK(rig.root->dispatchKey({.keyCode = juce::KeyPress::escapeKey}));
    const int beforeLength = rig.history.version();
    rig.enter(Id::Length, "1.5"); // Length (and Position) need the loop on, as in Live
    CHECK(rig.history.version() == beforeLength);
}

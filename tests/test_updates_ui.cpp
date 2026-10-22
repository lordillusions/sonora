// COM-4: Settings > Updates reflects the updater and drives it through UiContext callbacks.
#include "TestFramework.h"
#include "gfx/SkiaCanvasComponent.h"
#include "ui/views/MainLayout.h"
#include "ui/views/PreferencesPanel.h"

#include <juce_gui_basics/juce_gui_basics.h>

using namespace sonora;

namespace
{
ui::MouseEvent mouseAt(float x, float y) { ui::MouseEvent e; e.x = x; e.y = y; return e; }
template <typename T>
T* findWidget(ui::Widget& w)
{
    if (auto* hit = dynamic_cast<T*>(&w)) return hit;
    for (auto& child : w.children()) if (auto* hit = findWidget<T>(*child)) return hit;
    return nullptr;
}
} // namespace

TEST_CASE("updates settings: automatic checks, check now and the off-in-this-build state")
{
    ui::UiContext ctx;
    ui::UiContext::UpdateSettings state;
    state.available = true; state.automatic = true; state.canCheck = true;
    state.version = "0.2.0"; state.backend = "Sparkle 2.9.6";
    state.channels = {"Stable", "Beta"};
    int checks = 0, toggles = 0;
    ctx.updateSettings = [&] { return state; };
    ctx.checkForUpdates = [&] { ++checks; };
    ctx.setAutomaticUpdateChecks = [&](bool on) { ++toggles; state.automatic = on; };
    ctx.preferencesOpen = true; ctx.settingsPage = (int) ui::PreferencesPanel::Tab::Updates;
    ui::MainLayout root(ctx); root.setBounds({0, 0, 1000, 700});
    auto* settings = findWidget<ui::PreferencesPanel>(root); REQUIRE(settings);
    juce::TemporaryFile file(".png");
    auto draw = [&] { REQUIRE(gfx::renderToPng(root, 1000, 700, 1, file.getFile().getFullPathName().toStdString())); };
    auto click = [&](const std::string& label) {
        draw(); const auto r = settings->controlBounds(label); REQUIRE(! r.isEmpty());
        root.dispatchMouseDown(mouseAt(r.centreX(), r.centreY())); root.dispatchMouseUp(mouseAt(r.centreX(), r.centreY()));
    };
    click("Updates"); CHECK(ctx.settingsPage == (int) ui::PreferencesPanel::Tab::Updates);
    click("Check Automatically"); CHECK(toggles == 1); CHECK(! state.automatic);
    click("Check for Updates"); CHECK(checks == 1);
    draw(); CHECK(! settings->controlBounds("Update Channel").isEmpty());

    state.canCheck = false; // a session is in progress: the button is disabled
    draw(); CHECK(settings->controlBounds("Check for Updates").isEmpty());
    state.pending = true;   // a postponed install can always be resumed
    click("Check for Updates"); CHECK(checks == 2);

    state = {}; state.reason = "Updates are off in this build";
    draw();
    CHECK(settings->controlBounds("Check Automatically").isEmpty());
    CHECK(settings->controlBounds("Check for Updates").isEmpty());
}

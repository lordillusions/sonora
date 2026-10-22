#include "app/AboutWindow.h"

#include "core/Supporters.h"

#include "include/core/SkCanvas.h"

#include <cmath>

namespace sonora::app
{

namespace
{
AboutWindow* current = nullptr;

constexpr const char* websiteUrl = "https://sonoradaw.com";
constexpr const char* sourceUrl = "https://github.com/lordillusions/sonora";
constexpr const char* supportUrl = "https://ko-fi.com/lordillusions";

juce::File supportersFile()
{
    auto file = juce::File::getSpecialLocation(juce::File::currentApplicationFile).getChildFile("Contents/Resources/supporters.txt");
    if (! file.existsAsFile()) // Windows/Linux: Resources/ next to the executable (COM-7)
        file = juce::File::getSpecialLocation(juce::File::currentExecutableFile).getSiblingFile("Resources/supporters.txt");
    if (! file.existsAsFile()) // a development build run from the source tree
        file = juce::File(__FILE__).getParentDirectory().getParentDirectory().getParentDirectory().getChildFile("resources/supporters.txt");
    return file;
}
} // namespace

gfx::brand::AboutState AboutWindow::currentState()
{
    gfx::brand::AboutState state;
    auto* app = juce::JUCEApplicationBase::getInstance();
    state.version = "Version " + (app != nullptr ? app->getApplicationVersion().toStdString() : std::string("dev"));
    state.credit = "Created by Wellington Chaves \u00B7 Lordillusions";
    state.emptyText = "No supporters listed yet. Monthly supporters on Ko-fi can have their name here and on sonoradaw.com.";
    for (const auto& group : model::parseSupporters(supportersFile().loadFileAsString().toStdString()))
        state.groups.push_back({model::supporterTierTitle(group.tier), group.names});
    return state;
}

void AboutWindow::show()
{
    if (current == nullptr) current = new AboutWindow(); // owned by itself until close()
    current->toFront(true);
    current->grabKeyboardFocus();
}

AboutWindow::AboutWindow() : state_(currentState())
{
    setOpaque(false);
    setWantsKeyboardFocus(true);
    setSize((int) gfx::brand::aboutWidth, (int) std::ceil(gfx::brand::aboutHeight(state_)));
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getPrimaryDisplay())
        setCentrePosition(display->userBounds.toNearestInt().getCentre().translated(0, -60));
    addToDesktop(juce::ComponentPeer::windowHasDropShadow);
    setVisible(true);
}

AboutWindow::~AboutWindow()
{
    if (current == this) current = nullptr;
}

void AboutWindow::paint(juce::Graphics& g)
{
    float scale = 2.0f;
    if (const auto* display = juce::Desktop::getInstance().getDisplays().getDisplayForRect(getScreenBounds()))
        scale = (float) display->scale;
    const int pw = (int) std::lround((float) getWidth() * scale), ph = (int) std::lround((float) getHeight() * scale);
    const auto image = gfx::brand::renderImage(pw, ph, scale, [this](SkCanvas& c) {
        gfx::brand::drawAbout(c, (float) getWidth(), (float) getHeight(), state_);
    });
    g.drawImage(image, getLocalBounds().toFloat());
}

int AboutWindow::buttonAt(juce::Point<float> p) const
{
    const auto rects = gfx::brand::aboutButtonRects(state_, (float) getWidth(), (float) getHeight());
    for (size_t i = 0; i < rects.size(); ++i)
        if (rects[i].contains(p.x, p.y)) return (int) i;
    return -1;
}

void AboutWindow::mouseMove(const juce::MouseEvent& e)
{
    const int hot = buttonAt(e.position);
    if (hot != state_.hovered) { state_.hovered = hot; repaint(); }
    setMouseCursor(hot >= 0 ? juce::MouseCursor::PointingHandCursor : juce::MouseCursor::NormalCursor);
}

void AboutWindow::mouseExit(const juce::MouseEvent&)
{
    if (state_.hovered >= 0) { state_.hovered = -1; repaint(); }
}

void AboutWindow::mouseDown(const juce::MouseEvent& e)
{
    state_.pressed = state_.hovered = buttonAt(e.position);
    dragging_ = state_.pressed < 0;
    if (dragging_) dragger_.startDraggingComponent(this, e);
    repaint();
}

void AboutWindow::mouseDrag(const juce::MouseEvent& e)
{
    if (dragging_) dragger_.dragComponent(this, e, nullptr);
    else mouseMove(e);
}

void AboutWindow::mouseUp(const juce::MouseEvent& e)
{
    const int pressed = state_.pressed;
    state_.pressed = -1;
    dragging_ = false;
    repaint();
    if (pressed >= 0 && buttonAt(e.position) == pressed) press(pressed);
}

bool AboutWindow::keyPressed(const juce::KeyPress& key)
{
    if (key == juce::KeyPress::escapeKey || key == juce::KeyPress::returnKey
        || (key.getModifiers().isCommandDown() && (key.getKeyCode() == 'W' || key.getKeyCode() == 'w')))
    {
        close();
        return true;
    }
    return false;
}

void AboutWindow::press(int button)
{
    switch (button)
    {
        case gfx::brand::AboutWebsite: juce::URL(websiteUrl).launchInDefaultBrowser(); break;
        case gfx::brand::AboutGitHub: juce::URL(sourceUrl).launchInDefaultBrowser(); break;
        case gfx::brand::AboutSupport: juce::URL(supportUrl).launchInDefaultBrowser(); break;
        case gfx::brand::AboutClose: close(); break;
        default: break;
    }
}

void AboutWindow::close()
{
    setVisible(false);
    if (current == this) current = nullptr;
    juce::MessageManager::callAsync([this] { delete this; });
}

} // namespace sonora::app

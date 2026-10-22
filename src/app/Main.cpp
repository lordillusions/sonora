// Application entry point.
//
//   Sonora.app/Contents/MacOS/Sonora [path/to/set.als]
//
// Opening a .als from Finder also works (DOCUMENT_EXTENSIONS in src/CMakeLists.txt).

#include "app/AppController.h"
#include "app/MainWindow.h"
#include "app/PromptWindow.h"
#include "app/SplashWindow.h"
#include "core/AppDataFolder.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <cstdio>

namespace sonora::app
{

class SonoraApplication : public juce::JUCEApplication
{
public:
    // COM-4c: pick the data folder before anything reads settings, caches or mappings ("Sonora Nightly" never shares
    // the stable app's data).
    SonoraApplication()
    {
        // COL-3: SONORA_DATA_FOLDER runs a separate instance with its own settings and caches, e.g. a second Sonora on one
        // Mac to try a collab session (docs/collab/TESTING.md). An invalid name is ignored.
        const auto separate = juce::SystemStats::getEnvironmentVariable("SONORA_DATA_FOLDER", {}).toStdString();
        separateInstance_ = ! separate.empty() && core::setAppDataFolderName(separate);
        if (! separateInstance_) (void) core::setAppDataFolderName(SONORA_APP_DATA_FOLDER_STRING);
    }

    // src/CMakeLists.txt (COM-4): "Sonora Nightly" has its own name, so it runs next to the stable app.
    const juce::String getApplicationName() override { return SONORA_APP_NAME_STRING; }
    const juce::String getApplicationVersion() override { return SONORA_APP_VERSION_STRING; }
    // Local update test builds (docs/UPDATES.md) may run while the user's regular Sonora is open.
    bool moreThanOneInstanceAllowed() override { return SONORA_UPDATE_LOCAL_TEST != 0 || separateInstance_; }

    void initialise(const juce::String& commandLine) override
    {
        // UI-42: the splash paints before the slow startup steps and mirrors the first document's busy task.
        splash_ = std::make_unique<SplashWindow>(std::string("Version ") + SONORA_APP_VERSION_STRING);
        splash_->setStatus("Starting audio and MIDI...");
        controller_ = std::make_unique<AppController>();
        splash_->setStatus("Opening the window...");
        window_ = std::make_unique<MainWindow>(*controller_);
        splash_->toFront(false);
        if (! openFromCommandLine(commandLine))
            controller_->newProject();
        splash_->poll = [this](std::string& status, float& progress) {
            const auto& ui = controller_->ui();
            if (const auto* busy = ui.currentBusy()) { status = busy->text; progress = busy->progress; }
            return ui.song != nullptr && ! ui.waitingForBusy();
        };
        splash_->onFinished = [this] {
            juce::MessageManager::callAsync([this] { splash_.reset(); });
            if (window_) window_->toFront(true);
        };

        startQuitAutotest();

        // Cmd+O as a global shortcut (the menu item text only documents it).
        commands_.registerAllCommandsForTarget(this);
        window_->addKeyListener(commands_.getKeyMappings());
    }

    void shutdown() override
    {
        // [sonora] lines time each teardown step, so a slow quit names its cause.
        auto step = [last = juce::Time::getMillisecondCounterHiRes()](const char* what) mutable {
            const auto now = juce::Time::getMillisecondCounterHiRes();
            std::fprintf(stderr, "[sonora] quit: %s %.0f ms\n", what, now - last);
            last = now;
        };
        quitAutotest_.reset();
        splash_.reset();
        window_.reset();
        step("window");
        controller_.reset();
        step("controller");
        if (quitAutotestAnsweredAt_ > 0)
            std::fprintf(stderr, "[sonora] quit: %.0f ms from the answer\n", juce::Time::getMillisecondCounterHiRes() - quitAutotestAnsweredAt_);
    }

    void systemRequestedQuit() override
    {
        if (controller_)
            controller_->requestQuit([] { juce::JUCEApplicationBase::quit(); }); // offers to save unsaved changes first
        else
            quit();
    }

    // macOS delivers Finder "open file" events here too.
    void anotherInstanceStarted(const juce::String& commandLine) override { (void) openFromCommandLine(commandLine); }

    // ---- juce::ApplicationCommandTarget (Cmd+O)
    void getAllCommands(juce::Array<juce::CommandID>& ids) override
    {
        JUCEApplication::getAllCommands(ids);
        ids.add(kOpenCommand);
        ids.add(kAssistantCommand);
        ids.add(kSettingsCommand);
        ids.add(kCloseCommand);
    }

    void getCommandInfo(juce::CommandID id, juce::ApplicationCommandInfo& info) override
    {
        if (id == kSettingsCommand)
        {
            info.setInfo("Settings...", "Show Sonora Settings", "Sonora", 0);
            info.addDefaultKeypress(',', juce::ModifierKeys::commandModifier);
            info.setActive(! controller_->exporting());
        }
        else if (id == kCloseCommand)
        {
            info.setInfo("Close Window", "Close the current window", "File", 0);
            info.addDefaultKeypress('w', juce::ModifierKeys::commandModifier);
        }
        else if (id == kAssistantCommand)
        {
            info.setInfo("Agent Control", "Show Agent Control", "View", 0);
            info.addDefaultKeypress('p', juce::ModifierKeys::commandModifier | juce::ModifierKeys::shiftModifier);
        }
        else if (id == kOpenCommand)
        {
            info.setInfo("Open...", "Open a Live Set", "File", 0);
            info.addDefaultKeypress('o', juce::ModifierKeys::commandModifier);
            info.setActive(! controller_->exporting() && ! controller_->ui().preferencesOpen);
        }
        else
        {
            JUCEApplication::getCommandInfo(id, info);
        }
    }

    bool perform(const InvocationInfo& info) override
    {
        if (info.commandID == kSettingsCommand)
        {
            auto& ui = controller_->ui();
            ui.preferencesOpen = ! ui.preferencesOpen;
            if (ui.modelChanged) ui.modelChanged();
            return true;
        }
        if (info.commandID == kCloseCommand)
        {
            auto& ui = controller_->ui();
            if (ui.preferencesOpen) { ui.preferencesOpen = false; if (ui.modelChanged) ui.modelChanged(); }
            else systemRequestedQuit();
            return true;
        }
        if (info.commandID == kAssistantCommand) { window_->toggleAssistant(); return true; }
        if (info.commandID == kOpenCommand)
        {
            controller_->openFileDialog();
            return true;
        }
        return JUCEApplication::perform(info);
    }

private:
    bool separateInstance_ = false;

    // UI-44 verification without driving the UI: SONORA_QUIT_AUTOTEST=dont-save|save|cancel marks the open set edited
    // once it is ready, asks to quit like Cmd+Q and answers the save prompt with Cmd+D / Return / Escape. The
    // "[sonora] quit:" lines then time the teardown.
    void startQuitAutotest()
    {
        const auto answer = juce::SystemStats::getEnvironmentVariable("SONORA_QUIT_AUTOTEST", {});
        if (answer.isEmpty()) return;
        std::fprintf(stderr, "[sonora] autotest: quit, answer %s\n", answer.toRawUTF8());
        quitAutotest_ = std::make_unique<QuitAutotest>();
        quitAutotest_->poll = [this, answer] {
            const auto& ui = controller_->ui();
            if (ui.song == nullptr || ui.waitingForBusy() || splash_ != nullptr) return;
            quitAutotest_->stopTimer();
            controller_->ui().documentDirty = true;
            std::fprintf(stderr, "[sonora] autotest: set edited, quitting\n");
            systemRequestedQuit();
            juce::Timer::callAfterDelay(1500, [answer] {
                const auto key = answer == "save" ? juce::KeyPress(juce::KeyPress::returnKey)
                                 : answer == "cancel" ? juce::KeyPress(juce::KeyPress::escapeKey)
                                                      : juce::KeyPress('d', juce::ModifierKeys::commandModifier, 'd');
                std::fprintf(stderr, "[sonora] autotest: prompt %s\n", app::PromptWindow::isShowing() ? "shown, answering" : "MISSING");
                quitAutotestAnsweredAt_ = juce::Time::getMillisecondCounterHiRes();
                app::PromptWindow::pressKey(key);
            });
        };
        quitAutotest_->startTimer(200);
    }
    struct QuitAutotest : juce::Timer
    {
        std::function<void()> poll;
        void timerCallback() override { poll(); }
    };
    std::unique_ptr<QuitAutotest> quitAutotest_;
    static inline double quitAutotestAnsweredAt_ = 0;
    static constexpr juce::CommandID kOpenCommand = 0x5001;
    static constexpr juce::CommandID kAssistantCommand = 0x5002;
    static constexpr juce::CommandID kSettingsCommand = 0x5003;
    static constexpr juce::CommandID kCloseCommand = 0x5004;

    bool openFromCommandLine(const juce::String& commandLine)
    {
        const auto path = commandLine.unquoted().trim();
        if (! path.endsWithIgnoreCase(".als") || ! juce::File::isAbsolutePath(path))
            return false;
        // macOS sends the launch file both as argv and as an "open document" event.
        const auto now = juce::Time::getMillisecondCounter();
        if (path == lastOpened_ && now - lastOpenedAt_ < 3000)
            return true;
        lastOpened_ = path;
        lastOpenedAt_ = now;
        controller_->openFile(juce::File(path));
        return true;
    }

    juce::String lastOpened_;
    juce::uint32 lastOpenedAt_ = 0;
    juce::ApplicationCommandManager commands_;
    std::unique_ptr<AppController> controller_;
    std::unique_ptr<MainWindow> window_;
    std::unique_ptr<SplashWindow> splash_;
};

} // namespace sonora::app

START_JUCE_APPLICATION(sonora::app::SonoraApplication)

#pragma once
// REQ-3a: user-rebindable menu-bar commands (first step toward a full command registry, see docs/ROADMAP.md REQ-3).
//
// The app registers every menu-bar command with the default key its menu label shows. Default keys keep working through the
// existing view handlers; ShortcutMap only takes over keys the user changed:
//   - a key the user assigned runs that command (resolve() > 0);
//   - a default key the user moved away or cleared is "released" and does nothing (resolve() == kReleased);
//   - everything else is not ours (resolve() == 0) and goes to the views as before.
// Overrides are saved by a stable "Menu/Command" name, never by the menu item's numeric id.

#include "ui/Widget.h"

#include <optional>
#include <string>
#include <vector>

namespace sonora::ui
{

struct KeyCombo
{
    int key = 0; // juce::KeyPress key code, letters upper-case
    bool cmd = false, shift = false, alt = false, ctrl = false;

    bool valid() const { return key != 0; }
    bool operator==(const KeyCombo&) const = default;
    static KeyCombo fromEvent(const KeyEvent&);
    // A menu label's shortcut column ("⇧⌘T", "⌥0", "S", "⇧⌘F9", "⌘Enter", "Del"). Empty/unknown -> nullopt.
    static std::optional<KeyCombo> fromGlyphs(const std::string& utf8);
    std::string glyphs() const;   // "⌃⌥⇧⌘T" order, UTF-8
    std::string text() const;     // "Ctrl+Opt+Shift+Cmd+T" (ASCII, for settings and fonts without the symbols)
    static std::optional<KeyCombo> fromText(const std::string&);
    // Keys the views need for editing and navigation; they cannot be assigned without Cmd/Option/Control.
    bool reserved() const;
};

class ShortcutMap
{
public:
    static constexpr int kReleased = -1;
    struct Command
    {
        int id = 0;
        std::string menu, name; // "Create", "Insert Return Track"
        std::optional<KeyCombo> defaultKey;
        std::string storageName() const { return menu + "/" + name; }
    };

    void setCommands(std::vector<Command>);
    const std::vector<Command>& commands() const { return commands_; }
    const Command* command(int id) const;

    std::optional<KeyCombo> keyFor(int id) const; // effective binding
    bool isCustom(int id) const;                  // differs from its default
    // Binds `key` (nullopt clears) to `id`. Another command holding that key loses it; its id is returned (0 when none).
    int assign(int id, std::optional<KeyCombo> key);
    void reset(int id);
    void resetAll();
    int resolve(const KeyCombo&) const;

    std::string toJson() const;            // {"Create/Insert Return Track": "Opt+Cmd+R", "Edit/Rename": ""}
    void fromJson(const std::string&);     // unknown names and unparsable keys are ignored

private:
    struct Override { int id = 0; std::optional<KeyCombo> key; };
    const Override* overrideFor(int id) const;
    std::vector<Command> commands_;
    std::vector<Override> overrides_;
};

} // namespace sonora::ui

#include "ui/Shortcuts.h"

#include <juce_gui_basics/juce_gui_basics.h>

#include <algorithm>
#include <cctype>

namespace sonora::ui
{
namespace
{
int normalise(int key) { return key >= 'a' && key <= 'z' ? key - 'a' + 'A' : key; }

struct Named { const char* text; const char* glyph; int key; };
const std::vector<Named>& namedKeys()
{
    using K = juce::KeyPress;
    static const std::vector<Named> keys{
        {"Return", "↩", K::returnKey}, {"Enter", "↩", K::returnKey}, {"Tab", "⇥", K::tabKey}, {"Space", "Space", K::spaceKey},
        {"Esc", "Esc", K::escapeKey}, {"Backspace", "⌫", K::backspaceKey}, {"Del", "⌫", K::backspaceKey},
        {"Delete", "⌦", K::deleteKey}, {"Left", "←", K::leftKey}, {"Right", "→", K::rightKey}, {"Up", "↑", K::upKey},
        {"Down", "↓", K::downKey}, {"Home", "↖", K::homeKey}, {"End", "↘", K::endKey}, {"PageUp", "⇞", K::pageUpKey},
        {"PageDown", "⇟", K::pageDownKey},
        {"F1", "F1", K::F1Key}, {"F2", "F2", K::F2Key}, {"F3", "F3", K::F3Key}, {"F4", "F4", K::F4Key}, {"F5", "F5", K::F5Key},
        {"F6", "F6", K::F6Key}, {"F7", "F7", K::F7Key}, {"F8", "F8", K::F8Key}, {"F9", "F9", K::F9Key}, {"F10", "F10", K::F10Key},
        {"F11", "F11", K::F11Key}, {"F12", "F12", K::F12Key},
    };
    return keys;
}

std::optional<int> keyFromName(const std::string& name)
{
    if (name.empty()) return std::nullopt;
    for (const auto& k : namedKeys())
        if (name == k.text || name == k.glyph) return k.key;
    if (name.size() == 1 && (unsigned char) name[0] >= 33 && (unsigned char) name[0] < 127) return normalise(name[0]);
    return std::nullopt;
}

std::string keyName(int key, bool glyph)
{
    for (const auto& k : namedKeys())
        if (k.key == key && std::string(k.text) != "Enter" && std::string(k.text) != "Del") return glyph ? k.glyph : k.text;
    if (key >= 33 && key < 127) return std::string(1, (char) key);
    return "?";
}

bool consume(std::string& s, const std::string& prefix)
{
    if (s.rfind(prefix, 0) != 0) return false;
    s.erase(0, prefix.size());
    return true;
}
} // namespace

KeyCombo KeyCombo::fromEvent(const KeyEvent& k)
{
    return {normalise(k.keyCode), k.cmd, k.shift, k.alt, k.ctrl};
}

std::optional<KeyCombo> KeyCombo::fromGlyphs(const std::string& utf8)
{
    KeyCombo combo;
    std::string rest = utf8;
    for (bool more = true; more && ! rest.empty();)
        more = (consume(rest, "⌘") && (combo.cmd = true)) || (consume(rest, "⇧") && (combo.shift = true))
            || (consume(rest, "⌥") && (combo.alt = true)) || (consume(rest, "⌃") && (combo.ctrl = true));
    if (rest == "Delete") rest = "Backspace"; // menu labels name the Mac Delete key, which JUCE reports as backspace
    const auto key = keyFromName(rest);
    if (! key) return std::nullopt;
    combo.key = *key;
    return combo;
}

std::string KeyCombo::glyphs() const
{
    if (! valid()) return {};
    return std::string(ctrl ? "⌃" : "") + (alt ? "⌥" : "") + (shift ? "⇧" : "") + (cmd ? "⌘" : "") + keyName(key, true);
}

std::string KeyCombo::text() const
{
    if (! valid()) return {};
    return std::string(ctrl ? "Ctrl+" : "") + (alt ? "Opt+" : "") + (shift ? "Shift+" : "") + (cmd ? "Cmd+" : "") + keyName(key, false);
}

std::optional<KeyCombo> KeyCombo::fromText(const std::string& text)
{
    KeyCombo combo;
    std::string rest = text;
    for (bool more = true; more && rest.size() > 1;)
        more = (consume(rest, "Ctrl+") && (combo.ctrl = true)) || (consume(rest, "Opt+") && (combo.alt = true))
            || (consume(rest, "Shift+") && (combo.shift = true)) || (consume(rest, "Cmd+") && (combo.cmd = true));
    const auto key = keyFromName(rest);
    if (! key) return std::nullopt;
    combo.key = *key;
    return combo;
}

bool KeyCombo::reserved() const
{
    using K = juce::KeyPress;
    if (key == K::escapeKey) return true;
    if (cmd || alt || ctrl) return false;
    for (int k : {K::returnKey, K::tabKey, K::spaceKey, K::backspaceKey, K::deleteKey, K::leftKey, K::rightKey, K::upKey, K::downKey})
        if (key == k) return true;
    return false;
}

void ShortcutMap::setCommands(std::vector<Command> commands)
{
    commands_ = std::move(commands);
    std::erase_if(overrides_, [this](const Override& o) { return command(o.id) == nullptr; });
}

const ShortcutMap::Command* ShortcutMap::command(int id) const
{
    const auto it = std::find_if(commands_.begin(), commands_.end(), [id](const Command& c) { return c.id == id; });
    return it == commands_.end() ? nullptr : &*it;
}

const ShortcutMap::Override* ShortcutMap::overrideFor(int id) const
{
    const auto it = std::find_if(overrides_.begin(), overrides_.end(), [id](const Override& o) { return o.id == id; });
    return it == overrides_.end() ? nullptr : &*it;
}

std::optional<KeyCombo> ShortcutMap::keyFor(int id) const
{
    if (const auto* o = overrideFor(id)) return o->key;
    const auto* c = command(id);
    return c ? c->defaultKey : std::nullopt;
}

bool ShortcutMap::isCustom(int id) const
{
    const auto* o = overrideFor(id);
    const auto* c = command(id);
    return o != nullptr && c != nullptr && o->key != c->defaultKey;
}

int ShortcutMap::assign(int id, std::optional<KeyCombo> key)
{
    if (command(id) == nullptr) return 0;
    if (key && ! key->valid()) key.reset();
    int displaced = 0;
    if (key)
        for (const auto& other : commands_)
            if (other.id != id && keyFor(other.id) == key)
            {
                displaced = other.id;
                std::erase_if(overrides_, [&](const Override& o) { return o.id == other.id; });
                overrides_.push_back({other.id, std::nullopt});
            }
    std::erase_if(overrides_, [id](const Override& o) { return o.id == id; });
    overrides_.push_back({id, key});
    // Overrides equal to the default are not kept, so "custom" stays meaningful after a round trip.
    std::erase_if(overrides_, [this](const Override& o) { const auto* c = command(o.id); return c && o.key == c->defaultKey; });
    return displaced;
}

void ShortcutMap::reset(int id)
{
    const auto* c = command(id);
    if (! c) return;
    if (c->defaultKey)
        for (const auto& other : commands_) // a command that took this default gives it back
            if (other.id != id && isCustom(other.id) && keyFor(other.id) == c->defaultKey)
            {
                std::erase_if(overrides_, [&](const Override& o) { return o.id == other.id; });
                overrides_.push_back({other.id, std::nullopt});
            }
    std::erase_if(overrides_, [id](const Override& o) { return o.id == id; });
    std::erase_if(overrides_, [this](const Override& o) { const auto* cc = command(o.id); return cc && o.key == cc->defaultKey; });
}

void ShortcutMap::resetAll() { overrides_.clear(); }

int ShortcutMap::resolve(const KeyCombo& key) const
{
    if (! key.valid()) return 0;
    for (const auto& c : commands_)
        if (isCustom(c.id) && keyFor(c.id) == key) return c.id;
    for (const auto& c : commands_)
        if (c.defaultKey == key && isCustom(c.id)) return kReleased; // moved away or cleared, and nobody else took it
    return 0;
}

std::string ShortcutMap::toJson() const
{
    auto object = std::make_unique<juce::DynamicObject>();
    for (const auto& o : overrides_)
        if (const auto* c = command(o.id); c && isCustom(o.id))
            object->setProperty(juce::Identifier(juce::String::fromUTF8(c->storageName().c_str())),
                                juce::String(o.key ? o.key->text() : std::string()));
    return juce::JSON::toString(juce::var(object.release()), true).toStdString();
}

void ShortcutMap::fromJson(const std::string& json)
{
    overrides_.clear();
    const auto parsed = juce::JSON::parse(juce::String::fromUTF8(json.c_str()));
    const auto* object = parsed.getDynamicObject();
    if (! object) return;
    for (const auto& property : object->getProperties())
    {
        const auto name = property.name.toString().toStdString();
        const auto it = std::find_if(commands_.begin(), commands_.end(), [&](const Command& c) { return c.storageName() == name; });
        if (it == commands_.end()) continue;
        const auto text = property.value.toString().toStdString();
        if (text.empty()) { assign(it->id, std::nullopt); continue; }
        if (const auto key = KeyCombo::fromText(text); key && ! key->reserved()) assign(it->id, key);
    }
}

} // namespace sonora::ui

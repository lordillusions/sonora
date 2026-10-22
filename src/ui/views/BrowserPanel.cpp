#include "ui/views/BrowserPanel.h"

#include "ui/WheelScale.h"

#include "engine/AudioFileFormats.h"

#include "ui/Controls.h"
#include "gfx/Fonts.h"
#include "ui/Painter.h"

#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkRect.h"
#include "ui/RootWidget.h"
#include "ui/widgets/Splitter.h"
#include "ui/UiContext.h"
#include "core/DeviceCatalog.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>

namespace sonora::ui
{

namespace
{
constexpr float kRow = 19, kSearch = 28, kFooter = 24, kColumnHead = 18, kHeaderRow = 24, kTags = 24, kFacet = 21;
}

const std::vector<BrowserPanel::Category>& BrowserPanel::categories()
{
    static const std::vector<Category> categories{
        {"Library", true}, {"Sounds", false}, {"Drums", false}, {"Instruments", false},
        {"Audio Effects", false}, {"MIDI Effects", false}, {"Plug-ins", false}, {"Samples", false},
        {"Places", true}, {"Projects", false}, {"Current Project", false}, {"User Library", false}, {"Packs", false},
    };
    return categories;
}

Rect BrowserPanel::sidebarArea() const
{
    const float wanted = ctx().theme.metrics.browserSidebarWidth;
    return {0, kSearch, std::clamp(wanted, 90.0f, std::max(90.0f, width() * 0.7f)), height() - kSearch};
}

// The Library/Places column is resizable (drag the line between it and the list).
void BrowserPanel::layout()
{
    if (splitter_ == nullptr)
    {
        splitter_ = add<Splitter>(Splitter::Orientation::Vertical);
        splitter_->getSize = [this] { return ctx().theme.metrics.browserSidebarWidth; };
        splitter_->setSize = [this](float w) {
            ctx().theme.metrics.browserSidebarWidth = std::clamp(w, 90.0f, std::max(90.0f, width() * 0.7f));
            layout();
            repaint();
        };
        splitter_->finished = [this] { if (ctx().layoutChanged) ctx().layoutChanged(); };
    }
    const auto side = sidebarArea();
    splitter_->setBounds({side.right() - 3, side.y, 6, side.h});
}

Rect BrowserPanel::listArea() const
{
    const auto side = sidebarArea();
    const float top = kSearch + filtersHeight() + kColumnHead;
    return {side.right(), top, width() - side.w, std::max(0.0f, height() - top - kFooter - kTags)};
}

Rect BrowserPanel::previewFooter() const
{
    const float x = sidebarArea().right();
    return {x, height() - kFooter, width() - x, kFooter};
}

Rect BrowserPanel::previewButton() const
{
    const auto footer = previewFooter();
    return {footer.x + 6, footer.centreY() - 6, 12, 12};
}

Rect BrowserPanel::tagFooter() const
{
    const auto footer = previewFooter();
    return {footer.x, footer.y - kTags, footer.w, kTags};
}

Rect BrowserPanel::addTagBounds() const
{
    const auto footer = tagFooter();
    return {footer.right() - 45, footer.y + 2, 42, footer.h - 4};
}

BrowserTags::Tags BrowserPanel::selectedTags() const
{
    const auto entries = items();
    return selectedItem_ >= 0 && (size_t) selectedItem_ < entries.size() ? entries[(size_t) selectedItem_].tags : BrowserTags::Tags{};
}

Rect BrowserPanel::tagChipBounds(int index) const
{
    const auto tags = selectedTags();
    const auto footer = tagFooter();
    float x = footer.x + 35;
    for (int i = 0; i < (int) tags.size(); ++i)
    {
        const float w = gfx::measureText(tags[(size_t) i].value, ctx().theme.metrics.fontSize) + 23;
        if (x + w > addTagBounds().x - 20) return {};
        const Rect chip{x, footer.y + 3, w, footer.h - 6};
        if (i == index) return chip;
        x += w + 3;
    }
    return {};
}

std::vector<std::pair<std::string, Rect>> BrowserPanel::filterGroups() const
{
    if (! filtersVisible_) return {};
    std::set<std::string> groups;
    for (const auto& item : items(false, false))
        for (const auto& tag : item.tags) groups.insert(tag.group);
    for (const auto& tag : filters_) groups.insert(tag.group);
    std::vector<std::pair<std::string, Rect>> result;
    const float left = sidebarArea().right() + 5;
    float x = left, y = kSearch + 3;
    for (const auto& group : groups)
    {
        const float w = std::min(std::max(20.0f, width() - left - 5), gfx::measureText(group, ctx().theme.metrics.fontSize) + 23);
        if (x > left && x + w > width() - 4) { x = left; y += kFacet; }
        result.push_back({group, {x, y, w, kFacet - 3}});
        x += w + 3;
    }
    return result;
}

Rect BrowserPanel::filterGroupBounds(const std::string& group) const
{
    for (const auto& entry : filterGroups()) if (entry.first == group) return entry.second;
    return {};
}

std::vector<Rect> BrowserPanel::filterChips() const
{
    if (! filtersVisible_) return {};
    const auto groups = filterGroups();
    const float left = sidebarArea().right() + 5;
    float x = left, y = groups.empty() ? kSearch + 3 : groups.back().second.bottom() + 5;
    std::vector<Rect> result;
    for (const auto& tag : filters_)
    {
        const float w = std::min(std::max(20.0f, width() - left - 5), gfx::measureText(tag.value, ctx().theme.metrics.fontSize) + 23);
        if (x > left && x + w > width() - 4) { x = left; y += kFacet; }
        result.push_back({x, y, w, kFacet - 3});
        x += w + 3;
    }
    return result;
}

Rect BrowserPanel::filterChipBounds(int index) const
{
    const auto chips = filterChips();
    return index >= 0 && (size_t) index < chips.size() ? chips[(size_t) index] : Rect{};
}

float BrowserPanel::filtersHeight() const
{
    if (! filtersVisible_) return 0;
    const auto chips = filterChips();
    const auto groups = filterGroups();
    return chips.empty() ? (groups.empty() ? 0 : groups.back().second.bottom() + 4 - kSearch)
                         : chips.back().bottom() + 4 - kSearch;
}

void BrowserPanel::toggleTagFilter(BrowserTag tag)
{
    const auto before = items();
    const auto selectedKey = selectedItem_ >= 0 && (size_t) selectedItem_ < before.size() ? before[(size_t) selectedItem_].key : std::string{};
    const auto found = std::find(filters_.begin(), filters_.end(), tag);
    if (found != filters_.end()) filters_.erase(found); else filters_.push_back(std::move(tag));
    selectedItem_ = -1;
    const auto after = items();
    for (size_t i = 0; i < after.size(); ++i)
        if (! selectedKey.empty() && after[i].key == selectedKey) selectedItem_ = (int) i;
    scroll_ = 0;
    pendingDrag_.reset();
    tagEditing_ = false;
    if (ctx().previewSample) ctx().previewSample("");
    repaint();
}

void BrowserPanel::changeTag(const Item& item, BrowserTag tag, bool enabled)
{
    const auto entries = items();
    const auto selectedKey = selectedItem_ >= 0 && (size_t) selectedItem_ < entries.size() ? entries[(size_t) selectedItem_].key : std::string{};
    if (! ctx().browserTags.set(item.key, std::move(tag), enabled)) return;
    if (ctx().browserTagsChanged) ctx().browserTagsChanged();
    const auto changed = items();
    selectedItem_ = -1;
    for (size_t i = 0; i < changed.size(); ++i)
        if (! selectedKey.empty() && changed[i].key == selectedKey) selectedItem_ = (int) i;
    scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, (float) changed.size() * kRow - listArea().h + 8));
    pendingDrag_.reset();
    if (selectedItem_ < 0 && ctx().previewSample) ctx().previewSample("");
    repaint();
}

void BrowserPanel::tagSelection(BrowserTag tag, bool enabled)
{
    const auto entries = items();
    if (selectedItem_ >= 0 && (size_t) selectedItem_ < entries.size() && ! entries[(size_t) selectedItem_].key.empty())
        changeTag(entries[(size_t) selectedItem_], std::move(tag), enabled);
}

void BrowserPanel::showFilterMenu(const std::string& group)
{
    std::set<BrowserTag> tags;
    for (const auto& item : items(false, false))
        for (const auto& tag : item.tags) if (tag.group == group) tags.insert(tag);
    for (const auto& tag : filters_) if (tag.group == group) tags.insert(tag);
    juce::PopupMenu menu;
    for (const auto& tag : tags)
    {
        juce::PopupMenu::Item entry(tag.value);
        entry.isTicked = std::find(filters_.begin(), filters_.end(), tag) != filters_.end();
        entry.action = [this, tag] { if (Widget::isAlive(this)) toggleTagFilter(tag); };
        menu.addItem(std::move(entry));
    }
    menu.addSeparator();
    juce::PopupMenu::Item clear("Clear Filters");
    clear.isEnabled = ! filters_.empty();
    clear.action = [this] { if (Widget::isAlive(this)) { filters_.clear(); selectedItem_ = -1; scroll_ = 0; pendingDrag_.reset(); if (ctx().previewSample) ctx().previewSample(""); repaint(); } };
    menu.addItem(std::move(clear));
    menu.showMenuAsync(juce::PopupMenu::Options());
}

void BrowserPanel::beginTagEntry()
{
    const auto entries = items();
    if (selectedItem_ < 0 || (size_t) selectedItem_ >= entries.size() || entries[(size_t) selectedItem_].key.empty()) return;
    tagEditing_ = keyboardFocused_ = true;
    searchFocused_ = false; tagText_.clear();
    for (auto* ancestor = parent(); ancestor; ancestor = ancestor->parent())
        if (auto* root = dynamic_cast<RootWidget*>(ancestor)) root->setKeyboardFocus(this);
    repaint();
}

void BrowserPanel::showTagMenu()
{
    const auto entries = items();
    if (selectedItem_ < 0 || (size_t) selectedItem_ >= entries.size() || entries[(size_t) selectedItem_].key.empty()) return;
    const auto item = entries[(size_t) selectedItem_];
    std::set<BrowserTag> tags;
    for (const auto& other : items(false, false)) for (const auto& tag : other.tags) tags.insert(tag);
    for (const auto& tag : item.suggestedTags) tags.insert(tag);
    for (const auto& tag : ctx().browserTags.knownTags()) tags.insert(tag);
    juce::PopupMenu menu;
    juce::PopupMenu::Item create("New Tag...");
    create.action = [this] { if (Widget::isAlive(this)) beginTagEntry(); };
    menu.addItem(std::move(create));
    menu.addSeparator();
    for (const auto& tag : tags)
    {
        const bool selected = std::find(item.tags.begin(), item.tags.end(), tag) != item.tags.end();
        juce::PopupMenu::Item entry(tag.group + ": " + tag.value);
        entry.isTicked = selected;
        entry.action = [this, item, tag, selected] { if (Widget::isAlive(this)) changeTag(item, tag, ! selected); };
        menu.addItem(std::move(entry));
    }
    menu.showMenuAsync(juce::PopupMenu::Options());
}

// One rect per sidebar entry (categories, custom Places, "+ Add Folder"), shared by paint and hit-testing.
std::vector<Rect> BrowserPanel::sidebarRows() const
{
    const auto side = sidebarArea();
    std::vector<Rect> rows;
    float y = side.y + 4;
    const size_t total = categories().size() + ctx().browserPlaces.size() + 1;
    for (size_t i = 0; i < total; ++i)
    {
        const bool header = i < categories().size() && categories()[i].header;
        if (header && i > 0)
            y += 8;
        const float h = header ? kHeaderRow : kRow;
        rows.push_back({side.x, y, side.w, h});
        y += h;
    }
    return rows;
}

Rect BrowserPanel::sidebarRowBounds(int index) const
{
    const auto rows = sidebarRows();
    return index >= 0 && (size_t) index < rows.size() ? rows[(size_t) index] : Rect{};
}

Rect BrowserPanel::listRowBounds(int index) const
{
    const auto list = listArea();
    return {list.x, list.y + 2 + (float) index * kRow - scroll_, list.w, kRow};
}

std::vector<BrowserPanel::Item> BrowserPanel::items(bool filtered, bool groupRows) const
{
    const auto& context = ctx();
    std::vector<Item> result;
    if (folderMode_)
    {
        std::function<void(const std::vector<UiContext::BrowserFile>&, int, const std::string&)> append;
        append = [&](const auto& files, int depth, const std::string& parent) {
            if (depth > 32) return;
            for (const auto& file : files)
            {
                const juce::File path(file.path);
                const bool sample = ! file.directory && engine::isAudioFile(path);
                result.push_back({path.getFileName().toStdString(), file.path, file.directory, sample,
                                  sample ? std::optional<BrowserDrag>{{BrowserDrag::Kind::Sample, file.path, path.getFileName().toStdString()}} : std::nullopt,
                                  {}, false, depth, parent});
                const auto children = context.browserFolderChildren.find(file.path);
                if (file.directory && expandedFolders_.contains(file.path) && children != context.browserFolderChildren.end())
                    append(children->second, depth + 1, file.path);
            }
        };
        append(context.browserFiles, 0, {});
    }
    else if (selectedCategory_ == 6)
    {
        for (const auto& plugin : context.browserPlugins)
        {
            result.push_back({plugin.label, {}, false, false, BrowserDrag{BrowserDrag::Kind::Plugin, plugin.identifier, plugin.label},
                              plugin.vendor.empty() ? "Unknown Vendor" : plugin.vendor});
            auto& item = result.back();
            item.format = plugin.format == "AudioUnit" ? "AU" : plugin.format;
            item.instrument = plugin.instrument;
            if (juce::String(plugin.category).containsIgnoreCase("synth")) item.tags.push_back({"Device", "Synthesizer"});
            else if (! plugin.category.empty()) item.tags.push_back({"Device", plugin.category});
            item.tags.push_back({"Creator", item.group});
            item.tags.push_back({"Format", item.format});
            item.tags.push_back({"Type", plugin.instrument ? "Instrument" : "Audio Effect"});
        }
    }
    else if (selectedCategory_ == 9)
        for (const auto& file : context.projectFiles)
            result.push_back({juce::File(file).getFileNameWithoutExtension().toStdString(), file, false, false, {}});
    else if (selectedCategory_ >= 2 && selectedCategory_ <= 5)
    {
        const auto role = selectedCategory_ <= 3 ? model::DeviceRole::Instrument
                           : selectedCategory_ == 4 ? model::DeviceRole::AudioEffect : model::DeviceRole::MidiEffect;
        std::vector<model::CatalogEntry> entries(model::deviceCatalog().begin(), model::deviceCatalog().end());
        entries.insert(entries.end(), model::maxForLiveReplacements().begin(), model::maxForLiveReplacements().end());
        for (const auto& entry : entries)
        {
            if (! entry.implemented || entry.role != role || entry.xmlTag == "AutoFilter2") continue;
            if (selectedCategory_ == 2 && entry.xmlTag != "DrumGroupDevice" && entry.xmlTag != "SonoraImpulse") continue;
            const std::string name(entry.displayName), tag(entry.xmlTag);
            result.push_back({name, {}, false, false, BrowserDrag{BrowserDrag::Kind::NativeDevice, tag, name},
                              std::string(model::deviceCategory(entry))});
            result.back().instrument = role == model::DeviceRole::Instrument;
            result.back().tags = {{"Type", role == model::DeviceRole::Instrument ? "Instrument" : role == model::DeviceRole::MidiEffect ? "MIDI Effect" : "Audio Effect"},
                                  {"Device", result.back().group == "Synthesis" ? "Synthesizer" : result.back().group}};
        }
    }
    for (auto& item : result)
    {
        if (item.directory) continue;
        item.key = item.drag && item.drag->kind != BrowserDrag::Kind::Sample
                       ? (item.drag->kind == BrowserDrag::Kind::Plugin ? "plugin:" : "native:") + item.drag->value : "file:" + item.path;
        if (item.tags.empty())
        {
            item.tags = {{"Type", item.sample ? "Sample" : "Set"}};
            const auto extension = juce::File(item.path).getFileExtension().substring(1).toUpperCase().toStdString();
            if (! extension.empty()) item.tags.push_back({"Format", extension});
        }
        item.suggestedTags = item.tags;
        item.tags = context.browserTags.tagsFor(item.key, std::move(item.tags));
    }
    if (! query_.empty())
    {
        const auto query = juce::String::fromUTF8(query_.c_str());
        result.erase(std::remove_if(result.begin(), result.end(), [&](const Item& item) {
            std::string alias;
            if (item.drag && item.drag->kind == BrowserDrag::Kind::NativeDevice)
            {
                if (item.drag->value == "SonoraVector") alias = " Wavetable";
                else if (item.drag->value == "SonoraFm") alias = " Operator";
            }
            for (const auto& tag : item.tags) alias += " " + tag.value;
            return ! juce::String::fromUTF8((item.label + " " + item.group + alias).c_str()).containsIgnoreCase(query);
        }), result.end());
    }
    if (filtered && ! filters_.empty())
        std::erase_if(result, [&](const Item& item) { return item.directory || ! BrowserTags::matches(item.tags, filters_); });
    if (groupRows && ! folderMode_ && selectedCategory_ >= 2 && selectedCategory_ <= 6)
    {
        std::sort(result.begin(), result.end(), [](const Item& a, const Item& b) {
            const int groups = juce::String(a.group).compareIgnoreCase(juce::String(b.group));
            return groups == 0 ? juce::String(a.label).compareIgnoreCase(juce::String(b.label)) < 0 : groups < 0;
        });
        std::vector<Item> grouped;
        std::string previous;
        for (auto& item : result)
        {
            if (juce::String(item.group).compareIgnoreCase(juce::String(previous)) != 0)
            {
                previous = item.group;
                grouped.push_back({previous, {}, false, false, {}, previous, true});
            }
            if (! query_.empty() || ! filters_.empty() || expandedGroups_.contains(item.group)) grouped.push_back(std::move(item));
        }
        return grouped;
    }
    return result;
}

namespace
{
// Small monochrome glyphs for the sidebar, drawn from simple shapes in a 14x14 box.
// Original geometry: format name above a keyboard (instrument) or signal/controls (effect).
void drawPluginIcon(SkCanvas& c, const Rect& box, const std::string& format, bool instrument, SkColor fg, SkColor bg)
{
    const auto name = juce::String(format).toUpperCase();
    const bool vst = name.startsWith("VST");
    paint::stroke(c, box, fg, 1);
    paint::fill(c, {box.x, box.y, box.w, 7}, fg);
    paint::text(c, vst ? "VST" : name.toStdString(), {box.x, box.y - 1, box.w, 8}, bg, 6, Align::Centre, true);
    if (instrument)
    {
        for (int i = 1; i < 6; ++i) paint::line(c, box.x + (float) i * 2.5f, box.y + 8, box.x + (float) i * 2.5f, box.bottom(), fg, 0.7f);
        for (const int i : {1, 2, 4, 5}) paint::fill(c, {box.x + (float) i * 2.5f - 0.5f, box.y + 7, 1.5f, 3.5f}, fg);
    }
    else
    {
        paint::line(c, box.x + 2, box.y + 10, box.right() - 2, box.y + 10, fg, 1);
        for (const float x : {box.x + 5, box.x + 12}) paint::fill(c, {x, box.y + 8, 1.5f, 4}, fg);
    }
    if (name.contains("3"))
    {
        const Rect badge{box.right() - 5, box.bottom() - 6, 7, 8};
        paint::fill(c, badge, bg); paint::stroke(c, badge, fg, 0.7f);
        paint::text(c, "3", badge, fg, 6, Align::Centre, true);
    }
}

enum class Glyph { None, Library, Sounds, Drums, Instruments, AudioEffects, MidiEffects, PlugIn, Samples, Folder, Pack, Add };

void drawGlyph(SkCanvas& c, Glyph g, const Rect& box, SkColor color)
{
    SkPaint line;
    line.setAntiAlias(true);
    line.setColor(color);
    line.setStyle(SkPaint::kStroke_Style);
    line.setStrokeWidth(1.3f);
    SkPaint solid = line;
    solid.setStyle(SkPaint::kFill_Style);
    const float x = box.x, y = box.y, w = box.w, h = box.h;
    switch (g)
    {
        case Glyph::Library:
            for (int i = 0; i < 3; ++i)
            {
                const float fi = (float) i;
                c.drawLine(x + 2 + fi * 3.2f, y + 2, x + 2 + fi * 3.2f, y + h - 2, line);
            }
            c.drawLine(x + 10.5f, y + 2, x + w - 1.5f, y + h - 2, line);
            break;
        case Glyph::Sounds:
            c.drawOval(SkRect::MakeXYWH(x + 1.5f, y + h - 5.5f, 5, 4), solid);
            c.drawLine(x + 6.2f, y + h - 3.5f, x + 6.2f, y + 2, line);
            c.drawLine(x + 6.2f, y + 2, x + w - 2, y + 4.5f, line);
            break;
        case Glyph::Drums:
            c.drawOval(SkRect::MakeXYWH(x + 1.5f, y + 2.5f, w - 3, 4), line);
            c.drawLine(x + 1.5f, y + 4.5f, x + 1.5f, y + h - 3.5f, line);
            c.drawLine(x + w - 1.5f, y + 4.5f, x + w - 1.5f, y + h - 3.5f, line);
            c.drawArc(SkRect::MakeXYWH(x + 1.5f, y + h - 6.5f, w - 3, 4), 0, 180, false, line);
            break;
        case Glyph::Instruments:
        case Glyph::MidiEffects:
            c.drawRect(SkRect::MakeXYWH(x + 1, y + 3, w - 2, h - 6), line);
            for (int i = 0; i < 3; ++i)
                c.drawRect(SkRect::MakeXYWH(x + 3.5f + (float) i * 3.2f, y + 3, 1.8f, (h - 6) * 0.6f), solid);
            if (g == Glyph::MidiEffects)
                c.drawLine(x + 1, y + h - 1.5f, x + w - 1, y + h - 1.5f, line);
            break;
        case Glyph::AudioEffects:
            for (int i = 0; i < 5; ++i)
                for (int j = 0; j < 3; ++j)
                    c.drawRect(SkRect::MakeXYWH(x + 1.5f + (float) i * 2.6f, y + 2 + (float) j * 4, 1.2f, 2.6f), solid);
            break;
        case Glyph::PlugIn:
            c.drawRect(SkRect::MakeXYWH(x + 2.5f, y + 5, w - 5, h - 8), line);
            c.drawLine(x + 5, y + 5, x + 5, y + 1.5f, line);
            c.drawLine(x + w - 5, y + 5, x + w - 5, y + 1.5f, line);
            break;
        case Glyph::Samples:
        {
            const float hs[] = {3, 7, 10, 6, 9, 4, 2};
            for (int i = 0; i < 7; ++i)
            {
                const float px = x + 1.5f + (float) i * 1.8f;
                c.drawLine(px, y + h * 0.5f - hs[i] * 0.5f, px, y + h * 0.5f + hs[i] * 0.5f, line);
            }
            break;
        }
        case Glyph::Folder:
            c.drawRect(SkRect::MakeXYWH(x + 1, y + 3.5f, w - 2, h - 6.5f), line);
            c.drawLine(x + 1, y + 3.5f, x + 4, y + 1.5f, line);
            c.drawLine(x + 4, y + 1.5f, x + 6.5f, y + 1.5f, line);
            break;
        case Glyph::Pack:
            c.drawRect(SkRect::MakeXYWH(x + 1.5f, y + 1.5f, 4.5f, 4.5f), line);
            c.drawRect(SkRect::MakeXYWH(x + 8, y + 1.5f, 4.5f, 4.5f), line);
            c.drawRect(SkRect::MakeXYWH(x + 1.5f, y + 8, 4.5f, 4.5f), line);
            c.drawRect(SkRect::MakeXYWH(x + 8, y + 8, 4.5f, 4.5f), line);
            break;
        case Glyph::Add:
            c.drawLine(x + 3, y + h * 0.5f, x + w - 3, y + h * 0.5f, line);
            c.drawLine(x + w * 0.5f, y + 3, x + w * 0.5f, y + h - 3, line);
            break;
        case Glyph::None: break;
    }
}

Glyph glyphFor(const std::string& name)
{
    if (name == "Sounds") return Glyph::Sounds;
    if (name == "Drums") return Glyph::Drums;
    if (name == "Instruments") return Glyph::Instruments;
    if (name == "Audio Effects") return Glyph::AudioEffects;
    if (name == "MIDI Effects") return Glyph::MidiEffects;
    if (name == "Plug-ins") return Glyph::PlugIn;
    if (name == "Samples") return Glyph::Samples;
    if (name == "Packs") return Glyph::Pack;
    if (name == "Projects" || name == "Current Project" || name == "User Library") return Glyph::Folder;
    return Glyph::Library;
}
} // namespace

void BrowserPanel::paint(SkCanvas& canvas, const Theme& theme)
{
    const SkColor fg = theme.color("ControlForeground");
    const SkColor selectedBg = keyboardFocused_ ? theme.color("SelectionBackground") : theme.color("StandbySelectionBackground");
    paint::fill(canvas, localBounds(), theme.color("SurfaceBackground"));

    // Search row: back/forward-style navigation boxes, the search field and a filter box.
    {
        Rect row{0, 0, width(), kSearch};
        row = row.reduced(5, 4);
        const auto up = row.removeFromLeft(18);
        row.removeFromLeft(2);
        const auto fwd = row.removeFromLeft(18);
        row.removeFromLeft(4);
        const auto filter = row.removeFromRight(18);
        row.removeFromRight(4);
        ctl::framed(canvas, theme, up, theme.color("SurfaceBackground"));
        ctl::triangle(canvas, up.centreX(), up.centreY(), 8, 5, ctl::Dir::Left, folderMode_ ? fg : theme.color("TextDisabled"));
        ctl::framed(canvas, theme, fwd, theme.color("SurfaceBackground"));
        ctl::triangle(canvas, fwd.centreX(), fwd.centreY(), 8, 5, ctl::Dir::Right, theme.color("TextDisabled"));
        paint::fill(canvas, row, theme.color("SurfaceBackground"));
        paint::stroke(canvas, row, searchFocused_ ? theme.color("SelectionFrame") : theme.color("SurfaceArea"));
        paint::text(canvas, query_.empty() ? "Search (Cmd + F)" : query_, row.reduced(6, 0),
                    query_.empty() ? theme.color("TextDisabled") : fg, theme.metrics.fontSize);
        ctl::framed(canvas, theme, filter, theme.color(filtersVisible_ ? "ChosenDefault" : "ControlTextBack"));
        for (int i = 0; i < 3; ++i)
        {
            const float fi = (float) i;
            paint::fill(canvas, {filter.x + 4 + fi * 1.5f, filter.y + 5 + fi * 3.0f, filter.w - 8 - fi * 3.0f, 1.2f}, fg);
        }
    }

    const auto side = sidebarArea();
    const auto rows = sidebarRows();
    for (size_t i = 0; i < rows.size(); ++i)
    {
        const bool custom = i >= categories().size(), add = i == categories().size() + ctx().browserPlaces.size();
        const bool header = ! custom && categories()[i].header;
        const std::string label = add ? "Add Folder..." : custom ? juce::File(ctx().browserPlaces[i - categories().size()]).getFileName().toStdString() : categories()[i].name;
        const Rect row = rows[i];
        const bool selected = (int) i == selectedCategory_;
        if (selected)
            paint::fill(canvas, row, selectedBg);
        if (header)
        {
            paint::text(canvas, label, row.reduced(10, 0).translated(0, 3), theme.color("TreeRowCategoryForeground"),
                        theme.metrics.fontSize, Align::Left, true);
            continue;
        }
        drawGlyph(canvas, add ? Glyph::Add : custom ? Glyph::Folder : glyphFor(label), {row.x + 10, row.centreY() - 7, 14, 14}, fg);
        paint::text(canvas, label, Rect{row.x + 32, row.y, row.w - 36, row.h}, fg, theme.metrics.fontSize);
    }
    paint::fill(canvas, {side.right() - 1, side.y, 1, side.h}, paint::withAlpha(theme.color("SurfaceArea"), 110));

    // Facet selectors and removable active filters stay above the results column.
    for (const auto& [group, box] : filterGroups())
    {
        const bool active = std::any_of(filters_.begin(), filters_.end(), [&](const auto& tag) { return tag.group == group; });
        ctl::framed(canvas, theme, box, theme.color(active ? "ChosenDefault" : "ControlTextBack"));
        paint::text(canvas, group, {box.x + 5, box.y, box.w - 17, box.h}, fg, theme.metrics.fontSize);
        ctl::triangle(canvas, box.right() - 7, box.centreY(), 5, 3, ctl::Dir::Down, fg);
    }
    const auto filterBoxes = filterChips();
    for (size_t i = 0; i < filterBoxes.size(); ++i)
    {
        const auto box = filterBoxes[i];
        paint::fill(canvas, box, theme.color("SelectionBackground"));
        paint::text(canvas, filters_[i].value, {box.x + 4, box.y, box.w - 18, box.h}, fg, theme.metrics.fontSize);
        paint::text(canvas, "×", {box.right() - 14, box.y, 12, box.h}, fg, theme.metrics.fontSize);
    }

    // Content list with a "Name" column header.
    const auto list = listArea();
    {
        const Rect head{list.x, list.y - kColumnHead, list.w, kColumnHead};
        paint::fill(canvas, head, theme.color("TreeColumnHeadBackground"));
        paint::text(canvas, "Name", head.reduced(8, 0), theme.color("TreeColumnHeadForeground"), theme.metrics.fontSize, Align::Left, true);
        ctl::triangle(canvas, head.right() - 30, head.centreY(), 7, 5, ctl::Dir::Up, theme.color("TreeColumnHeadControl") );
    }
    const auto entries = items();
    canvas.save();
    canvas.clipRect(list.sk());
    float y = list.y + 2 - scroll_;
    for (size_t i = 0; i < entries.size(); ++i, y += kRow)
    {
        if (y + kRow < list.y || y > list.bottom()) continue;
        const Rect row{list.x, y, list.w, kRow};
        const bool selected = (int) i == selectedItem_;
        if (selected) paint::fill(canvas, row, selectedBg);
        const auto& item = entries[i];
        if (item.directory || item.heading)
            ctl::triangle(canvas, row.x + 12 + (float) item.depth * 14, row.centreY(), 8, 5,
                          ((item.heading && (! query_.empty() || ! filters_.empty() || expandedGroups_.contains(item.group)))
                           || (item.directory && expandedFolders_.contains(item.path))) ? ctl::Dir::Down : ctl::Dir::Right,
                          theme.color("TreeRowCategoryForeground"));
        float inset = (! item.heading && ! item.group.empty() ? 34.0f : 24.0f) + (float) item.depth * 14;
        if (! item.format.empty())
        {
            drawPluginIcon(canvas, {row.x + inset, row.centreY() - 7, 17, 14}, item.format, item.instrument, fg,
                           selected ? selectedBg : theme.color("SurfaceBackground"));
            inset += 24;
        }
        paint::text(canvas, item.label, Rect{row.x + inset, row.y, row.w - inset - 4, row.h}, fg,
                    theme.metrics.fontSize, Align::Left, item.heading);
    }
    if (entries.empty())
        paint::text(canvas, ctx().browserLoading && folderMode_ ? "Loading..." : "No results", {list.x + 8, list.y, list.w - 12, 30}, theme.color("TextDisabled"), theme.metrics.fontSize);
    canvas.restore();

    const auto tagsRow = tagFooter();
    canvas.save(); canvas.clipRect(tagsRow.sk());
    paint::fill(canvas, tagsRow, theme.color("SurfaceBackground"));
    paint::fill(canvas, {tagsRow.x, tagsRow.y, tagsRow.w, 1}, theme.color("SurfaceArea"));
    paint::text(canvas, "Tags:", {tagsRow.x + 6, tagsRow.y, 30, tagsRow.h}, fg, theme.metrics.fontSize);
    if (tagEditing_)
    {
        const Rect field{tagsRow.x + 35, tagsRow.y + 3, std::max(0.0f, tagsRow.w - 39), tagsRow.h - 6};
        ctl::framed(canvas, theme, field, theme.color("ControlTextBack"));
        paint::text(canvas, tagText_.empty() ? "New tag (Enter to add)" : tagText_, field.reduced(4, 0), fg, theme.metrics.fontSize);
    }
    else
    {
        const auto tags = selectedTags();
        for (size_t i = 0; i < tags.size(); ++i)
        {
            const auto chip = tagChipBounds((int) i);
            if (chip.isEmpty())
            {
                paint::text(canvas, "…", {addTagBounds().x - 20, tagsRow.y, 18, tagsRow.h}, fg, theme.metrics.fontSize);
                break;
            }
            paint::fill(canvas, chip, theme.color("ControlTextBack"));
            paint::text(canvas, tags[i].value, {chip.x + 4, chip.y, chip.w - 19, chip.h}, fg, theme.metrics.fontSize);
            paint::text(canvas, "×", {chip.right() - 14, chip.y, 12, chip.h}, fg, theme.metrics.fontSize);
        }
        paint::text(canvas, "Add...", addTagBounds(), theme.color(selectedItem_ < 0 ? "TextDisabled" : "ControlForeground"), theme.metrics.fontSize);
    }

    canvas.restore();
    const Rect footer = previewFooter();
    paint::fill(canvas, footer, theme.color("SurfaceBackground"));
    paint::fill(canvas, {footer.x, footer.y, footer.w, 1}, paint::withAlpha(theme.color("SurfaceArea"), 110));
    const Rect button = previewButton();
    SkPaint p;
    p.setAntiAlias(true);
    p.setColor(theme.color(ctx().browserPreviewEnabled ? "ChosenPreListen" : "SurfaceBackground"));
    canvas.drawCircle(button.centreX(), button.centreY(), 5.8f, p);
    p.setColor(fg);
    p.setStyle(SkPaint::kStroke_Style);
    p.setStrokeWidth(0.8f);
    canvas.drawCircle(button.centreX(), button.centreY(), 5.8f, p);
    // Headphones: a headband with two short ear cups, drawn from our own geometry.
    canvas.drawArc(SkRect::MakeXYWH(button.x + 3, button.y + 2.8f, 6, 6.5f), 180, 180, false, p);
    p.setStrokeWidth(1.6f);
    canvas.drawLine(button.x + 3, button.y + 5.5f, button.x + 3, button.y + 8, p);
    canvas.drawLine(button.x + 9, button.y + 5.5f, button.x + 9, button.y + 8, p);
    const bool selected = selectedItem_ >= 0 && (size_t) selectedItem_ < entries.size();
    paint::text(canvas, selected ? "1 item selected" : "0 items selected",
                {button.right() + 6, footer.y, std::max(0.0f, footer.right() - button.right() - 12), footer.h},
                fg, theme.metrics.fontSize, Align::Centre);

}

void BrowserPanel::navigate(const std::string& path)
{
    folderMode_ = true;
    selectedItem_ = -1;
    scroll_ = 0;
    if (ctx().openBrowserFolder) ctx().openBrowserFolder(path);
}

void BrowserPanel::selectCategory(int index)
{
    selectedCategory_ = index;
    filters_.clear(); tagEditing_ = false;
    selectedItem_ = -1;
    scroll_ = 0;
    folderMode_ = false;
    if (index >= (int) categories().size()) navigate(ctx().browserPlaces[(size_t) index - categories().size()]);
    else if (index == 10 && ctx().song) navigate(juce::File(ctx().song->sourcePath).getParentDirectory().getFullPathName().toStdString());
    else if (index == 11) navigate(ctx().userLibraryPath.empty() ? juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("Ableton/User Library").getFullPathName().toStdString() : ctx().userLibraryPath);
    else if (index == 12) navigate(ctx().packsPath.empty() ? "/Users/Shared/Ableton/Factory Packs" : ctx().packsPath);
    else if (index == 7) navigate(juce::File::getSpecialLocation(juce::File::userMusicDirectory).getFullPathName().toStdString());
}

void BrowserPanel::disclose(const Item& item, bool open)
{
    if (item.heading)
    {
        if (open) expandedGroups_.insert(item.group); else expandedGroups_.erase(item.group);
    }
    else if (item.directory)
    {
        if (open)
        {
            const bool inserted = expandedFolders_.insert(item.path).second;
            if (inserted && ! ctx().browserFolderChildren.contains(item.path) && ctx().expandBrowserFolder)
                ctx().expandBrowserFolder(item.path);
        }
        else expandedFolders_.erase(item.path);
    }
    scroll_ = std::clamp(scroll_, 0.0f, std::max(0.0f, (float) items().size() * kRow - listArea().h + 8));
    repaint();
}

void BrowserPanel::activate(const Item& item)
{
    if (item.heading)
    {
        if (! expandedGroups_.erase(item.group)) expandedGroups_.insert(item.group);
        repaint();
    }
    else if (item.directory) disclose(item, ! expandedFolders_.contains(item.path));
    else if (item.sample && ctx().previewSample) ctx().previewSample(item.path);
    else if (! item.path.empty() && ctx().openFile) ctx().openFile(item.path);
    else if (item.drag && ctx().dropBrowserItem) ctx().dropBrowserItem(*item.drag, ctx().selectedTrack, 0, -1);
}

void BrowserPanel::showItemMenu(Item item)
{
    juce::PopupMenu menu;
    if (item.heading || item.directory)
    {
        const bool expanded = item.heading ? expandedGroups_.contains(item.group) : expandedFolders_.contains(item.path);
        juce::PopupMenu::Item entry(expanded ? "Collapse" : "Expand");
        entry.action = [this, item, expanded] { if (Widget::isAlive(this)) disclose(item, ! expanded); };
        menu.addItem(std::move(entry));
    }
    else
    {
        juce::PopupMenu::Item load(item.path.empty() || item.sample ? "Load" : "Open Set");
        load.action = [this, item] {
            if (! Widget::isAlive(this)) return;
            if (item.sample && item.drag && ctx().dropBrowserItem) ctx().dropBrowserItem(*item.drag, ctx().selectedTrack, 0, -1);
            else activate(item);
        };
        menu.addItem(std::move(load));
    }
    if (! item.path.empty())
    {
        menu.addSeparator();
        juce::PopupMenu::Item reveal("Show in Finder");
        reveal.isEnabled = static_cast<bool>(ctx().revealInFinder);
        reveal.action = [this, path = item.path] { if (Widget::isAlive(this) && ctx().revealInFinder) ctx().revealInFinder(path); };
        menu.addItem(std::move(reveal));
    }
    menu.showMenuAsync(juce::PopupMenu::Options());
}

void BrowserPanel::mouseDown(const MouseEvent& event)
{
    pendingDrag_.reset();
    dragStartX_ = event.x;
    dragStartY_ = event.y;
    keyboardFocused_ = true;
    if (! tagFooter().contains(event.x, event.y)) tagEditing_ = false;
    searchFocused_ = event.y < kSearch && event.x >= 47 && event.x < width() - 27;
    if (event.y < kSearch)
    {
        if (event.x >= width() - 27)
        {
            filtersVisible_ = ! filtersVisible_; scroll_ = 0;
        }
        else if (event.x >= 5 && event.x < 23 && folderMode_)
            navigate(juce::File(ctx().browserFolder).getParentDirectory().getFullPathName().toStdString());
        repaint();
        return;
    }
    for (const auto& [group, box] : filterGroups())
        if (box.contains(event.x, event.y)) { showFilterMenu(group); return; }
    const auto filterBoxes = filterChips();
    for (size_t i = 0; i < filterBoxes.size(); ++i)
        if (filterBoxes[i].contains(event.x, event.y)) { toggleTagFilter(filters_[i]); return; }
    if (tagFooter().contains(event.x, event.y))
    {
        if (event.right) { showTagMenu(); return; }
        if (! tagEditing_)
        {
            const auto tags = selectedTags();
            for (size_t i = 0; i < tags.size(); ++i)
                if (tagChipBounds((int) i).contains(event.x, event.y))
                {
                    const auto box = tagChipBounds((int) i);
                    if (event.x >= box.right() - 14) tagSelection(tags[i], false); else toggleTagFilter(tags[i]);
                    return;
                }
            showTagMenu();
        }
        return;
    }
    if (previewFooter().contains(event.x, event.y))
    {
        if (previewButton().reduced(-3, -3).contains(event.x, event.y))
        {
            ctx().browserPreviewEnabled = ! ctx().browserPreviewEnabled;
            if (ctx().layoutChanged) ctx().layoutChanged();
            if (! ctx().browserPreviewEnabled && ctx().previewSample) ctx().previewSample("");
            else if (ctx().browserPreviewEnabled && ctx().previewSample && selectedItem_ >= 0)
            {
                const auto entries = items();
                if ((size_t) selectedItem_ < entries.size() && entries[(size_t) selectedItem_].sample) ctx().previewSample(entries[(size_t) selectedItem_].path);
            }
        }
    }
    else if (sidebarArea().contains(event.x, event.y))
    {
        const auto rows = sidebarRows();
        const int count = (int) (categories().size() + ctx().browserPlaces.size());
        for (int index = 0; index < (int) rows.size(); ++index)
            if (rows[(size_t) index].contains(event.x, event.y))
            {
                if (index == count) { if (ctx().addBrowserFolder) ctx().addBrowserFolder(); }
                else if (index >= (int) categories().size() || ! categories()[(size_t) index].header) selectCategory(index);
                break;
            }
    }
    else if (listArea().contains(event.x, event.y))
    {
        selectedItem_ = (int) ((event.y - listArea().y - 2 + scroll_) / kRow);
        const auto entries = items();
        if (event.right && selectedItem_ >= 0 && (size_t) selectedItem_ < entries.size())
        { showItemMenu(entries[(size_t) selectedItem_]); repaint(); return; }
        if (! event.right && selectedItem_ >= 0 && (size_t) selectedItem_ < entries.size())
        {
            const auto& item = entries[(size_t) selectedItem_];
            if (item.heading || (item.directory && event.x < listArea().x + 22 + (float) item.depth * 14)) activate(item);
            else pendingDrag_ = item.drag;
        }
        if (selectedItem_ >= 0 && (size_t) selectedItem_ < entries.size() && entries[(size_t) selectedItem_].sample
            && ctx().browserPreviewEnabled && ctx().previewSample) ctx().previewSample(entries[(size_t) selectedItem_].path);
    }
    repaint();
}

void BrowserPanel::mouseDrag(const MouseEvent& event)
{
    if (pendingDrag_ && std::hypot(event.x - dragStartX_, event.y - dragStartY_) >= 5)
    {
        ctx().browserDrag = pendingDrag_;
        if (ctx().previewSample) ctx().previewSample("");
        pendingDrag_.reset();
        repaint();
    }
}

void BrowserPanel::mouseDoubleClick(const MouseEvent& event)
{
    if (event.right) { mouseDown(event); return; }
    const auto before = items();
    const int row = (int) ((event.y - listArea().y - 2 + scroll_) / kRow);
    if (listArea().contains(event.x, event.y) && row >= 0 && (size_t) row < before.size() && (before[(size_t) row].heading || (before[(size_t) row].directory && event.x < listArea().x + 22 + (float) before[(size_t) row].depth * 14))) return;
    mouseDown(event);
    if (! listArea().contains(event.x, event.y)) return;
    const auto entries = items();
    if (selectedItem_ >= 0 && (size_t) selectedItem_ < entries.size() && ! entries[(size_t) selectedItem_].heading)
        activate(entries[(size_t) selectedItem_]);
}

void BrowserPanel::focusSearch()
{
    ctx().browserVisible = true;
    tagEditing_ = false;
    searchFocused_ = keyboardFocused_ = true;
    for (auto* ancestor = parent(); ancestor; ancestor = ancestor->parent())
        if (auto* root = dynamic_cast<RootWidget*>(ancestor)) root->setKeyboardFocus(this);
    repaint();
}

// Up / Down walk the list (also while the search field has the focus), scrolling the row into view and auditioning samples.
void BrowserPanel::moveSelection(int delta)
{
    const auto entries = items();
    if (entries.empty())
        return;
    selectedItem_ = std::clamp(selectedItem_ < 0 ? (delta > 0 ? 0 : (int) entries.size() - 1) : selectedItem_ + delta, 0, (int) entries.size() - 1);
    const float top = (float) selectedItem_ * kRow, bottom = top + kRow;
    const float viewHeight = listArea().h - 4;
    if (top < scroll_) scroll_ = top;
    else if (bottom > scroll_ + viewHeight) scroll_ = bottom - viewHeight;
    scroll_ = std::max(0.0f, scroll_);
    const auto& item = entries[(size_t) selectedItem_];
    if (item.sample && ctx().browserPreviewEnabled && ctx().previewSample) ctx().previewSample(item.path);
    else if (! item.sample && ctx().previewSample) ctx().previewSample("");
    repaint();
}

bool BrowserPanel::keyPressed(const KeyEvent& key)
{
    if (key.cmd && ! key.alt && ! key.ctrl && ! key.shift && (key.textChar == 'f' || key.textChar == 'F')) { focusSearch(); return true; }
    if (! keyboardFocused_) return false;
    if (tagEditing_)
    {
        if (key.keyCode == juce::KeyPress::escapeKey) tagEditing_ = false;
        else if (key.keyCode == juce::KeyPress::returnKey)
        {
            tagSelection({"User", tagText_}, true); tagEditing_ = false;
        }
        else if (key.keyCode == juce::KeyPress::backspaceKey) paint::popLastCodePoint(tagText_);
        else if (key.cmd && (key.textChar == 'v' || key.textChar == 'V'))
            tagText_ = (juce::String(tagText_) + juce::SystemClipboard::getTextFromClipboard()).substring(0, 80).toStdString();
        else if (! key.cmd && ! key.alt && key.textChar >= 32 && key.textChar != 127 && juce::String(tagText_).length() < 80)
            tagText_ += juce::String::charToString((juce::juce_wchar) key.textChar).toStdString();
        repaint(); return true;
    }
    if (key.keyCode == juce::KeyPress::escapeKey) { searchFocused_ = keyboardFocused_ = false; repaint(); return true; }
    if (! key.cmd && ! key.alt && (key.keyCode == juce::KeyPress::upKey || key.keyCode == juce::KeyPress::downKey))
    {
        moveSelection(key.keyCode == juce::KeyPress::upKey ? -1 : 1);
        return true;
    }
    if (! searchFocused_ && ! key.cmd && ! key.alt
        && (key.keyCode == juce::KeyPress::leftKey || key.keyCode == juce::KeyPress::rightKey))
    {
        const auto entries = items();
        if (selectedItem_ >= 0 && (size_t) selectedItem_ < entries.size())
        {
            const auto item = entries[(size_t) selectedItem_];
            if (key.keyCode == juce::KeyPress::rightKey)
            {
                if (item.heading || item.directory) disclose(item, true);
                else if (item.sample && ctx().previewSample) ctx().previewSample(item.path);
            }
            else if ((item.heading && expandedGroups_.contains(item.group)) || (item.directory && expandedFolders_.contains(item.path)))
                disclose(item, false);
            else
                for (int i = selectedItem_ - 1; i >= 0; --i)
                    if ((! item.parentFolder.empty() && entries[(size_t) i].path == item.parentFolder)
                        || (! item.group.empty() && entries[(size_t) i].heading && entries[(size_t) i].group == item.group))
                    { selectedItem_ = i; break; }
        }
        repaint();
        return true;
    }
    if (searchFocused_ && ! key.cmd && ! key.alt)
    {
        if (key.keyCode == juce::KeyPress::backspaceKey) paint::popLastCodePoint(query_);
        else if (key.textChar >= 32 && key.textChar != 127) query_ += juce::String::charToString((juce::juce_wchar) key.textChar).toStdString();
        else if (key.keyCode == juce::KeyPress::returnKey) searchFocused_ = false;
        else return false;
        selectedItem_ = -1;
        scroll_ = 0;
        repaint();
        return true;
    }
    if (! searchFocused_ && key.keyCode == juce::KeyPress::returnKey)
    {
        const auto entries = items();
        if (selectedItem_ >= 0 && (size_t) selectedItem_ < entries.size()) activate(entries[(size_t) selectedItem_]);
        return true;
    }
    return false;
}

bool BrowserPanel::mouseWheel(const MouseEvent& event)
{
    const float maximum = std::max(0.0f, (float) items().size() * kRow - listArea().h + 8);
    scroll_ = std::clamp(scroll_ - event.wheelDeltaY * kWheelPixels, 0.0f, maximum);
    repaint();
    return true;
}

} // namespace sonora::ui

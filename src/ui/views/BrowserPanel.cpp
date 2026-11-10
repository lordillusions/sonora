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
#include "ui/SampleFiles.h"
#include "ui/widgets/Splitter.h"
#include "ui/UiContext.h"
#include "core/DeviceCatalog.h"

#include <juce_gui_basics/juce_gui_basics.h>
#include <cmath>
#include <span>

namespace sonora::ui
{

namespace
{
constexpr float kRow = 19, kSearch = 28, kFooter = 24, kColumnHead = 18, kHeaderRow = 24, kTags = 24, kFacet = 21;

// The folder the Samples category (and All's Samples section) lists.
std::string samplesRoot()
{
    return juce::File::getSpecialLocation(juce::File::userMusicDirectory).getFullPathName().toStdString();
}

// LIB-1 Packs list helpers.
const UiContext::SamplePack* findPack(const UiContext& context, const std::string& id)
{
    for (const auto& pack : context.samplePacks)
        if (pack.id == id) return &pack;
    return nullptr;
}

std::string megabytes(int64_t bytes)
{
    char text[32];
    std::snprintf(text, sizeof text, bytes < 10'000'000 ? "%.1f MB" : "%.0f MB", (double) bytes / 1.0e6);
    return text;
}

// The Packs row's button and status column: the status column only when the list is wide enough to keep the name readable.
constexpr float kPackButton = 64, kPackInfo = 104, kPackWide = 360;

// LIB-2: the presets of one device (UiContext::devicePresets is sorted by device tag).
using Preset = UiContext::BrowserPreset;
std::span<const Preset> presetsOf(const std::vector<Preset>& presets, const std::string& tag)
{
    const auto first = std::partition_point(presets.begin(), presets.end(), [&](const Preset& p) { return p.deviceTag < tag; });
    const auto last = std::partition_point(first, presets.end(), [&](const Preset& p) { return p.deviceTag == tag; });
    return {first, last};
}

bool presetMatches(const Preset& preset, const juce::String& query)
{
    return juce::String::fromUTF8(preset.name.c_str()).containsIgnoreCase(query)
        || juce::String::fromUTF8(preset.folder.c_str()).containsIgnoreCase(query);
}

BrowserTags::Tags presetTags(const Preset& preset)
{
    return {{"Type", "Preset"}, {"Sound", preset.folder.empty() ? preset.deviceName : preset.folder}, {"Device", preset.deviceName}};
}
}

const std::vector<BrowserPanel::Category>& BrowserPanel::categories()
{
    static const std::vector<Category> categories{
        {"Library", true}, {"All", false}, {"Sounds", false}, {"Drums", false}, {"Instruments", false},
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
    const auto& entries = items();
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
    const auto& entries = items(false, false); // validates the cache before reading its facets
    if (! facetGroups_)
    {
        facetGroups_.emplace();
        for (const auto& item : entries)
            for (const auto& tag : item.tags) facetGroups_->insert(tag.group);
        for (const auto& tag : filters_) facetGroups_->insert(tag.group);
    }
    std::vector<std::pair<std::string, Rect>> result;
    const float left = sidebarArea().right() + 5;
    float x = left, y = kSearch + 3 + similarRowHeight();
    for (const auto& group : *facetGroups_)
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
    float x = left, y = groups.empty() ? kSearch + 3 + similarRowHeight() : groups.back().second.bottom() + 5;
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
    if (! filtersVisible_) return similarRowHeight();
    const auto chips = filterChips();
    const auto groups = filterGroups();
    return chips.empty() ? (groups.empty() ? similarRowHeight() : groups.back().second.bottom() + 4 - kSearch)
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
    restartCaret();
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

std::string BrowserPanel::rowLabel(int index) const
{
    const auto& entries = items();
    return index >= 0 && (size_t) index < entries.size() ? entries[(size_t) index].label : std::string{};
}

int BrowserPanel::rowIndex(const std::string& label) const
{
    const auto& entries = items();
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].label == label) return (int) i;
    return -1;
}

float BrowserPanel::rowInset(const Item& item)
{
    return (! item.heading && ! item.group.empty() ? 34.0f : 24.0f) + (float) item.depth * 14;
}

Rect BrowserPanel::disclosureBounds(int index) const
{
    const auto& entries = items();
    if (index < 0 || (size_t) index >= entries.size()) return {};
    const auto& item = entries[(size_t) index];
    if (! item.heading && ! item.directory && item.disclosure.empty()) return {};
    const auto row = listRowBounds(index);
    const float centre = item.disclosure.empty() ? 12 + (float) item.depth * 14 : rowInset(item) - 12;
    return {row.x + centre - 6, row.y, 12, row.h};
}

bool BrowserPanel::searchesPresets() const
{
    return ! query_.empty() && ! folderMode_ && selectedCategory_ != AllItems;
}

const std::vector<BrowserPanel::Item>& BrowserPanel::items(bool filtered, bool groupRows) const
{
    const auto& context = ctx();
    // UI-54: paint, facet layout and hit testing share the same derived rows. Validate cheap
    // inputs without copying them on a cache hit; scroll, selection and canvas size do not change rows.
    if (! itemsKey_ || itemsKey_->contentRevision != context.browserContentRevision
        || itemsKey_->tagsRevision != context.browserTags.revision() || itemsKey_->category != selectedCategory_
        || itemsKey_->folderMode != folderMode_ || itemsKey_->query != query_ || itemsKey_->folder != context.browserFolder
        || itemsKey_->filters != filters_ || itemsKey_->groups != expandedGroups_ || itemsKey_->folders != expandedFolders_
        || itemsKey_->similarRevision != context.similarSamples.revision)
    {
        itemsKey_ = ItemsKey{context.browserContentRevision, context.browserTags.revision(), selectedCategory_, folderMode_,
                             query_, context.browserFolder, filters_, expandedGroups_, expandedFolders_, context.similarSamples.revision};
        for (auto& entry : itemsCache_) entry.reset();
        facetGroups_.reset();
    }
    auto& cached = itemsCache_[(filtered ? 2u : 0u) + (groupRows ? 1u : 0u)];
    if (! cached) cached = buildItems(filtered, groupRows);
    return *cached;
}

// The rows of one category before tags, search and grouping. Every folder view lists ctx().browserFolder as Samples does.
void BrowserPanel::appendCategory(int category, std::vector<Item>& result, bool indexed) const
{
    const auto& context = ctx();
    if (category == Samples)
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
        if (indexed)
        {
            // Pre-filter by name (or by user tags, which the query also matches) so large trees stay cheap.
            const auto query = juce::String::fromUTF8(query_.c_str());
            std::vector<UiContext::BrowserFile> matches;
            for (const auto& file : context.browserSearchFiles)
                if (juce::File(file.path).getFileName().containsIgnoreCase(query) || context.browserTags.hasChanges("file:" + file.path))
                    matches.push_back(file);
            append(matches, 0, {});
        }
        else
            append(context.browserFiles, 0, {});
    }
    else if (category == PlugIns)
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
    else if (category == Sounds)
    {
        // LIB-2: instrument presets grouped by their library folder (Bass, Strings, ...); a preset directly in its
        // device's folder goes under the device's name.
        for (const auto& preset : context.devicePresets)
        {
            const auto* entry = model::findCatalogEntry(preset.deviceTag);
            if (! entry || entry->role != model::DeviceRole::Instrument) continue;
            result.push_back({preset.name, preset.path, false, false, BrowserDrag{BrowserDrag::Kind::DevicePreset, preset.path, preset.name},
                              preset.folder.empty() ? preset.deviceName : preset.folder});
            result.back().instrument = true;
            result.back().tags = presetTags(preset);
        }
    }
    else if (category == Projects)
        for (const auto& file : context.projectFiles)
            result.push_back({juce::File(file).getFileNameWithoutExtension().toStdString(), file, false, false, {}});
    else if (category >= Drums && category <= MidiEffects)
    {
        const auto role = category <= Instruments ? model::DeviceRole::Instrument
                           : category == AudioEffects ? model::DeviceRole::AudioEffect : model::DeviceRole::MidiEffect;
        std::vector<model::CatalogEntry> entries(model::deviceCatalog().begin(), model::deviceCatalog().end());
        entries.insert(entries.end(), model::maxForLiveReplacements().begin(), model::maxForLiveReplacements().end());
        for (const auto& entry : entries)
        {
            if (! entry.implemented || entry.role != role || entry.xmlTag == "AutoFilter") continue; // one Auto Filter: the 12.4 device
            const bool drum = entry.xmlTag == "DrumGroupDevice" || entry.xmlTag == "SonoraImpulse";
            if (category == Drums && ! drum) continue;
            if (category == Instruments && drum && selectedCategory_ == AllItems) continue; // UI-64: once, under Drums
            const std::string name(entry.displayName), tag(entry.xmlTag);
            result.push_back({name, {}, false, false, BrowserDrag{BrowserDrag::Kind::NativeDevice, tag, name},
                              std::string(model::deviceCategory(entry))});
            result.back().instrument = role == model::DeviceRole::Instrument;
            result.back().tags = {{"Type", role == model::DeviceRole::Instrument ? "Instrument" : role == model::DeviceRole::MidiEffect ? "MIDI Effect" : "Audio Effect"},
                                  {"Device", result.back().group == "Synthesis" ? "Synthesizer" : result.back().group}};
            if (! presetsOf(context.devicePresets, tag).empty()) result.back().disclosure = "preset:" + tag; // LIB-2
        }
    }
}

std::vector<BrowserPanel::Item> BrowserPanel::buildItems(bool filtered, bool groupRows) const
{
    const auto& context = ctx();
    std::vector<Item> result;
    // UI-6b: a search in a folder lists matches from every subfolder once the recursive index of that folder is in.
    const bool indexed = listsFolder() && ! query_.empty() && ! context.browserSearchLoading
                         && ! context.browserSearchRoot.empty() && context.browserSearchRoot == context.browserFolder;
    if (similarList()) appendSimilar(result);
    else if (folderMode_) appendCategory(Samples, result, indexed);
    else if (selectedCategory_ == AllItems)
        for (int category = Sounds; category <= Samples; ++category)
        {
            const auto first = result.size();
            appendCategory(category, result, indexed);
            for (auto i = first; i < result.size(); ++i) result[i].section = category;
        }
    else if (packList()) appendPacks(result);
    else appendCategory(selectedCategory_, result, indexed);
    for (auto& item : result)
    {
        if (item.directory || ! item.packId.empty()) continue;
        item.key = item.drag && item.drag->kind != BrowserDrag::Kind::Sample && item.drag->kind != BrowserDrag::Kind::DevicePreset
                       ? (item.drag->kind == BrowserDrag::Kind::Plugin ? "plugin:" : "native:") + item.drag->value : "file:" + item.path;
        if (item.tags.empty())
        {
            item.tags = {{"Type", item.sample ? "Sample" : "Set"}};
            const auto extension = juce::File(item.path).getFileExtension().substring(1).toUpperCase().toStdString();
            if (! extension.empty()) item.tags.push_back({"Format", extension});
            if (const auto pack = context.samplePackFileTags.find(item.path); pack != context.samplePackFileTags.end())
                item.tags.insert(item.tags.end(), pack->second.begin(), pack->second.end()); // LIB-1: Style, Sound, Pack...
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
            if (indexed && item.depth > 0) return false; // the contents of an opened result folder
            if (! item.disclosure.empty() && item.drag && searchesPresets()) // LIB-2: a device whose presets match stays
                for (const auto& preset : presetsOf(context.devicePresets, item.drag->value))
                    if (presetMatches(preset, query)) return false;
            for (const auto& tag : item.tags) alias += " " + tag.value;
            return ! juce::String::fromUTF8((item.label + " " + item.group + alias).c_str()).containsIgnoreCase(query);
        }), result.end());
    }
    if (filtered && ! filters_.empty())
        std::erase_if(result, [&](const Item& item) { return item.directory || ! BrowserTags::matches(item.tags, filters_); });
    if (similarList()) return result; // LIB-4: nearest first, never grouped
    if (groupRows && ! folderMode_ && selectedCategory_ == AllItems) return attachPresets(groupAll(std::move(result)));
    if (groupRows && ! folderMode_ && selectedCategory_ >= Sounds && selectedCategory_ <= PlugIns)
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
        return attachPresets(std::move(grouped));
    }
    return result;
}

// LIB-2: under each shown device row with presets, its preset folders (then the presets directly in its folder) when the
// row is open, and under an open folder its presets. A search outside All opens the devices and folders holding matches
// and lists only those; All leaves them to its Sounds section, so a preset is found once.
std::vector<BrowserPanel::Item> BrowserPanel::attachPresets(std::vector<Item> rows) const
{
    const auto& context = ctx();
    if (context.devicePresets.empty()) return rows;
    const auto query = juce::String::fromUTF8(query_.c_str());
    const bool searching = searchesPresets();
    std::vector<Item> result;
    result.reserve(rows.size());
    for (auto& row : rows)
    {
        if (row.disclosure.empty() || ! row.drag || row.drag->kind != BrowserDrag::Kind::NativeDevice)
        {
            result.push_back(std::move(row));
            continue;
        }
        const auto presets = presetsOf(context.devicePresets, row.drag->value);
        std::vector<const Preset*> shown;
        if (searching)
            for (const auto& preset : presets)
                if (presetMatches(preset, query)) shown.push_back(&preset);
        const bool found = ! shown.empty();
        row.open = found || expandedFolders_.contains(row.disclosure);
        const Item parent = row;
        result.push_back(std::move(row));
        if (! parent.open) continue;
        if (! found)
            for (const auto& preset : presets) shown.push_back(&preset);
        std::string folderKey;
        bool folderOpen = false;
        for (const auto* preset : shown)
        {
            const bool loose = preset->folder.empty();
            if (! loose && folderKey != parent.disclosure + "/" + preset->folder)
            {
                folderKey = parent.disclosure + "/" + preset->folder;
                folderOpen = found || expandedFolders_.contains(folderKey);
                Item folder{preset->folder, {}, false, false, {}, parent.group, false, parent.depth + 1, parent.disclosure};
                folder.disclosure = folderKey;
                folder.open = folderOpen;
                folder.section = parent.section;
                folder.instrument = parent.instrument;
                result.push_back(std::move(folder));
            }
            if (! loose && ! folderOpen) continue;
            Item item{preset->name, preset->path, false, false, BrowserDrag{BrowserDrag::Kind::DevicePreset, preset->path, preset->name},
                      parent.group, false, parent.depth + (loose ? 1 : 2), loose ? parent.disclosure : folderKey};
            item.instrument = parent.instrument;
            item.section = parent.section;
            item.key = "file:" + preset->path;
            item.suggestedTags = presetTags(*preset);
            item.tags = context.browserTags.tagsFor(item.key, item.suggestedTags);
            result.push_back(std::move(item));
        }
    }
    return result;
}

// UI-64: one heading per Library category (open the first time All is shown) over that category's own groups, or over
// the folder rows for Samples. Rows keep their identity, drag and icon; only their indent and group key change.
std::vector<BrowserPanel::Item> BrowserPanel::groupAll(std::vector<Item> result) const
{
    const bool searching = ! query_.empty() || ! filters_.empty();
    std::vector<Item> grouped;
    for (int category = Sounds; category <= Samples; ++category)
    {
        std::vector<Item> rows;
        for (auto& item : result) if (item.section == category) rows.push_back(std::move(item));
        if (rows.empty()) continue;
        const std::string name = categories()[(size_t) category].name, key = "All/" + name;
        grouped.push_back({name, {}, false, false, {}, key, true});
        grouped.back().section = category;
        if (! searching && ! expandedGroups_.contains(key)) continue;
        if (category == Samples)
        {
            for (auto& item : rows) { ++item.depth; grouped.push_back(std::move(item)); }
            continue;
        }
        std::stable_sort(rows.begin(), rows.end(), [](const Item& a, const Item& b) {
            const int groups = juce::String(a.group).compareIgnoreCase(juce::String(b.group));
            return groups == 0 ? juce::String(a.label).compareIgnoreCase(juce::String(b.label)) < 0 : groups < 0;
        });
        std::string previous, subgroup;
        for (auto& item : rows)
        {
            if (subgroup.empty() || juce::String(item.group).compareIgnoreCase(juce::String(previous)) != 0)
            {
                previous = item.group;
                subgroup = key + "/" + previous;
                grouped.push_back({previous, {}, false, false, {}, subgroup, true, 1});
                grouped.back().section = category;
            }
            if (! searching && ! expandedGroups_.contains(subgroup)) continue;
            item.group = subgroup;
            item.depth = 1;
            grouped.push_back(std::move(item));
        }
    }
    return grouped;
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

enum class Glyph { None, Library, All, Sounds, Drums, Instruments, AudioEffects, MidiEffects, PlugIn, Samples, Folder, Pack, Add };

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
        case Glyph::All:
        {
            // A shelf: three upright spines of different heights, a banded middle one, a fourth leaning on them.
            const float base = y + h - 1.5f;
            c.drawLine(x + 0.5f, base, x + w - 0.5f, base, line);
            const float tops[] = {2.5f, 4.5f, 3.2f};
            for (int i = 0; i < 3; ++i)
            {
                const float px = x + 2.5f + (float) i * 2.9f;
                c.drawLine(px, y + tops[i], px, base, line);
            }
            c.drawRect(SkRect::MakeXYWH(x + 4.6f, y + 7.5f, 1.6f, 1.6f), solid);
            c.drawLine(x + 9.6f, y + 4, x + 12.8f, base, line);
            break;
        }
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
    if (name == "All") return Glyph::All;
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
        // UI-78: the text with its selection and caret; a clear button at the right end while anything filters the list.
        const bool clearable = clearSearchVisible();
        const Rect text{row.x + 3, row.y, std::max(0.0f, row.w - 3 - (clearable ? 16.0f : 0.0f)), row.h};
        if (query_.empty())
        {
            paint::text(canvas, "Search (Cmd + F)", row.reduced(6, 0), theme.color("TextDisabled"), theme.metrics.fontSize);
            if (searchFocused_ && caretVisible()) paint::line(canvas, row.x + 6, row.y + 4, row.x + 6, row.bottom() - 4, fg);
        }
        else
            searchEdit_.paintText(canvas, text, fg, searchFocused_ ? theme.color("SelectionBackground") : theme.color("StandbySelectionBackground"),
                                  fg, theme.metrics.fontSize, false, searchFocused_ && caretVisible());
        if (clearable)
        {
            const auto clear = clearSearchBounds();
            SkPaint disc;
            disc.setAntiAlias(true);
            disc.setColor(paint::withAlpha(theme.color("TextDisabled"), 200));
            canvas.drawCircle(clear.centreX(), clear.centreY(), 6.0f, disc);
            const SkColor cross = theme.color("SurfaceBackground");
            paint::line(canvas, clear.centreX() - 2.5f, clear.centreY() - 2.5f, clear.centreX() + 2.5f, clear.centreY() + 2.5f, cross, 1.3f);
            paint::line(canvas, clear.centreX() - 2.5f, clear.centreY() + 2.5f, clear.centreX() + 2.5f, clear.centreY() - 2.5f, cross, 1.3f);
        }
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

    // LIB-4: "Similar to: <file>" with its x, and the search's progress or outcome beside it.
    if (similarList())
    {
        const auto chip = similarChipBounds();
        const auto& similar = ctx().similarSamples;
        paint::fill(canvas, chip, theme.color("ChosenDefault"));
        const auto name = juce::File(juce::String::fromUTF8(similar.reference.c_str())).getFileName().toStdString();
        paint::text(canvas, "Similar to: " + name, {chip.x + 5, chip.y, chip.w - 21, chip.h}, fg, theme.metrics.fontSize);
        paint::text(canvas, "\u00D7", {chip.right() - 14, chip.y, 12, chip.h}, fg, theme.metrics.fontSize);
        std::string note;
        if (similar.running)
            note = similar.total > 0 ? "Comparing " + std::to_string(similar.done) + " of " + std::to_string(similar.total) + "..." : "Looking for samples...";
        else if (! similar.message.empty()) note = similar.message;
        else if (similar.clip.uid >= 0) note = "Double-click / Enter: use in the clip";
        if (! note.empty() && chip.right() + 8 < width() - 4)
            paint::text(canvas, note, {chip.right() + 8, chip.y, width() - chip.right() - 12, chip.h}, theme.color("TextDisabled"),
                        theme.metrics.fontSize);
    }

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
    const auto& entries = items();
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
        if (item.directory || item.heading || ! item.disclosure.empty())
            ctl::triangle(canvas, row.x + (item.disclosure.empty() ? 12 + (float) item.depth * 14 : rowInset(item) - 12), row.centreY(), 8, 5,
                          ((item.heading && (! query_.empty() || ! filters_.empty() || expandedGroups_.contains(item.group)))
                           || (item.directory && expandedFolders_.contains(item.path)) || (! item.disclosure.empty() && item.open))
                              ? ctl::Dir::Down : ctl::Dir::Right,
                          theme.color("TreeRowCategoryForeground"));
        float inset = rowInset(item);
        if (! item.format.empty())
        {
            drawPluginIcon(canvas, {row.x + inset, row.centreY() - 7, 17, 14}, item.format, item.instrument, fg,
                           selected ? selectedBg : theme.color("SurfaceBackground"));
            inset += 24;
        }
        if (! item.packId.empty()) { paintPack(canvas, theme, item, row, selected); continue; }
        float labelRight = row.right() - 4;
        if (similarList() && ! item.hint.empty() && row.w > 220) // LIB-4: the folder each match lives in
        {
            const float hintWidth = std::min({row.w * 0.3f, 120.0f, gfx::measureText(item.hint, theme.metrics.fontSize) + 4});
            paint::text(canvas, item.hint, {row.right() - hintWidth - 6, row.y, hintWidth, row.h}, theme.color("TextDisabled"),
                        theme.metrics.fontSize, Align::Right);
            labelRight -= hintWidth + 6;
        }
        paint::text(canvas, item.label, Rect{row.x + inset, row.y, labelRight - row.x - inset, row.h}, fg,
                    theme.metrics.fontSize, Align::Left, item.heading);
    }
    if (entries.empty())
    {
        const char* note = similarList() ? (ctx().similarSamples.running ? "Searching for similar samples..." : "No similar samples found")
                           : ctx().browserLoading && folderMode_ ? "Loading..."
                           : listsFolder() && ! query_.empty() && ctx().browserSearchLoading ? "Searching..." : "No results";
        paint::text(canvas, note, {list.x + 8, list.y, list.w - 12, 30}, theme.color("TextDisabled"), theme.metrics.fontSize);
    }
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
        if (caretVisible())
        {
            const float caret = std::min(field.right() - 3, field.x + 4 + gfx::measureText(tagText_, theme.metrics.fontSize) + 1);
            paint::line(canvas, caret, field.y + 3, caret, field.bottom() - 3, fg);
        }
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
    std::string footerText = selected ? "1 item selected" : "0 items selected";
    if (selected && ! entries[(size_t) selectedItem_].packId.empty())
        for (const auto& pack : ctx().samplePacks)
            if (pack.id == entries[(size_t) selectedItem_].packId)
                footerText = ! pack.message.empty() ? pack.message
                             : megabytes(pack.bytes) + (pack.licence == "CC0-1.0" ? ", free to use, no credit needed (CC0)" : ", " + pack.licence);
    paint::text(canvas, footerText,
                {button.right() + 6, footer.y, std::max(0.0f, footer.right() - button.right() - 12), footer.h},
                fg, theme.metrics.fontSize, Align::Centre);

}

void BrowserPanel::navigate(const std::string& path)
{
    leaveSimilar();
    folderMode_ = true;
    selectedItem_ = -1;
    scroll_ = 0;
    if (ctx().openBrowserFolder) ctx().openBrowserFolder(path);
}

void BrowserPanel::selectCategory(int index)
{
    leaveSimilar();
    selectedCategory_ = index;
    filters_.clear(); tagEditing_ = false;
    selectedItem_ = -1;
    scroll_ = 0;
    folderMode_ = false;
    if (index >= (int) categories().size()) navigate(ctx().browserPlaces[(size_t) index - categories().size()]);
    else if (index == CurrentProject && ctx().song) navigate(juce::File(ctx().song->sourcePath).getParentDirectory().getFullPathName().toStdString());
    else if (index == UserLibrary) navigate(ctx().userLibraryPath.empty() ? juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("Ableton/User Library").getFullPathName().toStdString() : ctx().userLibraryPath);
    else if (index == Packs && ctx().refreshSamplePacks) ctx().refreshSamplePacks(); // LIB-1: the Packs list
    else if (index == Samples) navigate(samplesRoot());
    else if (index == AllItems)
    {
        if (! allOpened_)
            for (int category = Sounds; category <= Samples; ++category)
                expandedGroups_.insert(std::string("All/") + categories()[(size_t) category].name);
        allOpened_ = true;
        // The Samples section reads the same folder on the same workers as the Samples category (no folder mode).
        if (ctx().openBrowserFolder) ctx().openBrowserFolder(samplesRoot());
    }
}

void BrowserPanel::disclose(const Item& item, bool open)
{
    if (item.heading)
    {
        if (open) expandedGroups_.insert(item.group); else expandedGroups_.erase(item.group);
    }
    else if (! item.disclosure.empty()) // LIB-2: a device's presets or a preset folder
    {
        if (open) expandedFolders_.insert(item.disclosure); else expandedFolders_.erase(item.disclosure);
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
    if (! item.packId.empty())
    {
        if (! item.path.empty()) navigate(item.path); // an installed pack opens like a folder; Up comes back here
        else packAction(item);
    }
    else if (item.directory && packList()) navigate(item.path); // the factory packs folder: rows expand only in folders
    else if (item.heading)
    {
        if (! expandedGroups_.erase(item.group)) expandedGroups_.insert(item.group);
        repaint();
    }
    else if (! item.disclosure.empty() && ! item.drag) disclose(item, ! item.open); // LIB-2: a preset folder
    else if (item.directory) disclose(item, ! expandedFolders_.contains(item.path));
    else if (item.sample && similarList() && ctx().similarSamples.clip.uid >= 0) // LIB-4/5: swap the clip's sample
        replaceClipSampleWith(ctx(), ctx().similarSamples.clip, item.path);
    else if (item.sample && ctx().previewSample) ctx().previewSample(item.path);
    else if (item.drag && item.drag->kind == BrowserDrag::Kind::DevicePreset) // LIB-2: loads like a device (selected track)
    { if (ctx().dropBrowserItem) ctx().dropBrowserItem(*item.drag, ctx().selectedTrack, 0, -1); }
    else if (! item.path.empty() && ctx().openFile) ctx().openFile(item.path);
    else if (item.drag && ctx().dropBrowserItem) ctx().dropBrowserItem(*item.drag, ctx().selectedTrack, 0, -1);
}

void BrowserPanel::showItemMenu(Item item)
{
    if (! item.packId.empty()) { showPackMenu(item); return; }
    juce::PopupMenu menu;
    const bool preset = item.drag && item.drag->kind == BrowserDrag::Kind::DevicePreset;
    if (item.heading || item.directory || (! item.disclosure.empty() && ! item.drag))
    {
        const bool expanded = item.heading ? expandedGroups_.contains(item.group)
                              : item.directory ? expandedFolders_.contains(item.path) : item.open;
        juce::PopupMenu::Item entry(expanded ? "Collapse" : "Expand");
        entry.action = [this, item, expanded] { if (Widget::isAlive(this)) disclose(item, ! expanded); };
        menu.addItem(std::move(entry));
    }
    else
    {
        juce::PopupMenu::Item load(item.path.empty() || item.sample || preset ? "Load" : "Open Set");
        load.action = [this, item] {
            if (! Widget::isAlive(this)) return;
            if (item.sample && item.drag && ctx().dropBrowserItem) ctx().dropBrowserItem(*item.drag, ctx().selectedTrack, 0, -1);
            else activate(item);
        };
        menu.addItem(std::move(load));
    }
    if (item.sample)
    {
        // LIB-4 / LIB-5: from a sample row, find what sounds like it, or put it into the selected (or searched-from) clip.
        menu.addSeparator();
        juce::PopupMenu::Item similar("Show Similar Files");
        similar.isEnabled = static_cast<bool>(ctx().findSimilarSamples);
        similar.action = [this, path = item.path] { if (Widget::isAlive(this)) showSimilarFiles(ctx(), path); };
        menu.addItem(std::move(similar));
        const auto target = similarList() && ctx().similarSamples.clip.uid >= 0 ? ctx().similarSamples.clip : ctx().selectedClipLocation();
        const auto* clip = ctx().song ? model::findClip(*ctx().song, target) : nullptr;
        juce::PopupMenu::Item replace(clip && clip->kind == model::Clip::Kind::Audio ? "Replace Sample of \"" + juce::String::fromUTF8(clip->name.c_str()) + "\""
                                                                                     : juce::String("Replace Sample of Selected Clip"));
        replace.isEnabled = clip && clip->kind == model::Clip::Kind::Audio && ctx().replaceClipSample;
        replace.action = [this, target, path = item.path] { if (Widget::isAlive(this)) replaceClipSampleWith(ctx(), target, path); };
        menu.addItem(std::move(replace));
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
    syncSimilar();
    if (! tagFooter().contains(event.x, event.y)) tagEditing_ = false;
    const bool wasFocused = searchFocused_;
    searchFocused_ = searchFieldBounds().reduced(0, -4).contains(event.x, event.y) && ! event.right;
    if (searchFocused_)
    {
        restartCaret();
        if (clearSearchVisible() && clearSearchBounds().reduced(-2, -4).contains(event.x, event.y))
        {
            clearSearch(); // UI-78: the x empties the field, drops tag filters and leaves a similar list
            repaint();
            return;
        }
        // UI-78: the first click into the field selects its text (typing replaces it); later clicks place the caret,
        // a double click selects a word, a triple click everything, Shift extends and dragging selects.
        if (! wasFocused && event.clicks < 2) searchEdit_.selectAll();
        else searchEdit_.click(searchEdit_.offsetAt(event.x, searchFieldBounds(), ctx().theme.metrics.fontSize), event.clicks, event.shift);
        selectingText_ = event.clicks < 2;
        repaint();
        return;
    }
    if (similarList() && similarChipBounds().contains(event.x, event.y))
    {
        if (event.x >= similarChipBounds().right() - 16) leaveSimilar();
        repaint();
        return;
    }
    if (event.y < kSearch)
    {
        if (event.x >= width() - 27)
        {
            filtersVisible_ = ! filtersVisible_; scroll_ = 0;
        }
        else if (event.x >= 5 && event.x < 23 && folderMode_)
        {
            const auto parentFolder = juce::File(ctx().browserFolder).getParentDirectory().getFullPathName().toStdString();
            if (selectedCategory_ == packsCategory() && (parentFolder == ctx().samplePacksRoot || ctx().browserFolder == ctx().packsPath))
            { folderMode_ = false; selectedItem_ = -1; scroll_ = 0; } // LIB-1: back to the Packs list
            else navigate(parentFolder);
        }
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
            if (! item.packId.empty() && packButtonBounds(selectedItem_).contains(event.x, event.y)) { packAction(item); repaint(); return; }
            if (item.heading || (item.directory && event.x < listArea().x + 22 + (float) item.depth * 14)) activate(item);
            else if (! item.disclosure.empty() && event.x < listArea().x + rowInset(item) - 2) disclose(item, ! item.open);
            else pendingDrag_ = item.drag;
        }
        if (selectedItem_ >= 0 && (size_t) selectedItem_ < entries.size() && entries[(size_t) selectedItem_].sample
            && ctx().browserPreviewEnabled && ctx().previewSample) ctx().previewSample(entries[(size_t) selectedItem_].path);
    }
    repaint();
}

void BrowserPanel::mouseDrag(const MouseEvent& event)
{
    if (selectingText_ && searchFocused_)
    {
        searchEdit_.dragTo(searchEdit_.offsetAt(event.x, searchFieldBounds(), ctx().theme.metrics.fontSize));
        restartCaret();
        repaint();
        return;
    }
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
    if (event.right || event.y < kSearch) { mouseDown(event); return; }
    const auto before = items();
    const int row = (int) ((event.y - listArea().y - 2 + scroll_) / kRow);
    if (listArea().contains(event.x, event.y) && row >= 0 && (size_t) row < before.size()
        && (before[(size_t) row].heading || (before[(size_t) row].directory && event.x < listArea().x + 22 + (float) before[(size_t) row].depth * 14)
            || (! before[(size_t) row].disclosure.empty() && event.x < listArea().x + rowInset(before[(size_t) row]) - 2)))
        return;
    mouseDown(event);
    if (! listArea().contains(event.x, event.y)) return;
    const auto entries = items();
    if (selectedItem_ >= 0 && (size_t) selectedItem_ < entries.size() && ! entries[(size_t) selectedItem_].heading)
        activate(entries[(size_t) selectedItem_]);
}

void BrowserPanel::restartCaret()
{
    caretEpoch_ = juce::Time::getMillisecondCounter();
    caretShown_ = true;
}

bool BrowserPanel::caretVisible() const
{
    return (juce::Time::getMillisecondCounter() - caretEpoch_) / 530 % 2 == 0;
}

void BrowserPanel::tick()
{
    // Blink the caret, and (re)index the shown folder while a search is typed in it (UI-6b).
    if ((searchFocused_ || tagEditing_) && caretVisible() != caretShown_) { caretShown_ = ! caretShown_; repaint(); }
    syncSimilar();
    if (! ctx().browserReveal.empty()) revealPending();
    const auto& context = ctx();
    if (listsFolder() && ! query_.empty() && context.indexBrowserFolder && ! context.browserFolder.empty()
        && context.browserSearchRoot != context.browserFolder)
        context.indexBrowserFolder(context.browserFolder);
}

void BrowserPanel::focusSearch()
{
    ctx().browserVisible = true;
    tagEditing_ = false;
    restartCaret();
    searchFocused_ = keyboardFocused_ = true;
    searchEdit_.selectAll(); // UI-78: like the reference, Cmd+F selects what was typed, so typing starts a new search
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
        restartCaret();
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
    if (key.keyCode == juce::KeyPress::escapeKey)
    {
        // UI-78: Escape clears the search, the tag filters and a similar list (like the reference) and leaves the field, so
        // the next keys play clips / notes again.
        if (clearSearchVisible()) clearSearch();
        searchFocused_ = keyboardFocused_ = false;
        repaint();
        return true;
    }
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
                if (item.heading || item.directory || ! item.disclosure.empty()) disclose(item, true);
                else if (item.sample && ctx().previewSample) ctx().previewSample(item.path);
            }
            else if ((item.heading && expandedGroups_.contains(item.group)) || (item.directory && expandedFolders_.contains(item.path))
                     || (! item.disclosure.empty() && item.open))
                disclose(item, false);
            else
                for (int i = selectedItem_ - 1; i >= 0; --i)
                    if ((! item.parentFolder.empty() && (entries[(size_t) i].path == item.parentFolder || entries[(size_t) i].disclosure == item.parentFolder))
                        || (! item.group.empty() && entries[(size_t) i].heading && entries[(size_t) i].group == item.group)
                        || (item.parentFolder.empty() && entries[(size_t) i].heading && entries[(size_t) i].depth < item.depth))
                    { selectedItem_ = i; break; }
        }
        repaint();
        return true;
    }
    if (searchFocused_)
    {
        // UI-78: the field edits like a text field (InlineTextEdit): selection, Cmd+A/C/X/V, word moves.
        restartCaret();
        if (key.keyCode == juce::KeyPress::returnKey) { searchFocused_ = false; repaint(); return true; }
        if (key.ctrl || (key.cmd && key.textChar != 'a' && key.textChar != 'A' && key.textChar != 'c' && key.textChar != 'C'
                         && key.textChar != 'x' && key.textChar != 'X' && key.textChar != 'v' && key.textChar != 'V'
                         && key.keyCode != juce::KeyPress::leftKey && key.keyCode != juce::KeyPress::rightKey
                         && key.keyCode != juce::KeyPress::backspaceKey))
            return false; // other shortcuts keep working while the field has the focus
        const bool starting = query_.empty();
        const auto before = searchEdit_.text;
        searchEdit_.key(key);
        if (searchEdit_.text != before) searchChanged(starting);
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

// ---- UI-78 search field ---------------------------------------------------------------------------------------------

Rect BrowserPanel::searchFieldBounds() const
{
    return {47, 4, std::max(0.0f, width() - 74), kSearch - 8};
}

Rect BrowserPanel::clearSearchBounds() const
{
    const auto field = searchFieldBounds();
    return {field.right() - 16, field.y, 16, field.h};
}

bool BrowserPanel::clearSearchVisible() const
{
    return ! query_.empty() || ! filters_.empty() || similarList();
}

void BrowserPanel::searchChanged(bool starting)
{
    query_ = searchEdit_.text;
    // A new search re-reads the folder tree so it also finds files added since the last one.
    if (starting && ! query_.empty() && listsFolder() && ctx().indexBrowserFolder) ctx().indexBrowserFolder(ctx().browserFolder);
    selectedItem_ = -1;
    scroll_ = 0;
    pendingDrag_.reset();
}

void BrowserPanel::clearSearch()
{
    searchEdit_.clear();
    query_.clear();
    filters_.clear();
    tagEditing_ = false;
    leaveSimilar();
    selectedItem_ = -1;
    scroll_ = 0;
    pendingDrag_.reset();
    if (ctx().previewSample) ctx().previewSample("");
}

// ---- LIB-4 similar list ----------------------------------------------------------------------------------------------

bool BrowserPanel::similarList() const
{
    return ctx().similarSamples.active;
}

float BrowserPanel::similarRowHeight() const
{
    return similarList() ? kFacet + 2 : 0.0f;
}

Rect BrowserPanel::similarChipBounds() const
{
    if (! similarList()) return {};
    const float left = sidebarArea().right() + 5;
    const auto name = juce::File(juce::String::fromUTF8(ctx().similarSamples.reference.c_str())).getFileName().toStdString();
    const float w = std::min(std::max(40.0f, width() - left - 5), gfx::measureText("Similar to: " + name, ctx().theme.metrics.fontSize) + 26);
    return {left, kSearch + 3, w, kFacet - 3};
}

void BrowserPanel::appendSimilar(std::vector<Item>& result) const
{
    for (const auto& [path, distance] : ctx().similarSamples.results)
    {
        const juce::File file(juce::String::fromUTF8(path.c_str()));
        Item item{file.getFileName().toStdString(), path, false, true,
                  BrowserDrag{BrowserDrag::Kind::Sample, path, file.getFileName().toStdString()}};
        item.hint = file.getParentDirectory().getFileName().toStdString();
        result.push_back(std::move(item));
    }
}

void BrowserPanel::leaveSimilar()
{
    if (! ctx().similarSamples.active && ! ctx().similarSamples.running) return;
    leaveSimilarFiles(ctx());
    shownSimilar_.clear();
    selectedItem_ = -1;
    scroll_ = 0;
}

void BrowserPanel::syncSimilar()
{
    const auto& similar = ctx().similarSamples;
    const auto key = similar.active ? similar.reference + "#" + std::to_string(similar.clip.uid) : std::string{};
    if (key == shownSimilar_) return;
    shownSimilar_ = key;
    if (! similar.active) return;
    // A new list: start at its top with nothing filtering it.
    searchEdit_.clear();
    query_.clear();
    filters_.clear();
    tagEditing_ = false;
    selectedItem_ = -1;
    scroll_ = 0;
    repaint();
}

// ---- LIB-3 Show in Browser -------------------------------------------------------------------------------------------
// Opens the folder that holds UiContext::browserReveal (under the Place whose folder contains it, or as a plain folder
// view like any folder outside Places), waits for its listing, then selects the file's row and scrolls it into view.
void BrowserPanel::revealPending()
{
    auto& x = ctx();
    const juce::File file(juce::String::fromUTF8(x.browserReveal.c_str()));
    const auto folder = file.getParentDirectory().getFullPathName().toStdString();
    if (revealing_ != x.browserReveal)
    {
        revealing_ = x.browserReveal;
        leaveSimilar();
        searchEdit_.clear();
        query_.clear();
        filters_.clear();
        tagEditing_ = false;
        int category = Samples;
        const auto inside = [&](const std::string& root) {
            const juce::File base(juce::String::fromUTF8(root.c_str()));
            return ! root.empty() && (file.isAChildOf(base));
        };
        for (size_t i = 0; i < x.browserPlaces.size(); ++i)
            if (inside(x.browserPlaces[i])) { category = (int) (categories().size() + i); break; }
        if (category == Samples && x.song && inside(juce::File(juce::String::fromUTF8(x.song->sourcePath.c_str())).getParentDirectory().getFullPathName().toStdString()))
            category = CurrentProject;
        else if (category == Samples && inside(x.userLibraryPath)) category = UserLibrary;
        selectedCategory_ = category;
        navigate(folder);
        keyboardFocused_ = true;
        for (auto* ancestor = parent(); ancestor; ancestor = ancestor->parent())
            if (auto* root = dynamic_cast<RootWidget*>(ancestor)) root->setKeyboardFocus(this);
        repaint();
        return;
    }
    if (! folderMode_ || x.browserFolder != folder) { x.browserReveal.clear(); revealing_.clear(); return; } // went elsewhere
    if (x.browserLoading) return;
    const auto& entries = items();
    for (size_t i = 0; i < entries.size(); ++i)
        if (entries[i].path == revealing_)
        {
            selectedItem_ = (int) i;
            const float top = (float) i * kRow, view = listArea().h - 4;
            if (top < scroll_ || top + kRow > scroll_ + view) scroll_ = std::max(0.0f, top - view * 0.4f);
            break;
        }
    x.browserReveal.clear();
    revealing_.clear();
    repaint();
}

// ---- LIB-1 Packs list (docs/SAMPLE_PACKS.md) -------------------------------------------------------------------------
// One row per Sonora pack (installed first; the app keeps that order), tagged by Style and Type so the facets filter them,
// plus the reference factory packs folder when it exists and a status row for the catalogue. Downloads only start from
// the row's button, its menu or a double-click; an installed pack opens like a folder.

int BrowserPanel::packsCategory()
{
    return Packs;
}


void BrowserPanel::appendPacks(std::vector<Item>& result) const
{
    const auto& context = ctx();
    using List = UiContext::SamplePackList;
    std::string status;
    switch (context.samplePackList)
    {
        case List::Off: status = "Free Sonora sample packs: coming in a later version"; break;
        case List::Hidden: status = "Get Packs...  free CC0 sample packs, downloaded only when you ask"; break;
        case List::Loading: status = "Loading the pack list..."; break;
        case List::Failed: status = context.samplePackListMessage + "  (double-click to retry)"; break;
        case List::Ready: break;
    }
    if (! status.empty())
    {
        result.push_back({status, {}, false, false, {}});
        result.back().packId = "@list";
    }
    if (! context.packsPath.empty() && juce::File(juce::String::fromUTF8(context.packsPath.c_str())).isDirectory())
        result.push_back({juce::File(context.packsPath).getFileName().toStdString(), context.packsPath, true, false, {}});
    for (const auto& pack : context.samplePacks)
    {
        Item item{pack.name, pack.folder, false, false, {}};
        item.packId = pack.id;
        item.key = "pack:" + pack.id;
        for (const auto& style : pack.styles) item.tags.push_back({"Style", style});
        for (const auto& type : pack.types) item.tags.push_back({"Type", type});
        item.tags.push_back({"Pack", pack.folder.empty() ? "Available" : "Installed"});
        item.suggestedTags = item.tags;
        item.tags = context.browserTags.tagsFor(item.key, std::move(item.tags));
        result.push_back(std::move(item));
    }
}

Rect BrowserPanel::packButtonBounds(int listRow) const
{
    const auto row = listRowBounds(listRow);
    return {row.right() - kPackButton - 6, row.y + 2, kPackButton, row.h - 4};
}

void BrowserPanel::paintPack(SkCanvas& canvas, const Theme& theme, const Item& item, const Rect& row, bool selected) const
{
    const SkColor fg = theme.color("ControlForeground"), dim = theme.color("TextDisabled");
    const float font = theme.metrics.fontSize;
    if (item.packId == "@list")
    {
        paint::text(canvas, item.label, row.reduced(10, 0), dim, font);
        return;
    }
    drawGlyph(canvas, Glyph::Pack, {row.x + 8, row.centreY() - 7, 14, 14}, fg);
    const auto* pack = findPack(ctx(), item.packId);
    const bool wide = row.w >= kPackWide;
    const Rect button{row.right() - kPackButton - 6, row.y + 2, kPackButton, row.h - 4};
    const Rect info{button.x - kPackInfo - 6, row.y, kPackInfo, row.h};
    using State = UiContext::SamplePack::State;
    const bool failed = pack != nullptr && pack->state == State::Failed;
    const float nameRight = wide ? info.x - 4 : button.x - 6;
    paint::text(canvas, item.label, Rect{row.x + 28, row.y, std::max(0.0f, nameRight - row.x - 28), row.h},
                failed ? theme.color("ChosenAlert") : fg, font);
    if (pack == nullptr) return;
    const float fraction = pack->bytes > 0 ? std::clamp((float) pack->received / (float) pack->bytes, 0.0f, 1.0f) : 0.0f;
    const std::string percent = std::to_string((int) std::floor(fraction * 100.0f)) + "%";
    auto drawButton = [&](const std::string& label, float progress) {
        ctl::framed(canvas, theme, button, theme.color(selected ? "ControlTextBack" : "SurfaceBackground"));
        if (progress > 0) paint::fill(canvas, {button.x + 1, button.y + 1, (button.w - 2) * progress, button.h - 2}, theme.color("ChosenDefault"));
        paint::text(canvas, label, button, fg, font, Align::Centre);
    };
    auto status = [&](const std::string& text, SkColor colour) { if (wide) paint::text(canvas, text, info, colour, font, Align::Right); };
    switch (pack->state)
    {
        case State::Available:
            status(pack->received > 0 ? percent + " of " + megabytes(pack->bytes) : megabytes(pack->bytes), dim);
            drawButton(pack->received > 0 ? "Resume" : "Download", 0);
            break;
        case State::Queued:
            status("Waiting...", dim);
            drawButton("Cancel", 0);
            break;
        case State::Downloading:
            status(percent + " of " + megabytes(pack->bytes), dim);
            drawButton(wide ? "Cancel" : percent, std::max(fraction, 0.02f)); // the bar is the button: a click cancels
            break;
        case State::Verifying:
        case State::Installing:
            paint::text(canvas, pack->state == State::Verifying ? "Checking..." : "Installing...", button, dim, font, Align::Centre);
            break;
        case State::Installed:
            paint::text(canvas, "Installed", button, dim, font, Align::Centre);
            break;
        case State::Update:
            status("Installed " + pack->version, dim);
            drawButton("Update", 0);
            break;
        case State::Failed:
            status("Failed", theme.color("ChosenAlert"));
            drawButton(pack->received > 0 ? "Resume" : "Retry", 0);
            break;
    }
}

void BrowserPanel::packAction(const Item& item)
{
    auto& context = ctx();
    if (item.packId == "@list")
    {
        if (context.samplePackList != UiContext::SamplePackList::Off && context.showSamplePackList) context.showSamplePackList();
        return;
    }
    const auto* pack = findPack(context, item.packId);
    if (pack == nullptr) return;
    using State = UiContext::SamplePack::State;
    switch (pack->state)
    {
        case State::Available: case State::Update: case State::Failed:
            if (context.downloadSamplePack) context.downloadSamplePack(pack->id);
            break;
        case State::Queued: case State::Downloading:
            if (context.cancelSamplePack) context.cancelSamplePack(pack->id);
            break;
        case State::Verifying: case State::Installing: case State::Installed:
            break;
    }
}

void BrowserPanel::showPackMenu(const Item& item)
{
    if (item.packId == "@list") { packAction(item); return; }
    const auto* pack = findPack(ctx(), item.packId);
    if (pack == nullptr) return;
    using State = UiContext::SamplePack::State;
    juce::PopupMenu menu;
    auto add = [&](const juce::String& label, bool enabled, std::function<void()> run) {
        juce::PopupMenu::Item entry(label);
        entry.isEnabled = enabled;
        entry.action = [this, task = std::move(run)] { if (Widget::isAlive(this)) task(); };
        menu.addItem(std::move(entry));
    };
    const std::string id = pack->id, folder = pack->folder;
    if (! folder.empty()) add("Open Pack", true, [this, folder] { navigate(folder); repaint(); });
    const bool busy = pack->state == State::Queued || pack->state == State::Downloading;
    if (busy) add("Cancel Download", true, [this, id] { if (ctx().cancelSamplePack) ctx().cancelSamplePack(id); });
    else if (pack->state != State::Installed && pack->state != State::Verifying && pack->state != State::Installing)
        add(pack->state == State::Update ? "Update Pack" : pack->received > 0 ? "Resume Download" : "Download Pack (" + juce::String(megabytes(pack->bytes)) + ")",
            true, [this, id] { if (ctx().downloadSamplePack) ctx().downloadSamplePack(id); });
    if (! folder.empty())
    {
        menu.addSeparator();
        add("Show in Finder", static_cast<bool>(ctx().revealInFinder), [this, folder] { if (ctx().revealInFinder) ctx().revealInFinder(folder); });
        add("Remove Pack (moves it to the Trash)", ! busy, [this, id] { if (ctx().removeSamplePack) ctx().removeSamplePack(id); });
    }
    menu.showMenuAsync(juce::PopupMenu::Options());
}

} // namespace sonora::ui

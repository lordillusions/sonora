// LIB-1: the Browser's Packs list on top of packs::PackStore (docs/SAMPLE_PACKS.md).
//
// Nothing is fetched by default. The first "Get Packs..." click is the user's opt-in (setting samplePacksListShown);
// after that, opening Packs refreshes the catalogue once per run. Downloads start only from the Browser.
// The feed and the public key are compiled in (CMake SONORA_PACKS_FEED_URL / SONORA_PACKS_PUBLIC_KEY; an empty key turns
// the feature off). SONORA_PACKS_FEED and SONORA_PACKS_PUBLIC_KEY override them for local tests (scripts/packs/serve_packs.py);
// SONORA_PACKS_ROOT installs elsewhere; SONORA_PACKS_AUTOTEST=<pack id> loads the list and downloads that pack at launch,
// logging "[sonora] sample pack" lines.
#include "app/AppController.h"
#include "util/Log.h"

#ifndef SONORA_PACKS_FEED_URL_STRING
#define SONORA_PACKS_FEED_URL_STRING ""
#endif
#ifndef SONORA_PACKS_PUBLIC_KEY_STRING
#define SONORA_PACKS_PUBLIC_KEY_STRING ""
#endif

namespace sonora::app
{
namespace
{
juce::String overridden(const char* variable, const char* compiled)
{
    const auto value = juce::SystemStats::getEnvironmentVariable(variable, {});
    return value.isNotEmpty() ? value : juce::String(compiled);
}

ui::UiContext::SamplePack::State uiState(packs::PackStore::PackState state)
{
    using From = packs::PackStore::PackState;
    using To = ui::UiContext::SamplePack::State;
    switch (state)
    {
        case From::Available: return To::Available;
        case From::Queued: return To::Queued;
        case From::Downloading: return To::Downloading;
        case From::Verifying: return To::Verifying;
        case From::Installing: return To::Installing;
        case From::Installed: return To::Installed;
        case From::UpdateAvailable: return To::Update;
        case From::Failed: return To::Failed;
    }
    return To::Available;
}
} // namespace

void AppController::setUpSamplePacks()
{
    const auto root = overridden("SONORA_PACKS_ROOT", settings().getValue("samplePacksPath",
        juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("Sonora/Packs").getFullPathName()).toRawUTF8());
    packs::PackStore::Config config{overridden("SONORA_PACKS_FEED", SONORA_PACKS_FEED_URL_STRING),
                                    overridden("SONORA_PACKS_PUBLIC_KEY", SONORA_PACKS_PUBLIC_KEY_STRING).toStdString(), juce::File(root)};
    ui_.samplePacksRoot = config.root.getFullPathName().toStdString();
    packStore_ = std::make_unique<packs::PackStore>(config, [this, alive = std::weak_ptr<bool>(alive_)] {
        // Worker thread: coalesce into one message-thread publish.
        if (samplePacksPublishPending_.exchange(true)) return;
        juce::MessageManager::callAsync([this, alive] {
            if (alive.expired()) return;
            samplePacksPublishPending_ = false;
            publishSamplePacks();
        });
    });
    ui_.showSamplePackList = [this] {
        settings().setValue("samplePacksListShown", true);
        settings().saveIfNeeded();
        samplePacksFetched_ = true;
        packStore_->loadCatalogue();
    };
    ui_.refreshSamplePacks = [this] {
        packStore_->rescan();
        if (! samplePacksFetched_ && settings().getBoolValue("samplePacksListShown", false))
        {
            samplePacksFetched_ = true;
            packStore_->loadCatalogue();
        }
    };
    ui_.downloadSamplePack = [this](const std::string& id) { packStore_->download(id); };
    ui_.cancelSamplePack = [this](const std::string& id) { packStore_->cancel(id); };
    ui_.removeSamplePack = [this](const std::string& id) { packStore_->remove(id); };
    publishSamplePacks();
    if (const auto autotest = juce::SystemStats::getEnvironmentVariable("SONORA_PACKS_AUTOTEST", {}); autotest.isNotEmpty())
    {
        logf("sample pack autotest: %s from %s into %s", autotest.toRawUTF8(), config.catalogueUrl.toRawUTF8(), ui_.samplePacksRoot.c_str());
        samplePacksFetched_ = true;
        packStore_->loadCatalogue(); // the worker runs jobs in order: the download sees the loaded list
        packStore_->download(autotest.toStdString());
    }
}

void AppController::publishSamplePacks()
{
    const auto snapshot = packStore_->snapshot();
    using List = ui::UiContext::SamplePackList;
    using Catalogue = packs::PackStore::CatalogueState;
    switch (snapshot.catalogue)
    {
        case Catalogue::Off: ui_.samplePackList = List::Off; break;
        case Catalogue::NotLoaded: ui_.samplePackList = List::Hidden; break;
        case Catalogue::Loading: ui_.samplePackList = List::Loading; break;
        case Catalogue::Ready: ui_.samplePackList = List::Ready; break;
        case Catalogue::Failed: ui_.samplePackList = List::Failed; break;
    }
    ui_.samplePackListMessage = snapshot.catalogueMessage;
    std::vector<ui::UiContext::SamplePack> rows;
    std::map<std::string, ui::BrowserTags::Tags> fileTags;
    std::set<std::string> installedNow;
    for (const auto& row : snapshot.rows)
    {
        ui::UiContext::SamplePack pack;
        pack.id = row.info.id;
        pack.name = row.info.name;
        pack.version = row.info.version;
        pack.description = row.info.description;
        pack.licence = row.info.licence;
        pack.styles = row.info.styles;
        pack.types = row.info.types;
        pack.bytes = row.total;
        pack.received = row.received;
        pack.state = uiState(row.state);
        pack.message = row.message;
        if (row.installed)
        {
            pack.folder = row.installed->folder.getFullPathName().toStdString();
            installedNow.insert(pack.id + " " + row.installed->contents.info.version);
            // Every file gets its pack's style and name plus its own pack.json tags (Sound, Kind, BPM, Key...).
            ui::BrowserTags::Tags packTags;
            for (const auto& style : row.installed->contents.info.styles) packTags.push_back({"Style", style});
            packTags.push_back({"Pack", row.installed->contents.info.name});
            for (const auto& file : row.installed->contents.files)
            {
                auto tags = packTags;
                for (const auto& [group, value] : file.tags) tags.push_back({group, value});
                fileTags[row.installed->folder.getChildFile(juce::String::fromUTF8(file.path.c_str())).getFullPathName().toStdString()] = std::move(tags);
            }
        }
        rows.push_back(std::move(pack));
    }
    for (const auto& id : installedNow)
        if (! samplePacksInstalled_.contains(id) && samplePacksPublished_) logf("sample pack installed: %s", id.c_str());
    std::set<std::string> failedNow;
    for (const auto& row : rows)
        if (row.state == ui::UiContext::SamplePack::State::Failed)
        {
            failedNow.insert(row.id);
            if (! samplePacksFailed_.contains(row.id)) logf("sample pack %s failed: %s", row.id.c_str(), row.message.c_str());
        }
    samplePacksFailed_ = std::move(failedNow);
    samplePacksInstalled_ = std::move(installedNow);
    samplePacksPublished_ = true;
    ui_.samplePacks = std::move(rows);
    ui_.samplePackFileTags = std::move(fileTags);
    ++ui_.browserContentRevision;
    if (ui_.requestRepaint) ui_.requestRepaint();
}

} // namespace sonora::app

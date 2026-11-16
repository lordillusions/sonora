// SPL-3/4/5: the Browser's Splice section on top of platform::splice::SpliceStore (docs/SPLICE_INTEROP.md).
//
// Off by default (Preference "spliceEnabled"). Every network call runs on splicePool_ (one serial worker) and marshals a
// result back with callAsync; the browser re-reads when ui_.splice.revision and browserContentRevision advance. Buying
// spends the user's credits and only ever happens on an explicit "+" or a place action. The login window (the system
// browser opening the Auth0 verification page) is the only separate window this feature opens.
#include "app/AppController.h"

#include "engine/AudioEngine.h"
#include "io/als/SampleDrop.h"
#include "platform/SecretStore.h"
#include "platform/splice/SpliceKey.h"
#include "util/Log.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <algorithm>

namespace sonora::app
{
using platform::splice::SpliceApi;
using platform::splice::SpliceSample;
using platform::splice::SpliceSearchQuery;

namespace
{
ui::UiContext::SpliceRow toRow(const SpliceSample& s)
{
    ui::UiContext::SpliceRow r;
    r.uuid = s.uuid;
    r.name = s.name;
    r.fileName = juce::File(juce::String::fromUTF8(s.name.c_str())).getFileName().toStdString();
    r.pack = s.packName;
    r.provider = s.providerName;
    r.musicalKey = s.key;
    r.chordType = s.chordType;
    r.previewUrl = s.previewUrl;
    r.category = s.category;
    r.tags = s.tags;
    r.bpm = s.bpm;
    r.durationMs = s.durationMs;
    r.priceCredits = s.priceCredits;
    r.licensed = s.licensed;
    r.loop = s.isLoop;
    return r;
}

juce::File spliceRoot()
{
    return juce::File::getSpecialLocation(juce::File::userMusicDirectory).getChildFile("Sonora/Splice");
}

juce::File previewCacheDir()
{
    auto dir = juce::File::getSpecialLocation(juce::File::tempDirectory).getChildFile("Sonora/SplicePreviews");
    dir.createDirectory();
    return dir;
}
} // namespace

void AppController::setUpSplice()
{
    platform::splice::SpliceEndpoints endpoints;
    if (auto* app = juce::JUCEApplication::getInstance()) endpoints.userAgent = "Sonora/" + app->getApplicationVersion();
    static platform::SecretStore secrets; // process-lifetime; the keychain backend ignores the folder
    spliceStore_ = std::make_unique<platform::splice::SpliceStore>(endpoints, platform::splice::Auth0Config{}, secrets);

    ui_.splice.enabled = settings().getBoolValue("spliceEnabled", false);
    ui_.splice.matchKey = settings().getBoolValue("spliceMatchKey", true);
    ui_.splice.previewGain = (float) settings().getDoubleValue("splicePreviewGain", 0.5);
    engine_.setPreviewGain(ui_.splice.previewGain);
    ui_.splice.auth = ! ui_.splice.enabled ? ui::UiContext::SpliceAuth::Disabled
                    : spliceStore_->loggedIn() ? ui::UiContext::SpliceAuth::LoggedIn
                                               : ui::UiContext::SpliceAuth::LoggedOut;

    const auto bump = [this] { ++ui_.splice.revision; ++ui_.browserContentRevision; if (ui_.requestRepaint) ui_.requestRepaint(); };

    // Re-fetch the credit balance (and confirm the session) in the background.
    const auto refreshCredits = [this] {
        const auto alive = std::weak_ptr<bool>(alive_);
        splicePool_.addJob([this, alive] {
            std::atomic<bool> cancel{false};
            int credits = -1;
            const bool hadToken = spliceStore_->withToken(cancel, [&](const std::string& token) {
                credits = spliceStore_->api().creditBalance(token, cancel);
            });
            const bool needLogin = spliceStore_->takeNeedsLogin();
            juce::MessageManager::callAsync([this, alive, credits, hadToken, needLogin] {
                if (alive.expired()) return;
                if (! hadToken && needLogin && ui_.splice.enabled && ui_.splice.auth != ui::UiContext::SpliceAuth::AwaitingLogin)
                {
                    ui_.splice.auth = ui::UiContext::SpliceAuth::LoggedOut;
                    ui_.splice.credits = -1;
                }
                if (credits >= 0) { ui_.splice.credits = credits; ui_.splice.auth = ui::UiContext::SpliceAuth::LoggedIn; }
                ++ui_.splice.revision; ++ui_.browserContentRevision; if (ui_.requestRepaint) ui_.requestRepaint();
            });
        });
    };
    // COM-15: the balance needs the stored sign-in, a keychain read that macOS may ask to confirm after an update (the
    // build is signed ad hoc), so it waits for the first Splice search instead of running at launch.
    auto creditsPending = std::make_shared<bool>(ui_.splice.enabled && spliceStore_->loggedIn());

    ui_.spliceSetEnabled = [this, bump, refreshCredits](bool on) {
        settings().setValue("spliceEnabled", on);
        settings().saveIfNeeded();
        ui_.splice.enabled = on;
        ui_.splice.auth = ! on ? ui::UiContext::SpliceAuth::Disabled
                        : spliceStore_->loggedIn() ? ui::UiContext::SpliceAuth::LoggedIn
                                                   : ui::UiContext::SpliceAuth::LoggedOut;
        ui_.splice.message.clear();
        bump();
        if (on && spliceStore_->loggedIn()) refreshCredits();
    };

    ui_.spliceSignOut = [this, bump] {
        spliceStore_->signOut();
        ui_.splice.auth = ui_.splice.enabled ? ui::UiContext::SpliceAuth::LoggedOut : ui::UiContext::SpliceAuth::Disabled;
        ui_.splice.credits = -1;
        ui_.splice.username.clear();
        ui_.splice.results.clear();
        ui_.splice.message.clear();
        bump();
    };

    ui_.spliceLogin = [this, bump, refreshCredits] {
        if (spliceLoginActive_->exchange(true)) return;
        ui_.splice.auth = ui::UiContext::SpliceAuth::AwaitingLogin;
        ui_.splice.message = "Starting sign-in...";
        bump();
        const auto alive = std::weak_ptr<bool>(alive_);
        const auto active = spliceLoginActive_;
        splicePool_.addJob([this, alive, active, refreshCredits] {
            std::atomic<bool> cancel{false};
            const auto start = spliceStore_->startLogin(cancel);
            if (! start.ok)
            {
                juce::MessageManager::callAsync([this, alive] {
                    if (alive.expired()) return;
                    ui_.splice.auth = ui::UiContext::SpliceAuth::Error;
                    ui_.splice.message = "Could not start sign-in";
                    ++ui_.splice.revision; if (ui_.requestRepaint) ui_.requestRepaint();
                });
                active->store(false);
                return;
            }
            juce::MessageManager::callAsync([this, alive, start] {
                if (alive.expired()) return;
                ui_.splice.loginUserCode = start.userCode;
                ui_.splice.loginUrl = start.verificationUri;
                ui_.splice.message = "Sign in in your browser (code " + start.userCode + ")";
                ++ui_.splice.revision; if (ui_.requestRepaint) ui_.requestRepaint();
                juce::URL(juce::String(start.verificationUriComplete)).launchInDefaultBrowser();
            });
            const int64_t deadline = juce::Time::currentTimeMillis() + (int64_t) start.expiresInSeconds * 1000;
            int interval = start.intervalSeconds > 0 ? start.intervalSeconds : 5;
            while (juce::Time::currentTimeMillis() < deadline && ! alive.expired())
            {
                juce::Thread::sleep(interval * 1000);
                const auto tokens = spliceStore_->pollLogin(start.deviceCode, cancel);
                if (tokens.ok)
                {
                    juce::MessageManager::callAsync([this, alive, refreshCredits] {
                        if (alive.expired()) return;
                        ui_.splice.auth = ui::UiContext::SpliceAuth::LoggedIn;
                        ui_.splice.message.clear();
                        ui_.splice.loginUserCode.clear();
                        ++ui_.splice.revision; if (ui_.requestRepaint) ui_.requestRepaint();
                        refreshCredits();
                    });
                    active->store(false);
                    return;
                }
                if (tokens.error == "slow_down") interval += 5;
                else if (tokens.error != "pending" && tokens.error != "network")
                    break; // access_denied / expired_token
            }
            juce::MessageManager::callAsync([this, alive] {
                if (alive.expired()) return;
                if (ui_.splice.auth == ui::UiContext::SpliceAuth::AwaitingLogin)
                {
                    ui_.splice.auth = ui::UiContext::SpliceAuth::LoggedOut;
                    ui_.splice.message = "Sign-in was not completed";
                    ++ui_.splice.revision; if (ui_.requestRepaint) ui_.requestRepaint();
                }
            });
            active->store(false);
        });
    };

    ui_.spliceSearch = [this, creditsPending, refreshCredits](const std::string& query) {
        if (*creditsPending) { *creditsPending = false; refreshCredits(); }
        ui_.splice.query = query;
        ui_.splice.loading = true;
        ui_.splice.message.clear();
        ++ui_.splice.revision; ++ui_.browserContentRevision; if (ui_.requestRepaint) ui_.requestRepaint();
        const int gen = ++(*spliceSearchGen_);
        const auto alive = std::weak_ptr<bool>(alive_);
        const auto genPtr = spliceSearchGen_;
        const std::string category = ui_.splice.category; // the Loop / One-shot filter
        const bool describe = ui_.splice.describe;         // SPL-9: semantic search
        splicePool_.addJob([this, alive, genPtr, gen, query, category, describe] {
            std::atomic<bool> cancel{false};
            SpliceSearchQuery q; q.text = query; q.limit = 50; q.category = category; q.semantic = describe ? 1.0f : 0.0f;
            platform::splice::SpliceSearchResult result;
            const bool hadToken = spliceStore_->withToken(cancel, [&](const std::string& token) {
                result = spliceStore_->api().searchSamples(token, q, cancel);
            });
            const bool needLogin = spliceStore_->takeNeedsLogin();
            juce::MessageManager::callAsync([this, alive, genPtr, gen, result, hadToken, needLogin] {
                if (alive.expired() || gen != genPtr->load()) return; // a newer search won
                ui_.splice.loading = false;
                if (needLogin) spliceRequireLogin("Sign in to search Splice");
                else if (! hadToken) ui_.splice.message = "Splice is not reachable (network)";
                else if (! result.ok) ui_.splice.message = result.error.empty() ? "Search failed" : result.error;
                else
                {
                    ui_.splice.message.clear();
                    ui_.splice.results.clear();
                    for (const auto& s : result.samples) ui_.splice.results.push_back(toRow(s));
                    ui_.splice.page = result.page;
                    ui_.splice.totalPages = result.totalPages;
                    ui_.splice.totalRecords = result.totalRecords;
                }
                ++ui_.splice.revision; ++ui_.browserContentRevision; if (ui_.requestRepaint) ui_.requestRepaint();
            });
        });
    };

    ui_.spliceLoadMore = [this] {
        auto& s = ui_.splice;
        if (s.loading || s.loadingMore || s.auth != ui::UiContext::SpliceAuth::LoggedIn) return;
        if (s.totalPages > 0 && s.page >= s.totalPages) return; // no more pages
        s.loadingMore = true;
        ++s.revision; if (ui_.requestRepaint) ui_.requestRepaint();
        const int gen = *spliceSearchGen_;            // a new search invalidates this append
        const int nextPage = s.page + 1;
        const std::string query = s.query, category = s.category;
        const bool describe = s.describe;
        const auto alive = std::weak_ptr<bool>(alive_);
        const auto genPtr = spliceSearchGen_;
        splicePool_.addJob([this, alive, genPtr, gen, nextPage, query, category, describe] {
            std::atomic<bool> cancel{false};
            SpliceSearchQuery q; q.text = query; q.limit = 50; q.page = nextPage; q.category = category; q.semantic = describe ? 1.0f : 0.0f;
            platform::splice::SpliceSearchResult result;
            spliceStore_->withToken(cancel, [&](const std::string& token) { result = spliceStore_->api().searchSamples(token, q, cancel); });
            const bool needLogin = spliceStore_->takeNeedsLogin();
            juce::MessageManager::callAsync([this, alive, genPtr, gen, result, needLogin] {
                if (alive.expired() || gen != genPtr->load()) return; // a newer search replaced the list
                ui_.splice.loadingMore = false;
                if (needLogin) spliceRequireLogin("Sign in to Splice to see more results");
                if (result.ok)
                {
                    for (const auto& smp : result.samples) ui_.splice.results.push_back(toRow(smp));
                    ui_.splice.page = result.page;
                    ui_.splice.totalPages = result.totalPages;
                    ui_.splice.totalRecords = result.totalRecords;
                }
                ++ui_.splice.revision; ++ui_.browserContentRevision; if (ui_.requestRepaint) ui_.requestRepaint();
            });
        });
    };

    ui_.spliceBuy = [this, refreshCredits](const std::string& uuid) { spliceBuyAsset(uuid, refreshCredits); };

    ui_.splicePreview = [this](const std::string& uuid) { splicePreviewRow(uuid); };

    ui_.splicePreviewTransport = [this](int cmd) {
        if (cmd == 0) // stop
        {
            ui_.splice.playingUuid.clear();
            ui_.splice.paused = false;
            engine_.setPreviewPaused(false);
            engine_.previewSample("");
        }
        else if (cmd == 1 && ! ui_.splice.playingUuid.empty()) // play/pause toggle
        {
            ui_.splice.paused = ! ui_.splice.paused;
            engine_.setPreviewPaused(ui_.splice.paused);
        }
        ++ui_.splice.revision; if (ui_.requestRepaint) ui_.requestRepaint();
    };

    ui_.spliceSetMatchKey = [this](bool on) {
        ui_.splice.matchKey = on;
        settings().setValue("spliceMatchKey", on);
        settings().saveIfNeeded();
        if (! ui_.splice.playingUuid.empty()) splicePreviewRow(ui_.splice.playingUuid); // re-audition with the new setting
        ++ui_.splice.revision; if (ui_.requestRepaint) ui_.requestRepaint();
    };

    ui_.spliceSetPreviewGain = [this](float gain) {
        ui_.splice.previewGain = std::clamp(gain, 0.0f, 1.0f);
        engine_.setPreviewGain(ui_.splice.previewGain);
        settings().setValue("splicePreviewGain", ui_.splice.previewGain);
        settings().saveIfNeeded();
        ++ui_.splice.revision; if (ui_.requestRepaint) ui_.requestRepaint();
    };

    ui_.spliceSetCategory = [this](const std::string& category) {
        if (ui_.splice.category == category) return;
        ui_.splice.category = category;
        ++ui_.splice.revision; if (ui_.requestRepaint) ui_.requestRepaint();
        if (ui_.spliceSearch) ui_.spliceSearch(ui_.splice.query); // re-run with the new filter
    };

    ui_.spliceSetDescribe = [this](bool on) {
        if (ui_.splice.describe == on) return;
        ui_.splice.describe = on;
        ++ui_.splice.revision; if (ui_.requestRepaint) ui_.requestRepaint();
        if (ui_.spliceSearch && ! ui_.splice.query.empty()) ui_.spliceSearch(ui_.splice.query); // re-run as a semantic search
    };

    logf("[sonora] Splice section %s", ui_.splice.enabled ? "enabled" : "off");
}

namespace
{
ui::UiContext::SpliceRow* findRow(ui::UiContext& ui, const std::string& uuid)
{
    for (auto& r : ui.splice.results) if (r.uuid == uuid) return &r;
    return nullptr;
}
} // namespace

// SPL-12: an action the user started (search, buy, place) found no valid Splice sign-in in the keychain, or Splice
// refused the stored one. Say why and start the sign-in at once (the browser opens on Splice's page with the code) instead
// of failing quietly. Launch-time checks never come here: they only show the signed-out state.
void AppController::spliceRequireLogin(const std::string& reason)
{
    auto& s = ui_.splice;
    if (! s.enabled) return;
    s.credits = -1;
    if (s.auth == ui::UiContext::SpliceAuth::AwaitingLogin) return; // a sign-in is already running
    s.auth = ui::UiContext::SpliceAuth::LoggedOut;
    s.message = reason;
    ui_.statusText = reason;
    ++s.revision; ++ui_.browserContentRevision; if (ui_.requestRepaint) ui_.requestRepaint();
    if (ui_.spliceLogin) ui_.spliceLogin();
}

int AppController::spliceTranspose(const ui::UiContext::SpliceRow& row) const
{
    if (! song_) return 0;
    const auto sample = platform::splice::parseSpliceKey(row.musicalKey, row.chordType);
    const auto project = platform::splice::projectKey(song_->scaleRoot, song_->scaleName);
    return platform::splice::transposeToProject(sample, project);
}

void AppController::splicePreviewRow(const std::string& uuid)
{
    if (uuid.empty()) { ui_.splice.playingUuid.clear(); engine_.previewSample(""); if (ui_.requestRepaint) ui_.requestRepaint(); return; }
    auto* row = findRow(ui_, uuid);
    if (! row || ! spliceStore_) return;
    ui_.splice.playingUuid = uuid;
    const double bpm = row->bpm;
    const int semis = ui_.splice.matchKey ? spliceTranspose(*row) : 0;
    const bool sync = row->loop && bpm >= 20;
    const auto file = previewCacheDir().getChildFile(juce::String(uuid) + ".mp3");
    if (file.existsAsFile())
    {
        engine_.previewSample({file.getFullPathName().toStdString(), bpm, semis, sync});
        if (ui_.requestRepaint) ui_.requestRepaint();
        return;
    }
    const std::string url = row->previewUrl;
    if (url.empty()) return;
    const auto alive = std::weak_ptr<bool>(alive_);
    splicePool_.addJob([this, alive, uuid, url, file, bpm, semis, sync] {
        std::atomic<bool> cancel{false};
        const auto bytes = spliceStore_->fetchPreview(url, cancel);
        if (! bytes.empty()) file.replaceWithData(bytes.data(), bytes.size());
        juce::MessageManager::callAsync([this, alive, uuid, file, bpm, semis, sync, ok = ! bytes.empty()] {
            if (alive.expired()) return;
            if (ui_.splice.playingUuid != uuid) return; // the user moved on
            if (ok) engine_.previewSample({file.getFullPathName().toStdString(), bpm, semis, sync});
            if (ui_.requestRepaint) ui_.requestRepaint();
        });
    });
}

void AppController::spliceBuyAsset(const std::string& uuid, const std::function<void()>& afterCredits)
{
    auto* row = findRow(ui_, uuid);
    if (! row || ! spliceStore_) return;
    if (row->licensed) { ui_.statusText = "Already in your library"; return; }
    row->busy = ui::UiContext::SpliceRow::Busy::Buying;
    ui_.statusText = "Buying " + row->fileName + "...";
    ++ui_.splice.revision; if (ui_.requestRepaint) ui_.requestRepaint();
    const auto alive = std::weak_ptr<bool>(alive_);
    splicePool_.addJob([this, alive, uuid, afterCredits] {
        std::atomic<bool> cancel{false};
        std::string hash;
        spliceStore_->withToken(cancel, [&](const std::string& token) { hash = spliceStore_->api().purchaseAsset(token, uuid, cancel); });
        const bool needLogin = spliceStore_->takeNeedsLogin();
        juce::MessageManager::callAsync([this, alive, uuid, hash, afterCredits, needLogin] {
            if (alive.expired()) return;
            if (needLogin) spliceRequireLogin("Sign in to Splice to buy this sample");
            if (auto* r = findRow(ui_, uuid))
            {
                r->busy = ui::UiContext::SpliceRow::Busy::None;
                if (! hash.empty()) { r->licensed = true; ui_.statusText = "Bought " + r->fileName; }
                else ui_.statusText = "Could not buy " + r->fileName;
            }
            ++ui_.splice.revision; if (ui_.requestRepaint) ui_.requestRepaint();
            if (! hash.empty() && afterCredits) afterCredits();
        });
    });
}

void AppController::spliceEnsureLocal(const std::string& uuid, std::function<void(std::string)> then)
{
    auto* row = findRow(ui_, uuid);
    if (! row || ! spliceStore_) { then({}); return; }
    if (! row->localPath.empty() && juce::File(juce::String::fromUTF8(row->localPath.c_str())).existsAsFile())
    { then(row->localPath); return; }
    row->busy = ui::UiContext::SpliceRow::Busy::Downloading;
    ++ui_.splice.revision; if (ui_.requestRepaint) ui_.requestRepaint();
    const auto alive = std::weak_ptr<bool>(alive_);
    const std::string fileName = row->fileName;
    splicePool_.addJob([this, alive, uuid, fileName, thenCb = std::move(then)] {
        std::atomic<bool> cancel{false};
        std::string localPath;
        spliceStore_->withToken(cancel, [&](const std::string& token) {
            localPath.clear();
            const auto hash = spliceStore_->api().purchaseAsset(token, uuid, cancel); // idempotent for an owned asset
            if (! hash.empty())
                if (const auto info = spliceStore_->api().premiumSample(token, hash, cancel); info.ok)
                {
                    const auto rel = info.path.empty() ? fileName : info.path;
                    const auto dest = spliceRoot().getChildFile(juce::String::fromUTF8(rel.c_str()));
                    if (spliceStore_->downloadWav(info.url, dest, cancel)) localPath = dest.getFullPathName().toStdString();
                }
        });
        const bool needLogin = spliceStore_->takeNeedsLogin();
        juce::MessageManager::callAsync([this, alive, uuid, localPath, needLogin, cb = std::move(thenCb)] {
            if (alive.expired()) return;
            if (needLogin) spliceRequireLogin("Sign in to Splice to download this sample");
            if (auto* r = findRow(ui_, uuid))
            {
                r->busy = ui::UiContext::SpliceRow::Busy::None;
                if (! localPath.empty()) { r->localPath = localPath; r->licensed = true; }
            }
            ++ui_.splice.revision; if (ui_.requestRepaint) ui_.requestRepaint();
            cb(localPath);
        });
    });
}

void AppController::dropSpliceSample(const ui::BrowserDrag& item, int track, double beat, int scene)
{
    auto* row = findRow(ui_, item.value);
    if (! row) { ui_.statusText = "That Splice sample is no longer listed"; return; }
    const double bpm = row->bpm;
    const int semis = ui_.splice.matchKey ? spliceTranspose(*row) : 0;
    const std::string label = row->fileName;
    ui_.statusText = "Getting " + label + " from Splice...";
    const auto alive = std::weak_ptr<bool>(alive_);
    const int dropGeneration = documentGeneration_;
    spliceEnsureLocal(item.value, [this, alive, label, bpm, semis, track, beat, scene, dropGeneration](std::string localPath) {
        if (alive.expired()) return;
        if (localPath.empty()) { ui_.statusText = "Could not get " + label + " from Splice"; return; }
        if (dropGeneration != documentGeneration_ || ! song_) return;
        if (track >= (int) song_->tracks.size()) { ui_.statusText = "Drop Splice samples on an audio track"; return; }
        juce::AudioFormatManager formats; formats.registerBasicFormats();
        const std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(juce::File(juce::String::fromUTF8(localPath.c_str()))));
        if (! reader) { ui_.statusText = "Could not read " + label; return; }
        io::DroppedSample sample{localPath, reader->sampleRate, (int64_t) reader->lengthInSamples, bpm, 1.0, semis};
        io::SampleDropTarget target{track, beat, scene, false};
        auto plan = io::placeSamples(*song_, {sample}, target);
        if (! plan.command.apply) { ui_.statusText = plan.error.empty() ? ("Could not place " + label) : plan.error; return; }
        const bool structural = plan.command.structureChanged;
        if (! applyEdit(std::move(plan.command)).ok) return;
        if (structural && plan.firstTrack >= 0 && plan.firstTrack < ui_.numTracks())
        { ui_.selectTrack(plan.firstTrack, false, false); if (ui_.modelChanged) ui_.modelChanged(); }
        ui_.statusText = "Inserted " + label + " at " + juce::String(bpm, 2).toStdString() + " BPM"
                       + (semis != 0 ? ", " + std::to_string(semis) + " st" : "");
    });
}

} // namespace sonora::app

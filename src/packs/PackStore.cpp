// LIB-1: catalogue + download worker (PackStore.h).
#include "packs/PackStore.h"

#include "core/Ed25519.h"
#include "platform/HttpClient.h"

#include <algorithm>
#include <chrono>

namespace sonora::packs
{
namespace
{
constexpr int kAttempts = 3;
constexpr int64_t kMaxCatalogueBytes = 4 * 1024 * 1024;

std::string readExact(const juce::File& file)
{
    juce::MemoryBlock data;
    if (! file.loadFileAsData(data)) return {};
    return std::string(static_cast<const char*>(data.getData()), data.getSize());
}

std::string outcomeText(const platform::DownloadResult& result)
{
    using Outcome = platform::HttpResponse::Outcome;
    switch (result.outcome)
    {
        case Outcome::Completed: return "the server answered HTTP " + std::to_string(result.status);
        case Outcome::InvalidUrl: return "the address is not https";
        case Outcome::TooLarge: return "the file is larger than the pack list says";
        case Outcome::TimedOut: return "the connection timed out";
        case Outcome::Cancelled: return "cancelled";
        case Outcome::NetworkFailed: break;
    }
    return "the network connection failed";
}

bool fetched(const platform::DownloadResult& result)
{
    return result.outcome == platform::HttpResponse::Outcome::Completed && result.status == 200;
}
} // namespace

const char* toString(PackStore::PackState state)
{
    switch (state)
    {
        case PackStore::PackState::Available: return "available";
        case PackStore::PackState::Queued: return "queued";
        case PackStore::PackState::Downloading: return "downloading";
        case PackStore::PackState::Verifying: return "verifying";
        case PackStore::PackState::Installing: return "installing";
        case PackStore::PackState::Installed: return "installed";
        case PackStore::PackState::UpdateAvailable: return "update available";
        case PackStore::PackState::Failed: return "failed";
    }
    return "?";
}

PackStore::PackStore(Config config, std::function<void()> changed)
    : config_(std::move(config)), library_(config_.root), changed_(std::move(changed))
{
    state_.catalogue = configured() ? CatalogueState::NotLoaded : CatalogueState::Off;
    worker_ = std::thread([this] { workerLoop(); });
    enqueue({}, [this] {
        loadCachedCatalogue();
        const std::lock_guard lock(mutex_);
        scanLocked();
    });
}

PackStore::~PackStore()
{
    {
        const std::lock_guard lock(mutex_);
        stopping_ = true;
        queue_.clear();
    }
    cancelRunning_ = true;
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

bool PackStore::configured() const
{
    return config_.catalogueUrl.isNotEmpty() && ! config_.publicKey.empty() && crypto::ed25519Available();
}

void PackStore::enqueue(std::string id, std::function<void()> run)
{
    {
        const std::lock_guard lock(mutex_);
        if (stopping_) return;
        queue_.push_back({std::move(id), std::move(run)});
    }
    wake_.notify_all();
}

void PackStore::workerLoop()
{
    for (;;)
    {
        Job job;
        {
            std::unique_lock lock(mutex_);
            running_ = false;
            runningId_.clear();
            idle_.notify_all();
            wake_.wait(lock, [this] { return stopping_ || ! queue_.empty(); });
            if (stopping_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
            if (! job.id.empty() && cancelledIds_.erase(job.id) > 0) continue;
            running_ = true;
            runningId_ = job.id;
            cancelRunning_ = false;
        }
        job.run();
        notify();
    }
}

void PackStore::waitUntilIdle() const
{
    std::unique_lock lock(mutex_);
    idle_.wait(lock, [this] { return stopping_ || (queue_.empty() && ! running_); });
}

void PackStore::scanLocked()
{
    installed_ = library_.installed();
    for (const auto& entry : catalogue_)
    {
        const auto part = library_.partialDownload(entry);
        if (part.existsAsFile() && ! transient_.contains(entry.info.id)) progress_[entry.info.id] = {part.getSize(), entry.bytes};
    }
}

void PackStore::notify()
{
    {
        const std::lock_guard lock(mutex_);
        std::vector<Row> rows;
        for (const auto& pack : installed_)
        {
            Row row;
            row.info = pack.contents.info;
            row.installed = pack;
            const auto entry = std::find_if(catalogue_.begin(), catalogue_.end(), [&](const auto& e) { return e.info.id == row.info.id; });
            if (entry != catalogue_.end()) row.entry = *entry;
            row.state = row.entry && row.entry->info.version != row.info.version ? PackState::UpdateAvailable : PackState::Installed;
            rows.push_back(std::move(row));
        }
        for (const auto& entry : catalogue_)
            if (std::none_of(rows.begin(), rows.end(), [&](const Row& r) { return r.info.id == entry.info.id; }))
            {
                Row row;
                row.info = entry.info;
                row.entry = entry;
                rows.push_back(std::move(row));
            }
        for (auto& row : rows)
        {
            if (const auto t = transient_.find(row.info.id); t != transient_.end())
            {
                row.state = t->second.first;
                row.message = t->second.second;
            }
            if (const auto p = progress_.find(row.info.id); p != progress_.end()) std::tie(row.received, row.total) = p->second;
            else if (row.entry) row.total = row.entry->bytes;
        }
        std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            if (a.installed.has_value() != b.installed.has_value()) return a.installed.has_value();
            return juce::String(a.info.name).compareNatural(juce::String(b.info.name)) < 0;
        });
        state_.rows = std::move(rows);
        ++state_.revision;
    }
    if (changed_) changed_();
}

PackStore::Snapshot PackStore::snapshot() const
{
    const std::lock_guard lock(mutex_);
    return state_;
}

void PackStore::loadCatalogue()
{
    if (! configured()) return;
    {
        const std::lock_guard lock(mutex_);
        if (state_.catalogue == CatalogueState::Loading) return;
        state_.catalogue = CatalogueState::Loading;
        state_.catalogueMessage.clear();
    }
    notify();
    enqueue({}, [this] { runCatalogue(); });
}

void PackStore::rescan()
{
    enqueue({}, [this] {
        const std::lock_guard lock(mutex_);
        scanLocked();
    });
}

void PackStore::download(const std::string& id)
{
    if (! configured()) return;
    {
        const std::lock_guard lock(mutex_);
        if (const auto t = transient_.find(id); t != transient_.end() && t->second.first != PackState::Failed) return;
        transient_[id] = {PackState::Queued, {}};
        cancelledIds_.erase(id);
    }
    notify();
    enqueue(id, [this, id] { runDownload(id); });
}

void PackStore::cancel(const std::string& id)
{
    {
        const std::lock_guard lock(mutex_);
        if (runningId_ == id) cancelRunning_ = true;
        else if (std::any_of(queue_.begin(), queue_.end(), [&](const Job& job) { return job.id == id; }))
        {
            cancelledIds_.insert(id);
            transient_.erase(id);
        }
    }
    notify();
}

void PackStore::remove(const std::string& id)
{
    cancel(id);
    enqueue({}, [this, id] { runRemove(id); }); // no id: a later cancel(id) must not skip the removal
}

void PackStore::loadCachedCatalogue()
{
    if (! configured()) return;
    const auto folder = library_.privateFolder().getChildFile("catalogue");
    const auto json = readExact(folder.getChildFile("packs.json"));
    const auto signature = folder.getChildFile("packs.json.sig").loadFileAsString().trim().toStdString();
    if (json.empty() || ! verifyCatalogue(json, signature, config_.publicKey)) return;
    std::string error;
    if (auto catalogue = parseCatalogue(json, error))
    {
        const std::lock_guard lock(mutex_);
        catalogue_ = std::move(catalogue->packs);
        state_.catalogue = CatalogueState::Ready;
    }
}

void PackStore::runCatalogue()
{
    const auto folder = library_.privateFolder().getChildFile("catalogue");
    folder.createDirectory();
    const auto json = folder.getChildFile("packs.json.download"), signature = folder.getChildFile("packs.json.sig.download");
    json.deleteFile();
    signature.deleteFile();
    auto fail = [&](std::string message) {
        json.deleteFile();
        signature.deleteFile();
        const std::lock_guard lock(mutex_);
        state_.catalogue = CatalogueState::Failed;
        state_.catalogueMessage = catalogue_.empty() ? message : message + " (showing the last pack list)";
    };
    const auto list = platform::downloadToFile({config_.catalogueUrl, json, kMaxCatalogueBytes, 20, 5}, {}, cancelRunning_);
    if (! fetched(list)) { fail("Could not load the pack list: " + outcomeText(list)); return; }
    const auto sig = platform::downloadToFile({config_.catalogueUrl + ".sig", signature, 4096, 20, 5}, {}, cancelRunning_);
    if (! fetched(sig)) { fail("Could not load the pack list's signature: " + outcomeText(sig)); return; }
    const auto text = readExact(json);
    if (! verifyCatalogue(text, signature.loadFileAsString().trim().toStdString(), config_.publicKey))
    { fail("The pack list's signature does not match: nothing from it is used"); return; }
    std::string error;
    auto catalogue = parseCatalogue(text, error);
    if (! catalogue) { fail("The pack list is unreadable: " + error); return; }
    json.moveFileTo(folder.getChildFile("packs.json"));
    signature.moveFileTo(folder.getChildFile("packs.json.sig"));
    for (const auto& warning : catalogue->warnings) DBG("[sonora] packs: skipped " << warning);
    const std::lock_guard lock(mutex_);
    catalogue_ = std::move(catalogue->packs);
    state_.catalogue = CatalogueState::Ready;
    state_.catalogueMessage.clear();
    scanLocked();
}

void PackStore::runDownload(const std::string& id)
{
    std::optional<PackEntry> found;
    {
        const std::lock_guard lock(mutex_);
        const auto entry = std::find_if(catalogue_.begin(), catalogue_.end(), [&](const auto& e) { return e.info.id == id; });
        if (entry != catalogue_.end()) found = *entry;
    }
    auto finish = [&](PackState state, std::string message) {
        const std::lock_guard lock(mutex_);
        if (state == PackState::Available) transient_.erase(id);
        else transient_[id] = {state, std::move(message)};
    };
    if (! found) { finish(PackState::Failed, "This pack is no longer in the pack list"); return; }
    const auto entry = *found;
    const auto url = resolveArchiveUrl(config_.catalogueUrl, entry.url);
    if (! platform::downloadUrlAllowed(url)) { finish(PackState::Failed, "The pack's address is not https"); return; }
    const auto part = library_.partialDownload(entry);
    part.getParentDirectory().createDirectory();
    for (const auto& stale : library_.stalePartials(id, part)) stale.deleteFile();
    if (part.existsAsFile() && part.getSize() > entry.bytes) part.deleteFile();

    auto setProgress = [&](int64_t received) {
        const std::lock_guard lock(mutex_);
        progress_[id] = {received, entry.bytes};
    };
    {
        const std::lock_guard lock(mutex_);
        transient_[id] = {PackState::Downloading, {}};
        progress_[id] = {part.existsAsFile() ? part.getSize() : 0, entry.bytes};
    }
    notify();
    auto paused = [&] {
        setProgress(part.existsAsFile() ? part.getSize() : 0);
        finish(PackState::Available, {});
    };
    for (int attempt = 1;; ++attempt)
    {
        if (part.existsAsFile() && part.getSize() == entry.bytes) break; // complete from an earlier run
        const auto result = platform::downloadToFile({url, part, entry.bytes, 30, 5},
                                                     [&](int64_t received, int64_t) { setProgress(received); notify(); }, cancelRunning_);
        using Outcome = platform::HttpResponse::Outcome;
        if (result.outcome == Outcome::Cancelled || cancelRunning_) { paused(); return; }
        if (result.outcome == Outcome::Completed && (result.status == 200 || result.status == 206 || result.status == 416)) break;
        if (result.outcome == Outcome::Completed || result.outcome == Outcome::InvalidUrl)
        { finish(PackState::Failed, "Download failed: " + outcomeText(result)); return; }
        if (result.outcome == Outcome::TooLarge) { part.deleteFile(); finish(PackState::Failed, "Download failed: " + outcomeText(result)); return; }
        if (attempt >= kAttempts)
        {
            setProgress(part.existsAsFile() ? part.getSize() : 0);
            finish(PackState::Failed, "Download stopped: " + outcomeText(result) + ". Download again to resume.");
            return;
        }
        for (int waited = 0; waited < config_.retryDelayMs && ! cancelRunning_; waited += 50)
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const auto size = part.existsAsFile() ? part.getSize() : 0;
    if (size != entry.bytes)
    {
        if (size > entry.bytes) part.deleteFile();
        setProgress(part.existsAsFile() ? part.getSize() : 0);
        finish(PackState::Failed, "Download incomplete. Download again to resume.");
        return;
    }
    finish(PackState::Verifying, {});
    notify();
    const auto hash = sha256OfFile(part, cancelRunning_);
    if (cancelRunning_) { paused(); return; }
    if (hash != entry.sha256)
    {
        part.deleteFile();
        {
            const std::lock_guard lock(mutex_);
            progress_.erase(id);
        }
        finish(PackState::Failed, "The download does not match the pack list's checksum and was discarded");
        return;
    }
    finish(PackState::Installing, {});
    notify();
    const auto installed = library_.install(part, entry, cancelRunning_);
    if (! installed.ok)
    {
        if (cancelRunning_) { paused(); return; }
        finish(PackState::Failed, "Install failed: " + installed.error);
        return;
    }
    part.deleteFile();
    part.getParentDirectory().deleteFile(); // only when empty
    const std::lock_guard lock(mutex_);
    transient_.erase(id);
    progress_.erase(id);
    installed_ = library_.installed();
}

void PackStore::runRemove(const std::string& id)
{
    std::string error;
    const bool removed = library_.remove(id, error, config_.useTrash);
    const std::lock_guard lock(mutex_);
    if (removed) transient_.erase(id);
    else transient_[id] = {PackState::Failed, "Remove failed: " + error};
    installed_ = library_.installed();
}

} // namespace sonora::packs

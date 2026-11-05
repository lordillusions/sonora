#pragma once
// LIB-1: the sample-pack catalogue and downloads behind the Browser's Packs list (docs/SAMPLE_PACKS.md).
//
// One worker thread runs every network and disk job in order: fetch + verify the signed catalogue, download (resumable,
// three tries), check the archive's SHA-256 and size, unpack, remove. Nothing runs until the caller asks: no catalogue
// fetch and no download happens by itself. Public methods are thread-safe; `changed` is called on the worker (at most
// every ~100 ms while downloading), so the app marshals it to the message thread before reading snapshot().

#include "packs/PackLibrary.h"

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

namespace sonora::packs
{

class PackStore
{
public:
    struct Config
    {
        juce::String catalogueUrl; // packs.json; the signature is catalogueUrl + ".sig"
        std::string publicKey;     // base64 Ed25519; empty = downloads are off in this build
        juce::File root;           // install folder (PackLibrary)
        bool useTrash = true;      // remove() moves packs to the Trash (tests delete)
        int retryDelayMs = 1000;
    };
    enum class CatalogueState { Off, NotLoaded, Loading, Ready, Failed };
    enum class PackState { Available, Queued, Downloading, Verifying, Installing, Installed, UpdateAvailable, Failed };
    struct Row
    {
        PackInfo info;                       // the catalogue's, or the installed pack.json's when it is not listed
        std::optional<PackEntry> entry;      // in the catalogue
        std::optional<InstalledPack> installed;
        PackState state = PackState::Available;
        int64_t received = 0, total = 0;     // download progress (bytes); a paused partial keeps `received`
        std::string message;                 // the last failure, or ""
    };
    struct Snapshot
    {
        CatalogueState catalogue = CatalogueState::Off;
        std::string catalogueMessage;
        std::vector<Row> rows; // installed first, then by name
        uint64_t revision = 0;
    };

    PackStore(Config config, std::function<void()> changed);
    ~PackStore(); // cancels the running job and joins the worker
    PackStore(const PackStore&) = delete;
    PackStore& operator=(const PackStore&) = delete;

    bool configured() const;
    const PackLibrary& library() const { return library_; }
    void loadCatalogue();     // fetch + verify; on failure the last verified copy (if any) stays listed
    void rescan();            // re-read the install folder (e.g. after the user deleted a pack by hand)
    void download(const std::string& id);
    void cancel(const std::string& id); // pauses: the partial file stays and the next download resumes it
    void remove(const std::string& id);
    Snapshot snapshot() const;
    void waitUntilIdle() const; // tests and tools

private:
    struct Job { std::string id; std::function<void()> run; };
    void enqueue(std::string id, std::function<void()> run);
    void workerLoop();
    void notify();
    void runCatalogue();
    void runDownload(const std::string& id);
    void runRemove(const std::string& id);
    void scanLocked();
    void loadCachedCatalogue();

    const Config config_;
    const PackLibrary library_;
    const std::function<void()> changed_;
    mutable std::mutex mutex_;
    mutable std::condition_variable wake_, idle_;
    std::deque<Job> queue_;
    std::set<std::string> cancelledIds_;
    std::string runningId_;
    bool running_ = false, stopping_ = false;
    std::atomic<bool> cancelRunning_{false};
    Snapshot state_;
    std::vector<PackEntry> catalogue_;
    std::vector<InstalledPack> installed_;
    std::map<std::string, std::pair<PackState, std::string>> transient_; // id -> state/message while not idle
    std::map<std::string, std::pair<int64_t, int64_t>> progress_;
    std::thread worker_;
};

const char* toString(PackStore::PackState state);

} // namespace sonora::packs

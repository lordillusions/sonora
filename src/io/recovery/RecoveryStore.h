#pragma once
// Crash recovery (PRJ-4): the files behind autosave and "recover on next launch".
//
//   <data folder>/Recovery/<started> pid <pid>/      one folder per running Sonora (several instances never share one)
//       running.lock      pid, start time, version; exists while that run is alive, removed by a clean quit
//       Autosave.als      the latest autosaved copy of the open set (written through a temporary file)
//       Recovered.als     a copy taken over from a crashed run: the base a recovered set is patched onto until saved
//       recovery.xml      which of the two is the newest copy, the set's own path, when it was written
//
// A folder whose lock names a dead process (or a lock nobody refreshed for kStaleLockMs) belongs to a run that did not
// quit cleanly; when it holds a copy, the next launch offers it. Nothing here ever writes into the user's project or
// their set file. No signal handler is involved: the lock itself is the "did not quit cleanly" marker.
//
// No UI, no audio; the app decides when (AutosavePolicy) and copies the model, this only manages files.

#include <juce_core/juce_core.h>

#include <string>
#include <vector>

namespace sonora::io::recovery
{

inline constexpr const char* kLockFile = "running.lock";
inline constexpr const char* kInfoFile = "recovery.xml";
inline constexpr const char* kAutosaveFile = "Autosave.als";
inline constexpr const char* kRecoveredFile = "Recovered.als";
// A live run refreshes its lock every minute; one older than this is treated as dead even if its pid exists (pid reuse).
inline constexpr juce::int64 kStaleLockMs = 10 * 60 * 1000;
inline constexpr juce::int64 kKeepDeclinedMs = 7LL * 24 * 60 * 60 * 1000; // copies the user did not recover
inline constexpr int kKeepCopies = 5;                                      // at most this many orphaned copies

struct Entry
{
    juce::File folder;
    juce::File setFile;              // the newest copy; nonexistent = nothing to recover
    std::string documentPath;        // the set the copy belongs to (where Save writes it)
    std::string setName;             // shown in the prompt ("Untitled" for a never-saved project)
    bool untitled = false;
    juce::int64 savedAtMs = 0;       // when the copy was written
    juce::int64 documentModifiedMs = 0; // the set file's modification time when the copy was written (0: none)
    std::string appVersion;
    bool declined = false;           // offered and not recovered (kept for kKeepDeclinedMs)
    int pid = 0;
    bool running = false;            // its lock names a live process: another Sonora is using it

    bool hasCopy() const { return setFile != juce::File() && setFile.existsAsFile(); }
    // The set file was saved again (by Sonora or another app) after this copy was made.
    bool documentChangedSince() const;
};

int currentProcessId();
bool processAlive(int pid);

class Store
{
public:
    explicit Store(juce::File root) : root_(std::move(root)) {}

    // Creates this run's folder and its lock. False when the folder cannot be written (autosave then stays off).
    bool start(const std::string& appVersion, juce::int64 nowMs = juce::Time::currentTimeMillis());
    bool started() const { return folder_ != juce::File(); }
    const juce::File& root() const { return root_; }
    const juce::File& folder() const { return folder_; }
    juce::File autosaveFile() const { return folder_.getChildFile(kAutosaveFile); }
    juce::File recoveredFile() const { return folder_.getChildFile(kRecoveredFile); }

    // Every other run's folder, newest first, with `running` worked out from its lock.
    std::vector<Entry> scan(juce::int64 nowMs = juce::Time::currentTimeMillis()) const;
    // The copies to offer: runs that are not alive, not declined and have a copy; newest first.
    std::vector<Entry> recoverable(juce::int64 nowMs = juce::Time::currentTimeMillis()) const;
    // Rotation: removes other runs' folders that hold nothing to recover, declined copies older than kKeepDeclinedMs and
    // all but the newest kKeepCopies copies. Folders of live runs are never touched. Returns how many were removed.
    int prune(juce::int64 nowMs = juce::Time::currentTimeMillis()) const;

    void heartbeat() const; // refreshes the lock's modification time (a live run does this every minute)
    // Records this run's newest copy (atomically). `info.folder` is ignored; the copy must be inside this run's folder.
    bool commit(const Entry& info) const;
    // The open set was saved, closed or is clean again: its copies go, except `keep` (still the base of the open set).
    void clearDocument(const juce::File& keep = {}) const;
    // A clean quit: this run's folder goes away.
    void finish();

    static bool decline(const Entry& entry); // the user said no: kept (marked) until rotation removes it
    static Entry read(const juce::File& folder, juce::int64 nowMs = juce::Time::currentTimeMillis());

private:
    juce::File root_, folder_;
};

// When the app autosaves (message thread, polled ~10 times a second). Times in ms.
struct AutosaveInputs
{
    bool documentOpen = false;
    bool dirty = false;               // unsaved changes
    bool changedSinceAutosave = false; // the edit history moved since the last copy
    bool recording = false;           // never while recording (the take is not in the model yet; disk is busy)
    bool rendering = false;           // never while an offline render (bounce, freeze, export) runs
    bool busy = false;                // a copy is being written, a device edit or a mouse gesture is in progress
    double sinceLastChangeMs = 0;     // since the edit history last moved
    double sincePendingMs = 0;        // since the oldest change that is not in a copy yet
    double sinceLastAutosaveMs = 1e12;
    double lastWriteMs = 0;           // how long writing the previous copy took (a big set: seconds)
};

struct AutosavePolicy
{
    double idleMs = 5000;             // a copy once the user paused for this long after an edit
    double maxDelayMs = 60000;        // and at least this often while edits keep coming
    double minIntervalMs = 15000;     // never more often than this
    double writeShare = 0.1;          // and the writer busy at most this share of the time (a 5 s write: every 50 s)
};

bool autosaveDue(const AutosaveInputs& in, const AutosavePolicy& policy = {});

} // namespace sonora::io::recovery

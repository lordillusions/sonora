#include "io/recovery/RecoveryStore.h"

#include <algorithm>

#if defined(_WIN32)
 #define WIN32_LEAN_AND_MEAN
 #define NOMINMAX
 #include <windows.h>
#else
 #include <cerrno>
 #include <csignal>
 #include <unistd.h>
#endif

namespace sonora::io::recovery
{

int currentProcessId()
{
#if defined(_WIN32)
    return (int) GetCurrentProcessId();
#else
    return (int) getpid();
#endif
}

bool processAlive(int pid)
{
    if (pid <= 0) return false;
#if defined(_WIN32)
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, (DWORD) pid);
    if (process == nullptr) return GetLastError() == ERROR_ACCESS_DENIED;
    const bool alive = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return alive;
#else
    return kill((pid_t) pid, 0) == 0 || errno == EPERM;
#endif
}

bool Entry::documentChangedSince() const
{
    if (documentPath.empty() || ! juce::File::isAbsolutePath(juce::String::fromUTF8(documentPath.c_str()))) return false;
    const juce::File document(juce::String::fromUTF8(documentPath.c_str()));
    if (! document.existsAsFile()) return false;
    // A second of slack: some file systems store modification times in whole seconds.
    return document.getLastModificationTime().toMilliseconds() > std::max(documentModifiedMs, savedAtMs) + 1000;
}

namespace
{
int lockPid(const juce::File& lock)
{
    for (const auto& line : juce::StringArray::fromLines(lock.loadFileAsString()))
        if (line.startsWith("pid="))
            return line.fromFirstOccurrenceOf("=", false, false).getIntValue();
    return 0;
}

bool writeAtomically(const juce::File& file, const juce::String& text)
{
    juce::TemporaryFile temp(file);
    return temp.getFile().replaceWithText(text) && temp.overwriteTargetFileWithTemporary();
}
} // namespace

bool Store::start(const std::string& appVersion, juce::int64 nowMs)
{
    if (! root_.createDirectory()) return false;
    const auto pid = currentProcessId();
    const auto name = juce::Time(nowMs).formatted("%Y-%m-%d %H-%M-%S") + " pid " + juce::String(pid);
    auto folder = root_.getChildFile(name);
    if (folder.exists()) folder = root_.getNonexistentChildFile(name, "", false);
    if (! folder.createDirectory()) return false;
    const auto lock = juce::String("pid=") + juce::String(pid) + "\nstarted=" + juce::String(nowMs)
                      + "\nversion=" + juce::String::fromUTF8(appVersion.c_str()) + "\n";
    if (! folder.getChildFile(kLockFile).replaceWithText(lock))
    {
        folder.deleteRecursively();
        return false;
    }
    folder_ = folder;
    return true;
}

Entry Store::read(const juce::File& folder, juce::int64 nowMs)
{
    Entry e;
    e.folder = folder;
    const auto lock = folder.getChildFile(kLockFile);
    if (lock.existsAsFile())
    {
        e.pid = lockPid(lock);
        // Our own pid in another folder is an earlier run whose pid this one inherited (after a restart).
        const bool fresh = nowMs - lock.getLastModificationTime().toMilliseconds() < kStaleLockMs;
        e.running = e.pid != currentProcessId() && processAlive(e.pid) && fresh;
    }
    if (auto xml = juce::parseXML(folder.getChildFile(kInfoFile)); xml && xml->hasTagName("SonoraRecovery"))
    {
        const auto set = xml->getStringAttribute("set");
        // Only a plain file name inside the folder: never follow a path out of it.
        if (set.isNotEmpty() && ! set.containsAnyOf("/\\:") && set != "." && set != "..")
            e.setFile = folder.getChildFile(set);
        e.documentPath = xml->getStringAttribute("document").toStdString();
        e.setName = xml->getStringAttribute("name").toStdString();
        e.untitled = xml->getBoolAttribute("untitled");
        e.savedAtMs = (juce::int64) xml->getDoubleAttribute("savedAt"); // also reads "1.79e12"
        e.documentModifiedMs = (juce::int64) xml->getDoubleAttribute("documentModified");
        e.appVersion = xml->getStringAttribute("appVersion").toStdString();
        e.declined = xml->getBoolAttribute("declined");
    }
    if (e.savedAtMs == 0 && e.hasCopy()) e.savedAtMs = e.setFile.getLastModificationTime().toMilliseconds();
    return e;
}

std::vector<Entry> Store::scan(juce::int64 nowMs) const
{
    std::vector<Entry> entries;
    if (! root_.isDirectory()) return entries;
    for (const auto& folder : root_.findChildFiles(juce::File::findDirectories, false))
        if (folder != folder_)
            entries.push_back(read(folder, nowMs));
    std::sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        const auto ta = a.savedAtMs ? a.savedAtMs : a.folder.getLastModificationTime().toMilliseconds();
        const auto tb = b.savedAtMs ? b.savedAtMs : b.folder.getLastModificationTime().toMilliseconds();
        return ta > tb;
    });
    return entries;
}

std::vector<Entry> Store::recoverable(juce::int64 nowMs) const
{
    std::vector<Entry> found;
    for (auto& e : scan(nowMs))
        if (! e.running && ! e.declined && e.hasCopy()) found.push_back(std::move(e));
    return found;
}

int Store::prune(juce::int64 nowMs) const
{
    int removed = 0, kept = 0;
    for (const auto& e : scan(nowMs)) // newest first
    {
        if (e.running) continue;
        bool remove = ! e.hasCopy();
        if (! remove && e.declined && nowMs - e.savedAtMs > kKeepDeclinedMs) remove = true;
        if (! remove && ++kept > kKeepCopies) remove = true;
        if (remove && e.folder.isAChildOf(root_) && e.folder.deleteRecursively()) ++removed;
    }
    return removed;
}

void Store::heartbeat() const
{
    if (started()) (void) folder_.getChildFile(kLockFile).setLastModificationTime(juce::Time::getCurrentTime());
}

bool Store::commit(const Entry& info) const
{
    if (! started() || ! info.setFile.isAChildOf(folder_)) return false;
    juce::XmlElement xml("SonoraRecovery");
    xml.setAttribute("format", 1);
    xml.setAttribute("set", info.setFile.getFileName());
    xml.setAttribute("document", juce::String::fromUTF8(info.documentPath.c_str()));
    xml.setAttribute("name", juce::String::fromUTF8(info.setName.c_str()));
    xml.setAttribute("untitled", info.untitled);
    xml.setAttribute("savedAt", juce::String(info.savedAtMs));
    xml.setAttribute("documentModified", juce::String(info.documentModifiedMs));
    xml.setAttribute("appVersion", juce::String::fromUTF8(info.appVersion.c_str()));
    if (info.declined) xml.setAttribute("declined", true);
    return writeAtomically(folder_.getChildFile(kInfoFile), xml.toString());
}

void Store::clearDocument(const juce::File& keep) const
{
    if (! started()) return;
    (void) folder_.getChildFile(kInfoFile).deleteFile();
    for (const auto* name : {kAutosaveFile, kRecoveredFile})
        if (const auto file = folder_.getChildFile(name); file != keep)
            (void) file.deleteFile();
}

void Store::finish()
{
    if (started() && folder_.isAChildOf(root_)) (void) folder_.deleteRecursively();
    folder_ = juce::File();
}

bool Store::decline(const Entry& entry)
{
    auto xml = juce::parseXML(entry.folder.getChildFile(kInfoFile));
    if (! xml) return false;
    xml->setAttribute("declined", true);
    (void) entry.folder.getChildFile(kLockFile).deleteFile(); // nothing will run in it again
    return writeAtomically(entry.folder.getChildFile(kInfoFile), xml->toString());
}

bool autosaveDue(const AutosaveInputs& in, const AutosavePolicy& policy)
{
    if (! in.documentOpen || ! in.dirty || ! in.changedSinceAutosave) return false;
    if (in.recording || in.rendering || in.busy) return false;
    const double spacing = std::max(policy.minIntervalMs, policy.writeShare > 0 ? in.lastWriteMs / policy.writeShare : 0.0);
    if (in.sinceLastAutosaveMs < spacing) return false;
    return in.sinceLastChangeMs >= policy.idleMs || in.sincePendingMs >= policy.maxDelayMs;
}

} // namespace sonora::io::recovery

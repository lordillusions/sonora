#pragma once
// COM-15: one folder's entries from the directory listing alone. The entries are not opened or asked for attributes:
// on macOS JUCE's iterator asks Foundation (NSDirectoryEnumerator, NSURL resource values) about every child, and doing
// that to ~/Music/Music (the Apple Music library) is enough for macOS to ask for "Media & Apple Music" although the
// folder is never entered. Here a POSIX readdir() gives names and types; only an entry of unknown type is stat'ed.
// Links are reported as links, unresolved, so the caller can decide from the target's path before touching it.
// Windows has no such prompts and keeps JUCE's iterator.

#include <juce_core/juce_core.h>

#include <vector>

#if ! defined(_WIN32)
    #include <dirent.h>
    #include <sys/stat.h>
#endif

namespace sonora::platform
{

struct FolderEntry
{
    juce::File file;
    bool directory = false; // false for a link (see `link`)
    bool link = false;      // a symbolic link: resolve it before use
};

// Entries of `folder` without "." and ".." and without hidden names (a leading '.').
inline std::vector<FolderEntry> listFolder(const juce::File& folder)
{
    std::vector<FolderEntry> entries;
#if defined(_WIN32)
    for (const auto& entry : juce::RangedDirectoryIterator(folder, false, "*", juce::File::findFilesAndDirectories | juce::File::ignoreHiddenFiles,
                                                           juce::File::FollowSymlinks::no))
    {
        // Keep the caller's spelling of the folder: the iterator may give the long form of a path the caller holds in
        // its short (8.3) form, and the guard / cycle checks compare paths.
        if (entry.getFile().getFileName().startsWithChar('.')) continue; // hidden by name, as on macOS/Linux
        const auto file = folder.getChildFile(entry.getFile().getFileName());
        const bool link = file.isSymbolicLink();
        entries.push_back({file, entry.isDirectory() && ! link, link});
    }
#else
    DIR* dir = opendir(folder.getFullPathName().toRawUTF8());
    if (dir == nullptr) return entries;
    while (const auto* d = readdir(dir))
    {
        if (d->d_name[0] == '.') continue;
        FolderEntry entry;
        entry.file = folder.getChildFile(juce::String::fromUTF8(d->d_name));
        entry.directory = d->d_type == DT_DIR;
        entry.link = d->d_type == DT_LNK;
        if (d->d_type == DT_UNKNOWN)
        {
            struct stat info {};
            if (lstat(entry.file.getFullPathName().toRawUTF8(), &info) == 0)
            {
                entry.link = S_ISLNK(info.st_mode);
                entry.directory = S_ISDIR(info.st_mode);
            }
        }
        entries.push_back(std::move(entry));
    }
    closedir(dir);
#endif
    return entries;
}

} // namespace sonora::platform

#pragma once
// Serum 2 library compatibility (UI-28d): finds the user's installed Serum 2 presets and wavetables at runtime and reads
// tables with the measured reference frame law. Nothing from these folders is copied into the repository or the app;
// a loaded table is stored in the set like any other imported table.
#include "devices/native/WavetableData.h"

#include <juce_core/juce_core.h>

#include <optional>
#include <string>
#include <vector>

namespace sonora::plugins
{
// Every table the reference plays is 1.00316 louder than its stored samples (measured: output level, PD/AM source depth,
// file and embedded tables alike); file tables are additionally normalised to a global peak of 1 at load.
inline constexpr float serumTableGain = 1.00316f;
// Serum wavetable files carry a "clm " RIFF chunk such as "<!>2048 01000000 wavetable ...". Its first flag digit selects
// the frame layout: 0 plays whole frames, any other value crossfades on a 256-cell grid (measured on factory tables
// with flags 0..4). Returns -1 when the chunk is absent or malformed.
int serumTableFlag(const juce::File& file);
// A table with the reference layout/gain/normalisation when it carries a Serum chunk, else Sonora's continuous layout.
devices::WavetableLoadResult loadSerumTable(const juce::File& file);

struct SerumLibraryItem
{
    std::string path, name; // absolute file, display name (file name without extension)
    std::string folder;     // "/"-separated folder below the library kind ("Factory/Bass"); empty at the top
};
// Installed "Serum 2 Presets" roots: the system library first, then the user's.
std::vector<juce::File> serumLibraryRoots();
// What the Prism browser and menus list (UI-50): Sonora Prism's own library (prismLibraryRoot(), when it exists) first,
// then serumLibraryRoots(). Banks with the same folder name merge.
std::vector<juce::File> presetLibraryRoots();
// Every file of a library kind ("Presets" -> .prism and .SerumPreset, "Tables" -> .wav) across presetLibraryRoots(),
// sorted by folder then name.
std::vector<SerumLibraryItem> serumLibrary(const std::string& kind);
// The item after/before the one whose path or name matches, wrapping; -1 when nothing matches.
int serumLibraryStep(const std::vector<SerumLibraryItem>& items, const std::string& pathOrName, int direction);

// Banks and groups (UI-28i). The first folder below the library kind is the bank ("Factory", "User", "Splice"), the
// second the instrument group ("Bass", "Pad"); deeper folders ("Bass/Reese") stay inside their group. Files directly in
// a bank form the unnamed group "", files directly in the kind folder the unnamed bank "". serumLibrary() sorts folder by
// folder, so every bank and every group is one contiguous run of the list.
std::string serumLibraryBank(const SerumLibraryItem&);
std::string serumLibraryGroup(const SerumLibraryItem&);
// Index of the item with this path, else the first with this display name; -1 when none.
int serumLibraryFind(const std::vector<SerumLibraryItem>& items, const std::string& pathOrName);
// Banks that hold items, in list order.
std::vector<std::string> serumLibraryBanks(const std::vector<SerumLibraryItem>& items);
// Bank folders on disk, including empty ones, across the roots (natural order, no duplicates).
std::vector<std::string> serumLibraryBankFolders(const std::string& kind);
// The bank's groups in list order.
std::vector<std::string> serumLibraryGroups(const std::vector<SerumLibraryItem>& items, const std::string& bank);
// "<" / ">": the item after/before the matching one inside the same bank, wrapping from the bank's last item to its
// first (and back), never into another bank; -1 when nothing matches.
int serumLibraryStepInBank(const std::vector<SerumLibraryItem>& items, const std::string& pathOrName, int direction);
// "<<" / ">>": the first item of the next/previous group of the same bank, wrapping; -1 when nothing matches or the
// bank has fewer than two groups (the buttons are disabled then).
int serumLibraryStepGroup(const std::vector<SerumLibraryItem>& items, const std::string& pathOrName, int direction);
// The browser's list: indices of the bank's items, limited to one group when `group` is set, whose name or folder below
// the bank contains every whitespace-separated word of `query` (case-insensitive). List order.
std::vector<int> serumLibraryFilter(const std::vector<SerumLibraryItem>& items, const std::string& bank,
                                    const std::optional<std::string>& group, const std::string& query);
} // namespace sonora::plugins

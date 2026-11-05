#pragma once
// LIB-1: the installed sample packs on disk (docs/SAMPLE_PACKS.md). No network, no threads: PackStore calls it on its worker,
// tests and tools/pack_tool call it directly.
//
//   <root>/<pack name>/pack.json, LICENSE.txt, sample folders     one folder per installed pack (the Browser lists these)
//   <root>/.sonora/downloads/<id>/<version>-<sha>.zip.part       partial downloads, kept so the next try resumes
//   <root>/.sonora/staging/                                     archives are unpacked here, then moved into place
//   <root>/.sonora/catalogue/                                   the last verified packs.json + packs.json.sig

#include "packs/PackManifest.h"

#include <atomic>

namespace sonora::packs
{

struct InstalledPack
{
    PackContents contents;
    juce::File folder;
};

// SHA-256 (lowercase hex) of a file; empty when it cannot be read or `cancelled` became true.
std::string sha256OfFile(const juce::File& file, const std::atomic<bool>& cancelled);

class PackLibrary
{
public:
    explicit PackLibrary(juce::File root) : root_(std::move(root)) {}
    const juce::File& root() const { return root_; }
    juce::File privateFolder() const { return root_.getChildFile(".sonora"); }
    juce::File partialDownload(const PackEntry& entry) const;
    // Partial downloads of `id` other than `keep` (an older version or archive): the caller deletes them.
    juce::Array<juce::File> stalePartials(const std::string& id, const juce::File& keep) const;

    std::vector<InstalledPack> installed() const; // every child folder with a readable pack.json, sorted by name
    std::optional<InstalledPack> find(const std::string& id) const;

    struct Result
    {
        bool ok = false;
        std::string error;
        juce::File folder;
    };
    // Unpacks a verified archive. It must hold pack.json (same id, version and an allowed licence as `expected`) and
    // LICENSE.txt at its top level, and only safe relative paths (no links, no "..", at most 20000 entries and 8 GiB, and
    // no more than twice the catalogue's installedBytes). An older install of the same id is replaced.
    Result install(const juce::File& archive, const PackEntry& expected, const std::atomic<bool>& cancelled) const;
    // Removes the installed folder of `id`: to the Trash when `toTrash` and the system has one, else deleted.
    bool remove(const std::string& id, std::string& error, bool toTrash = true) const;

private:
    juce::File root_;
};

} // namespace sonora::packs

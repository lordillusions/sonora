#pragma once
// SPL-1: the Splice data Sonora shows and uses (docs/SPLICE_INTEROP.md). No JUCE in the struct definitions.

#include <cstdint>
#include <string>
#include <vector>

namespace sonora::platform::splice
{

struct SpliceSample
{
    std::string uuid;          // asset uuid (for cost/purchase and RefreshSamplePreview)
    std::string name;          // last path component is the file name; full name is the pack-relative path
    std::string fileHash;      // the licensed WAV's content hash (empty until licensed); the download id
    std::string previewUrl;    // scrambled preview mp3 (presigned S3)
    std::string packName;
    std::string packUuid;
    std::string coverUrl;
    std::string providerName;
    std::string key;           // root note as Splice reports it, e.g. "a" (may be empty)
    std::string chordType;     // "minor" / "major" / ""
    std::string category;      // "loop" / "oneshot"
    std::vector<std::string> tags;
    double bpm = 0;
    int durationMs = 0;
    int priceCredits = 0;
    bool licensed = false;     // already owned: no purchase needed
    bool isLoop = false;
};

struct SpliceSearchResult
{
    std::vector<SpliceSample> samples;
    int page = 1;
    int totalPages = 0;
    int totalRecords = 0;
    bool ok = false;
    std::string error;
};

struct SpliceSearchQuery
{
    std::string text;
    std::string key;        // filter, e.g. "a" (optional)
    std::string chordType;  // "minor"/"major" (optional)
    std::string category;   // "loop"/"oneshot" (optional)
    int minBpm = 0;
    int maxBpm = 0;
    int page = 1;
    int limit = 50;
    float semantic = 0;     // SPL-9: 1 = "describe the sound" semantic search (needs a logged-in, entitled account)
};

struct SpliceCost
{
    int cost = 0;
    int userCredits = 0;
    bool ok = false;
};

struct SpliceUser
{
    std::string username;
    std::string email;
    int credits = 0;
    bool ok = false;
};

} // namespace sonora::platform::splice

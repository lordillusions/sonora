#pragma once
// LIB-5: replacing the sample file of audio clips (Clip > Replace Sample..., a browser sample dropped on the clip's sample
// editor, the similar-sample swap arrows). One undoable step that can cover one clip, every clip on its track that uses the
// same file (every kick clip of a kick track) or every clip in the set that uses it.
//
// Follows the reference (Live 12 manual, "Replacing and Editing the Sample"): the clip keeps its settings (gain, pitch,
// warp mode, loop and start/end, fades, envelopes); warp markers are kept only when the new file has the same length,
// otherwise they are reset to two anchors that keep the clip's tempo. A clip still named after its old file takes the new
// file's name. Frozen tracks are left alone (their clips cannot be edited until unfrozen).

#include "core/Edit.h"

#include <string>
#include <vector>

namespace sonora::model
{

enum class SampleScope
{
    Clip,  // only the given clip
    Track, // every audio clip on the clip's track that uses the same file (Arrangement, Session slots, take lanes)
    Set    // every audio clip in the set that uses the same file
};

const char* sampleScopeLabel(SampleScope); // "This Clip", "All Clips on This Track", "All Clips in the Set"

struct SampleFile
{
    FileRef ref;            // absolutePath set; relativePath/type when it lies in the project folder
    double sampleRate = 0;  // of the new file
    int64_t frames = 0;
};

// Two references name the same file (same absolute path, or the same project-relative path).
bool sameSampleFile(const FileRef& a, const FileRef& b);

// The audio clips the scope covers, starting with `location` itself. Empty when `location` is not an audio clip.
std::vector<ClipLocation> clipsUsingSample(const Song&, ClipLocation location, SampleScope);

// The clip after its sample was replaced (pure; used by replaceClipSample and tests).
Clip withReplacedSample(const Clip& clip, const SampleFile& replacement);

// One undo step replacing the sample of every clip `clipsUsingSample` lists. No apply when nothing can be replaced
// (not an audio clip, the file is the one already used, or the replacement has no frames). `replaced` receives the count.
EditCommand replaceClipSample(const Song&, ClipLocation location, const SampleFile& replacement, SampleScope, int* replaced = nullptr);

} // namespace sonora::model

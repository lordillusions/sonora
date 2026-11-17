#pragma once
// UI-93: multi-clip note editing in the Arrangement (the reference manual, "Multi-Clip Editing in the Arrangement View").
// When a time selection covers MIDI clips on several tracks (or several clips on one track), the note editor shows all
// their notes together in Arrangement time. These pure functions find the clips, merge their notes into one list the
// editor works on, and split an edited list back into one undoable command over the clips that changed.
// Unit tested in tests/test_multi_clip_notes.cpp.

#include "core/Edit.h"
#include "core/Model.h"

#include <string>
#include <vector>

namespace sonora::model
{

struct MultiClipMember
{
    ClipLocation location;
    double offset = 0.0;            // Arrangement beat = content beat + offset (the start marker sits at the clip's left edge)
    double start = 0.0, end = 0.0;  // the clip's span in the Arrangement
    int colorIndex = 0;
};

// The reference shows clips from up to eight tracks.
constexpr int kMaxMultiClipTracks = 8;

// MIDI clips of the main Arrangement lanes of tracks first..last that overlap [start, end), ordered by track, then time.
// Only the first kMaxMultiClipTracks tracks that hold such a clip count. Take lanes are not included.
std::vector<MultiClipMember> midiClipsInSelection(const Song&, int firstTrack, int lastTrack, double start, double end);

// The member clips' notes in one list, in Arrangement beats. A merged note's `key` is its 1-based index into `origin`,
// which remembers the clip and the note's own key; notes added while editing have key 0.
struct MergedNotes
{
    struct Origin
    {
        int member = 0;
        uint32_t key = 0;
        double contentStart = 0.0; // the stored start, restored exactly when the note did not move
    };
    std::vector<MidiNote> notes;
    std::vector<Origin> origin;
};
MergedNotes mergeNotes(const Song&, const std::vector<MultiClipMember>&);

// The member a merged note belongs to: its origin's, or for a new note the clip under its start (the focused clip first,
// then clips on the focused clip's track, then any), else the nearest clip on the focused track, else `focus`.
int memberOfNote(const MergedNotes&, const std::vector<MultiClipMember>&, const MidiNote&, int focus);

// Splits `edited` (merged notes after a gesture) back into the member clips. One command over the clips whose notes
// changed; its `apply` is empty when nothing changed.
EditCommand setMergedNotes(const Song&, const std::vector<MultiClipMember>&, const MergedNotes& base,
                           const std::vector<MidiNote>& edited, int focus, std::string label);

} // namespace sonora::model

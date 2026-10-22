#pragma once
// Session-view clip launching (ROADMAP ENG-6): message-thread API that queues launches and stops
// in TrackControls. The audio thread applies them at the next quantisation boundary
// (AudioEngine::renderTrackClips).
//
// Behaviour follows Live: launching starts the transport when it is stopped; an empty slot stops the
// track; launching a scene launches every slot of that row (and stops tracks with an empty slot);
// a launched session clip takes the track over from the arrangement until "Back to Arrangement".
// Per-clip launch quantisation (also for scenes), all four launch modes and legato are supported.
// TODO(ENG-6b): follow actions.

#include "core/Model.h"
#include "engine/EngineGraph.h"
#include "engine/Transport.h"

namespace sonora::engine
{

// Length in beats of Live's <GlobalQuantisation> setting (see Model.h); 0 == immediately.
double quantisationBeats(int globalQuantisation, int timeSigNumerator, int timeSigDenominator);

// Quantisation of one clip: its own setting (model::Clip::launchQuantisation, 0 == Global) or the set's.
double clipQuantisationBeats(const model::Clip& clip, int globalQuantisation, int timeSigNumerator, int timeSigDenominator);

// Launch with the clip's mode: Toggle stops a playing/queued clip; Repeat retriggers while held.
void launchClipWithMode(EngineGraph& graph, Transport& transport, const model::Clip& clip, int track, int scene, double quantBeats,
                        float velocity = 1.0f);
float clipLaunchGain(const model::Clip& clip, float velocity);
// Mouse/key released: Gate stops at the next boundary; Repeat finishes retriggering and keeps playing.
void releaseClip(EngineGraph& graph, Transport& transport, const model::Clip& clip, int track, int scene, double quantBeats);

void launchClip(EngineGraph& graph, Transport& transport, int track, int scene, double quantBeats);
void stopTrack(EngineGraph& graph, Transport& transport, int track, double quantBeats);
void launchScene(EngineGraph& graph, Transport& transport, const model::Song& song, int scene, double quantBeats);
void stopAllClips(EngineGraph& graph, Transport& transport, double quantBeats);
// Hands every track back to the arrangement and stops the session clips immediately.
void backToArrangement(EngineGraph& graph);
// True while at least one track is overridden by session playback (the orange button in Live).
bool arrangementOverridden(const EngineGraph& graph);

// UI helpers: what is the slot of `track` doing right now?
enum class SlotState { Empty, Stopped, Queued, Playing, QueuedStop };
SlotState slotState(const EngineGraph& graph, int track, int scene);

} // namespace sonora::engine

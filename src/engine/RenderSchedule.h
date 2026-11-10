#pragma once
// ENG-14e: the order in which one block of an EngineGraph may be rendered by several threads.
//
// Built on the message thread (EngineGraph::prepare); the audio thread only resets the counters and walks the lists.
// One job per track (graph.trackOrder) and per return (graph.returnNodes); Main is rendered by the callback thread once
// every job is done. A job starts with a gather: it adds its sources into its own buffer in the serial order (a group its
// children, a return every track's send), so every bus is summed in the same order as a serial render and the result is
// bit-identical whatever the thread count. Edges:
//   - a child before its group (its gather reads the child's buffer),
//   - every track before every return (any track may send to any return at run time),
//   - two nodes linked by a sidechain in the serial order (the listener reads the source's tap; a source that is rendered
//     later in the serial order must not overwrite the tap before the listener read the previous block's audio).
// Every edge points forward in the serial order, so the job graph has no cycle.

#include <atomic>
#include <memory>
#include <vector>

namespace sonora::engine
{

struct EngineGraph;

struct RenderSchedule
{
    struct Job
    {
        int node = -1;
        bool isReturn = false;
        int returnIndex = -1;        // a return's index in graph.returnNodes (its send slot)
        int dependencies = 0;        // jobs that must finish before this one starts
        std::vector<int> successors; // jobs released when this one finishes
        std::vector<int> sources;    // nodes gathered into this node's buffer first, in serial order
        int lateParent = -1;         // only in a graph that is not parallelSafe: the group this track adds itself to
    };
    std::vector<Job> jobs;        // serial order: trackOrder, then returnNodes
    std::vector<int> mainSources; // top-level tracks in trackOrder; Main gathers them, then the returns
    int width = 1;                // most jobs that can run at the same time (widest dependency level)
    bool parallelSafe = true;     // false: a routing loop put a child after its group; such a graph renders serially

    // Run-time state, audio thread only (EngineWorkers). Allocated with the jobs.
    std::unique_ptr<std::atomic<int>[]> pending; // dependencies left per job
    std::unique_ptr<std::atomic<int>[]> queue;   // ready jobs in the order they became ready (-1: slot not published yet)
};

// Message thread. The graph's trackOrder, returnNodes, outputNode and sidechains must be final.
RenderSchedule buildRenderSchedule(const EngineGraph& graph);

} // namespace sonora::engine

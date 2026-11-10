#include "engine/RenderSchedule.h"
#include "engine/EngineGraph.h"

#include <algorithm>

namespace sonora::engine
{

RenderSchedule buildRenderSchedule(const EngineGraph& graph)
{
    RenderSchedule s;
    const int numNodes = (int) graph.nodes.size();
    std::vector<int> jobOfNode((size_t) std::max(0, numNodes), -1);
    auto addJob = [&](int node, bool isReturn) {
        if (node < 0 || node >= numNodes || jobOfNode[(size_t) node] >= 0) return;
        jobOfNode[(size_t) node] = (int) s.jobs.size();
        RenderSchedule::Job job;
        job.node = node;
        job.isReturn = isReturn;
        s.jobs.push_back(std::move(job));
    };
    for (int node : graph.trackOrder) addJob(node, false);
    const int numTrackJobs = (int) s.jobs.size();
    for (int node : graph.returnNodes) addJob(node, true);
    for (int r = 0; r < (int) graph.returnNodes.size(); ++r)
        if (const int node = graph.returnNodes[(size_t) r]; node >= 0 && node < numNodes && jobOfNode[(size_t) node] >= 0
            && s.jobs[(size_t) jobOfNode[(size_t) node]].returnIndex < 0)
            s.jobs[(size_t) jobOfNode[(size_t) node]].returnIndex = r;
    const int numJobs = (int) s.jobs.size();

    std::vector<std::vector<char>> edge((size_t) numJobs, std::vector<char>((size_t) numJobs, 0));
    auto link = [&](int before, int after) {
        if (before < 0 || after < 0 || before == after) return;
        if (before > after) std::swap(before, after); // serial order decides (see the header)
        edge[(size_t) before][(size_t) after] = 1;
    };

    for (int j = 0; j < numTrackJobs; ++j)
    {
        const int node = s.jobs[(size_t) j].node;
        const int parent = graph.nodes[(size_t) node].outputNode;
        const int parentJob = parent >= 0 && parent < numNodes ? jobOfNode[(size_t) parent] : -1;
        if (parent < 0 || parentJob < 0 || parentJob >= numTrackJobs)
            s.mainSources.push_back(node); // Main (or an unknown parent, which the serial render also treats as Main)
        else if (parentJob < j)
        {
            s.parallelSafe = false; // a loop: the serial render adds this child after its group already ran
            s.jobs[(size_t) j].lateParent = parent;
        }
        else
        {
            s.jobs[(size_t) parentJob].sources.push_back(node);
            link(j, parentJob);
        }
    }
    for (int r = numTrackJobs; r < numJobs; ++r)
        for (int j = 0; j < numTrackJobs; ++j)
        {
            s.jobs[(size_t) r].sources.push_back(s.jobs[(size_t) j].node);
            link(j, r);
        }
    for (int j = 0; j < numJobs; ++j)
        for (const auto& sc : graph.nodes[(size_t) s.jobs[(size_t) j].node].sidechains)
            if (sc.sourceNode >= 0 && sc.sourceNode < numNodes)
                link(jobOfNode[(size_t) sc.sourceNode], j);

    std::vector<int> level((size_t) numJobs, 0);
    for (int a = 0; a < numJobs; ++a)
        for (int b = a + 1; b < numJobs; ++b)
            if (edge[(size_t) a][(size_t) b])
            {
                s.jobs[(size_t) a].successors.push_back(b);
                ++s.jobs[(size_t) b].dependencies;
                level[(size_t) b] = std::max(level[(size_t) b], level[(size_t) a] + 1); // a < b: a's level is final
            }
    std::vector<int> perLevel((size_t) numJobs + 1, 0);
    for (int j = 0; j < numJobs; ++j) s.width = std::max(s.width, ++perLevel[(size_t) level[(size_t) j]]);

    s.pending = std::make_unique<std::atomic<int>[]>((size_t) std::max(1, numJobs));
    s.queue = std::make_unique<std::atomic<int>[]>((size_t) std::max(1, numJobs));
    return s;
}

} // namespace sonora::engine

// engine_probe — headless engine check: load a set, time the build, render audio offline.
//
//   engine_probe <set.als> [--export file.wav|--stems folder] [--to <beat>] [--tail <s>] [--normalize] [--render <seconds>] [--from <beat>] [--out file.wav] [--no-plugins] [--check-finite] [--stress] [--bypass NAME]... [--launch TRACK:SCENE]... [--profile] [--block N]
//
// Prints: model load time, graph build time (UI-blocking part), background sample load time,
// failed samples, build log. With --render it plays the arrangement from --from (default 0)
// through the real audio callback code (AudioEngine::renderBlock) without a sound card and
// prints peak / RMS per second, optionally writing a 24-bit stereo WAV.
// --profile times every callback (worst block per second against the realtime budget of
// --block samples, default 256) and every top-level device, to find what overloads playback.

#include "devices/DeviceFactory.h"
#include "devices/RackProcessor.h"
#include "engine/AudioEngine.h"
#include "engine/ClipLauncher.h"
#include "engine/Exporter.h"
#include "io/als/AlsReader.h"
#include "midi/MidiManager.h"
#include "plugins/PluginHost.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_events/juce_events.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <functional>
#include <map>

using namespace sonora;

namespace
{
// --profile: wraps a top-level device and accumulates the time its process() takes.
class TimedDevice final : public devices::DeviceProcessor
{
public:
    TimedDevice(std::unique_ptr<devices::DeviceProcessor> d, std::string where) : inner(std::move(d)), track(std::move(where)) {}

    std::string name() const override { return inner->name(); }
    bool isInstrument() const override { return inner->isInstrument(); }
    bool isHostedPlugin() const override { return inner->isHostedPlugin(); }
    void prepare(double sr, int block) override { inner->prepare(sr, block); }
    void process(juce::AudioBuffer<float>& audio, juce::MidiBuffer& midi) override
    {
        const auto t = std::chrono::steady_clock::now();
        inner->process(audio, midi);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
        totalMs += ms;
        worstMs = std::max(worstMs, ms);
        blockMs += ms;
    }
    void reset() override { inner->reset(); }
    void setHostInfo(const devices::HostInfo& info) override { inner->setHostInfo(info); }
    void setSidechain(const juce::AudioBuffer<float>* audio, float gain) override { inner->setSidechain(audio, gain); }
    int latencySamples() const override { return inner->latencySamples(); }
    const std::vector<devices::ParameterInfo>& parameters() const override { return inner->parameters(); }
    int findParameterByHostId(int id, const std::string& n) const override { return inner->findParameterByHostId(id, n); }
    float getParameter(int index) const override { return inner->getParameter(index); }
    void setParameter(int index, float value) override { inner->setParameter(index, value); }

    std::unique_ptr<devices::DeviceProcessor> inner;
    std::string track;
    double totalMs = 0.0, worstMs = 0.0, blockMs = 0.0; // blockMs: this callback only (reset by the caller)
};
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: engine_probe <set.als> [--render <seconds>] [--from <beat>] [--out f.wav] [--no-plugins] [--check-finite] [--stress] [--bypass NAME]... [--launch TRACK:SCENE]... [--profile] [--block N]\n");
        return 2;
    }
    juce::ScopedJuceInitialiser_GUI juceInit;

    double renderSeconds = 0.0, fromBeat = 0.0;
    juce::File outFile;
    bool plugins = true, checkFinite = false, stress = false, exportNormalize = false, profile = false;
    int blockSize = 256;
    std::string exportFile, stemsFolder;
    double exportTo = 0.0, exportTail = 0.0;
    std::vector<std::string> launches;
    std::vector<std::string> bypassNames; // devices to bypass by name (bisecting problems)
    for (int i = 2; i < argc; ++i)
    {
        const juce::String a = argv[i];
        if (a == "--render" && i + 1 < argc)
            renderSeconds = std::atof(argv[++i]);
        else if (a == "--from" && i + 1 < argc)
            fromBeat = std::atof(argv[++i]);
        else if (a == "--out" && i + 1 < argc)
            outFile = juce::File::getCurrentWorkingDirectory().getChildFile(argv[++i]);
        else if (a == "--no-plugins")
            plugins = false;
        else if (a == "--check-finite")
            checkFinite = true;
        else if (a == "--stress")
            stress = true;
        else if (a == "--launch" && i + 1 < argc)
            launches.push_back(argv[++i]); // "track:scene", launched (immediately) before rendering
        else if (a == "--bypass" && i + 1 < argc)
            bypassNames.push_back(argv[++i]);
        else if (a == "--export" && i + 1 < argc)
            exportFile = argv[++i];
        else if (a == "--stems" && i + 1 < argc)
            stemsFolder = argv[++i];
        else if (a == "--to" && i + 1 < argc)
            exportTo = std::atof(argv[++i]);
        else if (a == "--tail" && i + 1 < argc)
            exportTail = std::atof(argv[++i]);
        else if (a == "--normalize")
            exportNormalize = true;
        else if (a == "--profile")
            profile = true;
        else if (a == "--block" && i + 1 < argc)
            blockSize = std::clamp(std::atoi(argv[++i]), 16, 4096);
    }

    auto t0 = juce::Time::getMillisecondCounterHiRes();
    auto lap = [&] {
        const auto now = juce::Time::getMillisecondCounterHiRes();
        const auto d = now - t0;
        t0 = now;
        return d;
    };

    auto r = io::loadAls(juce::File::getCurrentWorkingDirectory().getChildFile(juce::String::fromUTF8(argv[1])));
    if (! r)
    {
        std::fprintf(stderr, "ERROR: %s\n", r.error.c_str());
        return 1;
    }
    std::printf("model load      %7.0f ms\n", lap());

    if (stress)
    {
        // Feed every native device of the set (racks included) with noise and a sine and report
        // anything that goes non-finite or explodes.
        int checked = 0, bad = 0;
        std::function<void(const model::Device&, const std::string&)> visit = [&](const model::Device& d, const std::string& where) {
            for (const auto& chain : d.chains)
                for (const auto& child : chain.devices)
                    visit(child, where + " > " + d.displayName);
            if (d.kind != model::DeviceKind::Native)
                return;
            auto dev = devices::createNative(d);
            if (dev == nullptr)
                return;
            ++checked;
            if (d.type == "Eq8") // repeat creation: flaky initialisation bugs only show up occasionally
                for (int rep = 0; rep < 300; ++rep)
                {
                    auto again = devices::createNative(d);
                    again->prepare(48000.0, 512);
                    juce::AudioBuffer<float> z(2, 256);
                    for (int blk = 0; blk < 3; ++blk)
                    {
                        z.clear();
                        juce::MidiBuffer mm;
                        again->process(z, mm);
                        const float m = z.getMagnitude(0, 256);
                        if (! (m < 1e-6f))
                        {
                            std::printf("!! EQ Eight '%s' made %g from silence on creation #%d block %d\n", where.c_str(), (double) m, rep, blk);
                            rep = 1000;
                            break;
                        }
                    }
                }
            dev->prepare(48000.0, 512);
            juce::Random rnd(1);
            juce::MidiBuffer midi;
            if (dev->isInstrument())
                midi.addEvent(juce::MidiMessage::noteOn(1, 60, (juce::uint8) 100), 0);
            float worst = 0.0f;
            bool nonFinite = false;
            for (int blk = 0; blk < 400 && ! nonFinite; ++blk)
            {
                juce::AudioBuffer<float> b(2, 512);
                for (int i = 0; i < 512; ++i)
                {
                    const float noise = (rnd.nextFloat() - 0.5f) * 0.5f;
                    const float sine = 0.4f * std::sin(0.05f * (float) (blk * 512 + i));
                    b.setSample(0, i, blk % 2 ? noise : sine);
                    b.setSample(1, i, blk % 2 ? noise * 0.8f : sine);
                }
                dev->process(b, midi);
                midi.clear();
                const float m = std::max(b.getMagnitude(0, 0, 512), b.getMagnitude(1, 0, 512));
                if (! std::isfinite(m))
                    nonFinite = true;
                worst = std::max(worst, m);
            }
            if (nonFinite || worst > 30.0f)
            {
                ++bad;
                std::printf("!! %s / %s (%s): %s (max %g)\n", where.c_str(), d.displayName.c_str(), d.type.c_str(),
                            nonFinite ? "NON-FINITE" : "loud", (double) worst);
                for (const auto& p : d.parameters)
                    if (p.path.find("Bands") == std::string::npos || p.path.find("ParameterA") != std::string::npos)
                        std::printf("      %s = %g\n", p.path.c_str(), p.value);
            }
        };
        auto doTrack = [&](const model::Track& t) {
            for (const auto& d : t.devices)
                visit(d, t.name);
        };
        for (const auto& t : r.song->tracks)
            doTrack(t);
        for (const auto& t : r.song->returns)
            doTrack(t);
        doTrack(r.song->main);
        std::printf("stress: %d native devices checked, %d problem(s)\n", checked, bad);
        return bad == 0 ? 0 : 1;
    }

    plugins::PluginHost host;
    if (plugins)
        host.loadCache();
    midi::MidiManager midi;
    engine::AudioEngine engine(host, midi);

    const auto log = engine.loadSong(*r.song);
    std::printf("graph build     %7.0f ms  (blocks the UI)\n", lap());
    engine.sampleCache().waitForLoads(10 * 60 * 1000);
    std::printf("samples decoded %7.0f ms  (background)\n", lap());
    for (const auto& f : engine.failedSamples())
        std::printf("  FAILED sample: %s\n", f.c_str());
    for (const auto& l : log)
        std::printf("  log: %s\n", l.c_str());

    if (! bypassNames.empty())
    {
        std::function<void(devices::DeviceProcessor&)> apply = [&](devices::DeviceProcessor& d) {
            for (const auto& n : bypassNames)
                if (d.name() == n)
                    d.setBypassed(true);
            if (auto* rack = dynamic_cast<devices::RackProcessor*>(&d))
                for (auto& chain : rack->chains())
                    for (auto& child : chain.devices)
                        apply(*child);
        };
        for (auto& node : engine.latestGraph()->nodes)
            for (auto& d : node.devices)
                apply(*d);
    }
    {
        // Which processors did the graph actually build? (devices inside racks included)
        std::map<std::string, int> counts;
        std::function<void(const devices::DeviceProcessor&)> visit = [&](const devices::DeviceProcessor& d) {
            ++counts[d.name()];
            if (const auto* rack = dynamic_cast<const devices::RackProcessor*>(&d))
                for (const auto& chain : rack->chains())
                    for (const auto& child : chain.devices)
                        visit(*child);
        };
        for (const auto& node : engine.latestGraph()->nodes)
            for (const auto& d : node.devices)
                visit(*d);
        std::printf("devices built:");
        for (const auto& [name, n] : counts)
            std::printf("  %s x%d", name.c_str(), n);
        std::printf("\n");
    }

    if (checkFinite)
    {
        engine.debugCheckDevices = true;
        engine.onBadAudio = [](const std::string& track, const std::string& device, float inMag, float outMag) {
            std::printf("!! bad audio: track '%s' device '%s' (input magnitude %g -> output %g)\n", track.c_str(), device.c_str(),
                        (double) inMag, (double) outMag);
        };
    }
    if (checkFinite)
    {
        // Each device on its own with silence: anything that makes noise from nothing is broken.
        for (auto& node : engine.latestGraph()->nodes)
            for (auto& dev : node.devices)
            {
                float worst = 0.0f;
                for (int k = 0; k < 4; ++k)
                {
                    juce::AudioBuffer<float> b(2, 256);
                    b.clear();
                    juce::MidiBuffer m;
                    dev->process(b, m);
                    worst = std::max(worst, b.getMagnitude(0, 256));
                }
                if (! (worst < 1e-6f))
                    std::printf("!! '%s' / %s makes %g from silence\n", node.name.c_str(), dev->name().c_str(), (double) worst);
            }
    }

    if (! exportFile.empty() || ! stemsFolder.empty())
    {
        engine::ExportOptions options;
        options.target = stemsFolder.empty() ? engine::ExportTarget::Main : engine::ExportTarget::AllTracks;
        options.startBeat = fromBeat;
        options.endBeat = exportTo > 0.0 ? exportTo : engine::Exporter::arrangementEndBeat(*r.song);
        options.tailSeconds = exportTail;
        options.normalize = exportNormalize;
        options.destination = juce::File::getCurrentWorkingDirectory().getChildFile(juce::String(stemsFolder.empty() ? exportFile : stemsFolder));
        options.baseName = juce::File(juce::String(argv[1])).getFileNameWithoutExtension().toStdString();
        engine::Exporter exporter(engine, *r.song);
        std::atomic<float> progress{0.0f};
        std::atomic<bool> cancel{false};
        engine.beginOffline();
        const auto result = exporter.run(options, progress, cancel);
        engine.endOffline();
        if (! result.ok)
            std::printf("export failed: %s\n", result.error.c_str());
        else
        {
            std::printf("exported %zu file(s), %.1f s each, in %.2f s (%.1fx realtime)\n", result.files.size(), result.seconds,
                        result.renderSeconds, result.seconds / std::max(0.001, result.renderSeconds));
            std::printf("  %s: %.1f LUFS integrated, LRA %.1f LU, true peak %.1f dBTP, sample peak %.1f dBFS\n",
                        result.files.front().name.c_str(), (double) result.integratedLufs, (double) result.loudnessRange,
                        (double) result.truePeakDb, (double) result.samplePeakDb);
            for (const auto& f : result.files)
                std::printf("  -> %s\n", f.file.getFullPathName().toRawUTF8());
        }
    }

    if (renderSeconds > 0.0)
    {
        const double sr = engine.sampleRate();
        const int block = blockSize;
        std::vector<TimedDevice*> timed;
        if (profile)
            for (auto& node : engine.latestGraph()->nodes)
                for (auto& d : node.devices)
                {
                    auto wrapper = std::make_unique<TimedDevice>(std::move(d), node.name);
                    timed.push_back(wrapper.get());
                    d = std::move(wrapper);
                }
        const double budgetMs = 1000.0 * block / sr;
        double secWorstMs = 0.0, secSumMs = 0.0, secWorstBeat = 0.0;
        long long totalOver = 0;
        int secOver = 0, secBlocks = 0;
        std::string secWorstDevice;
        const auto totalBlocks = (juce::int64) (renderSeconds * sr / block);
        engine.transport().setPosition(fromBeat);
        engine.transport().play();
        for (const auto& l : launches)
        {
            int t = 0, sc = 0;
            if (std::sscanf(l.c_str(), "%d:%d", &t, &sc) == 2)
                engine::launchClip(*engine.latestGraph(), engine.transport(), t, sc, 0.0);
        }

        std::unique_ptr<juce::AudioFormatWriter> writer;
        if (outFile != juce::File())
        {
            outFile.deleteFile();
            std::unique_ptr<juce::OutputStream> stream(outFile.createOutputStream());
            if (stream != nullptr)
                writer = juce::WavAudioFormat().createWriterFor(
                    stream, juce::AudioFormatWriterOptions{}.withSampleRate(sr).withNumChannels(2).withBitsPerSample(24));
        }

        std::vector<float> l((size_t) block), rr((size_t) block);
        float* out[2] = {l.data(), rr.data()};
        bool reported = false;
        double sumSq = 0.0;
        float peak = 0.0f;
        juce::int64 samplesInSecond = 0;
        int second = 0;
        const auto renderStart = juce::Time::getMillisecondCounterHiRes();
        for (juce::int64 b = 0; b < totalBlocks; ++b)
        {
            const double beatBefore = engine.transport().positionBeats();
            for (auto* t : timed) t->blockMs = 0.0;
            const auto blockStart = std::chrono::steady_clock::now();
            engine.renderBlock(out, 2, block);
            if (profile)
            {
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - blockStart).count();
                secSumMs += ms;
                ++secBlocks;
                if (ms > budgetMs)
                {
                    ++secOver, ++totalOver;
                    std::printf("  !! block at beat %.3f took %.2f ms:", beatBefore, ms);
                    double inDevices = 0.0;
                    for (auto* t : timed)
                    {
                        inDevices += t->blockMs;
                        if (t->blockMs >= 0.25)
                            std::printf("  %s/%s %.2f", t->track.c_str(), t->name().c_str(), t->blockMs);
                    }
                    std::printf("  (devices %.2f ms)\n", inDevices);
                }
                if (ms > secWorstMs)
                {
                    secWorstMs = ms;
                    secWorstBeat = beatBefore;
                    const TimedDevice* top = nullptr;
                    for (auto* t : timed) if (top == nullptr || t->blockMs > top->blockMs) top = t;
                    secWorstDevice = top != nullptr ? top->track + " / " + top->name() + " " + juce::String(top->blockMs, 2).toStdString() + " ms" : "";
                }
            }
            if (checkFinite && ! reported)
                for (auto& node : engine.latestGraph()->nodes)
                {
                    const float mag = node.buffer.getMagnitude(0, 0, block);
                    if (! std::isfinite(mag) || mag > 50.0f)
                    {
                        std::printf("!! node '%s' went to %g in block %lld (t=%.3f s)\n", node.name.c_str(), (double) mag, b,
                                    (double) b * block / sr);
                        for (size_t di = 0; di < node.devices.size(); ++di)
                            std::printf("     device %zu: %s\n", di, node.devices[di]->name().c_str());
                        reported = true;
                    }
                }
            for (int i = 0; i < block; ++i)
            {
                const float a = std::fabs(l[(size_t) i]), c = std::fabs(rr[(size_t) i]);
                peak = std::max({peak, a, c});
                sumSq += (double) l[(size_t) i] * l[(size_t) i] + (double) rr[(size_t) i] * rr[(size_t) i];
            }
            if (writer != nullptr)
                writer->writeFromFloatArrays(out, 2, block);
            samplesInSecond += block;
            if (samplesInSecond >= (juce::int64) sr)
            {
                std::printf("t=%3ds beat=%8.2f  peak %6.1f dBFS  rms %6.1f dBFS\n", ++second, engine.transport().positionBeats(),
                            20.0 * std::log10(std::max(1e-9f, peak)),
                            10.0 * std::log10(std::max(1e-18, sumSq / (2.0 * (double) samplesInSecond))));
                if (profile)
                {
                    std::printf("        cpu avg %5.1f %%  worst %6.2f ms (%5.0f %% of %.2f ms) at beat %7.2f  over budget %d/%d  [%s]\n",
                                100.0 * secSumMs / (secBlocks * budgetMs), secWorstMs, 100.0 * secWorstMs / budgetMs, budgetMs,
                                secWorstBeat, secOver, secBlocks, secWorstDevice.c_str());
                    secWorstMs = secSumMs = 0.0;
                    secOver = secBlocks = 0;
                }
                peak = 0.0f;
                sumSq = 0.0;
                samplesInSecond = 0;
            }
        }
        const auto wall = (juce::Time::getMillisecondCounterHiRes() - renderStart) / 1000.0;
        std::printf("rendered %.1f s of audio in %.2f s (%.1fx realtime)\n", renderSeconds, wall, renderSeconds / wall);
        if (profile)
        {
            std::printf("blocks over the %.2f ms budget: %lld of %lld\n", budgetMs, totalOver, (long long) totalBlocks);
            std::sort(timed.begin(), timed.end(), [](auto* x, auto* y) { return x->totalMs > y->totalMs; });
            double all = 0.0;
            for (auto* t : timed) all += t->totalMs;
            std::printf("device time (%.0f ms total, %.1f %% of the realtime budget):\n", all, 100.0 * all / (renderSeconds * 1000.0));
            for (auto* t : timed)
                if (t->totalMs >= 0.005 * all)
                    std::printf("  %6.1f %%  %8.1f ms  worst %6.2f ms  %-28s %s\n", 100.0 * t->totalMs / (renderSeconds * 1000.0), t->totalMs,
                                t->worstMs, t->track.c_str(), t->name().c_str());
        }
    }
    return 0;
}

#include "engine/Exporter.h"

#include "core/Edit.h"
#include "core/TimeUtils.h"
#include "dsp/Loudness.h"

#include <algorithm>
#include <cmath>
#include <random>

namespace sonora::engine
{

namespace
{
struct Stream
{
    std::string name;
    int node = -1; // -1 == the Main mix
    juce::File scratch;
    std::unique_ptr<juce::AudioFormatWriter> writer;
    juce::int64 frames = 0;
    float peak = 0.0f, monoPeak = 0.0f;
};

std::unique_ptr<juce::AudioFormatWriter> floatWriter(const juce::File& file, double sampleRate)
{
    std::unique_ptr<juce::OutputStream> out(file.createOutputStream());
    if (out == nullptr)
        return nullptr;
    return std::unique_ptr<juce::AudioFormatWriter>(juce::WavAudioFormat().createWriterFor(
        out, juce::AudioFormatWriterOptions{}.withSampleRate(sampleRate).withNumChannels(2).withBitsPerSample(32)));
}

std::unique_ptr<juce::AudioFormat> formatFor(ExportFormat f)
{
    switch (f)
    {
        case ExportFormat::Aiff: return std::make_unique<juce::AiffAudioFormat>();
        case ExportFormat::Flac: return std::make_unique<juce::FlacAudioFormat>();
        case ExportFormat::Wav: break;
    }
    return std::make_unique<juce::WavAudioFormat>();
}
} // namespace

std::string Exporter::extensionFor(ExportFormat format)
{
    return format == ExportFormat::Aiff ? ".aif" : format == ExportFormat::Flac ? ".flac" : ".wav";
}

std::string Exporter::safeFileName(const std::string& name)
{
    std::string out;
    for (char c : name)
        out += (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') ? '-' : c;
    while (! out.empty() && (out.back() == ' ' || out.back() == '.'))
        out.pop_back();
    return out.empty() ? std::string("Track") : out;
}

double Exporter::arrangementEndBeat(const model::Song& song)
{
    double end = 0.0;
    auto scan = [&end](const std::vector<model::Track>& tracks) {
        for (const auto& t : tracks)
            for (const auto& c : t.arrangementClips)
                end = std::max(end, c.currentEnd);
    };
    scan(song.tracks);
    const double bar = std::max(1.0, song.timeSigNumerator * 4.0 / std::max(1, song.timeSigDenominator));
    return end <= 0.0 ? bar : std::ceil(end / bar - 1e-9) * bar;
}

Exporter::Range Exporter::resolveRange(const model::Song& song, int mode, double startBar, double endBar)
{
    const double bar = std::max(1.0, song.timeSigNumerator * 4.0 / std::max(1, song.timeSigDenominator));
    if (mode == 1 && song.loopOn && song.loopLength > 0.0)
        return {song.loopStart, song.loopStart + song.loopLength};
    if (mode == 2)
    {
        const double start = std::max(0.0, (std::floor(startBar) - 1.0) * bar);
        return {start, std::max(start + bar, (std::floor(endBar) - 1.0) * bar)};
    }
    return {0.0, arrangementEndBeat(song)};
}

ExportResult Exporter::run(const ExportOptions& options, std::atomic<float>& progress, const std::atomic<bool>& cancel)
{
    ExportResult result;
    const auto wallStart = juce::Time::getMillisecondCounterHiRes();
    auto* graph = engine_.latestGraph();
    if (graph == nullptr || ! engine_.offlineActive())
    {
        result.error = "The audio engine is not ready";
        return result;
    }
    if (options.endBeat <= options.startBeat)
    {
        result.error = "The render range is empty";
        return result;
    }
    const double sampleRate = engine_.sampleRate();
    const int mainNode = graph->mainNode;

    // ---- which streams
    std::vector<Stream> streams;
    if (options.target == ExportTarget::Main)
        streams.push_back({"Main", -1, {}, nullptr, 0, 0.0f, 0.0f});
    else if (options.target == ExportTarget::SingleTrack)
    {
        const auto* track = model::trackAt(song_, options.trackIndex);
        if (track == nullptr || options.trackIndex < 0 || (size_t) options.trackIndex >= graph->nodes.size() || options.trackIndex == mainNode)
        {
            result.error = "That track cannot be exported";
            return result;
        }
        streams.push_back({track->name, options.trackIndex, {}, nullptr, 0, 0.0f, 0.0f});
    }
    else
    {
        for (size_t i = 0; i < graph->nodes.size(); ++i)
            if ((int) i != mainNode)
                if (const auto* track = model::trackAt(song_, (int) i))
                    streams.push_back({track->name, (int) i, {}, nullptr, 0, 0.0f, 0.0f});
        if (streams.empty())
        {
            result.error = "There are no tracks to export";
            return result;
        }
    }

    juce::TemporaryFile dummy; // scratch files live in the temp directory
    for (size_t i = 0; i < streams.size(); ++i)
    {
        streams[i].scratch = juce::File::createTempFile("sonora-export-" + juce::String((int) i) + ".wav");
        streams[i].writer = floatWriter(streams[i].scratch, sampleRate);
        if (streams[i].writer == nullptr)
        {
            result.error = "Cannot write a scratch file in the temporary folder";
            for (auto& s : streams) { s.writer.reset(); s.scratch.deleteFile(); }
            return result;
        }
    }
    auto cleanup = [&] {
        for (auto& s : streams)
        {
            s.writer.reset();
            s.scratch.deleteFile();
        }
    };

    // ---- pass 1: render. Outputs are delayed by plugin latency; the first frames of each stream are dropped.
    const auto& main = graph->nodes[(size_t) mainNode];
    int mainOwn = 0;
    for (const auto& d : main.devices)
        mainOwn += std::max(0, d->latencySamples());
    const juce::int64 skipMain = main.latency;
    const juce::int64 skipStem = options.preMixer && options.trackIndex >= 0
        ? graph->nodes[(size_t) options.trackIndex].latency : std::max(0, main.latency - mainOwn);
    std::vector<int> stemNodes;
    for (const auto& s : streams)
        if (s.node >= 0)
            stemNodes.push_back(s.node);

    std::vector<juce::int64> skipLeft(streams.size());
    for (size_t i = 0; i < streams.size(); ++i)
        skipLeft[i] = streams[i].node < 0 ? skipMain : skipStem;
    const double rangeBeats = options.endBeat - options.startBeat;
    juce::int64 beyondEnd = 0, silentFrames = 0, framesAfterEnd = 0;
    const juce::int64 tailFrames = options.autoTail ? (juce::int64) (30.0 * sampleRate) : (juce::int64) (options.tailSeconds * sampleRate);
    bool reachedEnd = false;
    const juce::int64 latencyFrames = std::max(skipMain, skipStem);
    std::vector<float> scratchL, scratchR;
    juce::int64 inputFrames = 0;
    auto addMarker = [&](double beat) {
        model::WarpMarker marker{(double) inputFrames / sampleRate, beat - options.startBeat};
        auto& markers = result.warpMarkers;
        if (markers.size() >= 2)
        {
            const auto& a = markers[markers.size() - 2]; const auto& b = markers.back();
            const double expected = a.beats + (marker.seconds - a.seconds) * (b.beats - a.beats) / std::max(1e-12, b.seconds - a.seconds);
            if (std::abs(expected - marker.beats) < 1e-8) { markers.back() = marker; return; }
        }
        markers.push_back(marker);
    };

    engine_.renderOffline(
        options.startBeat, stemNodes,
        [&](double position, double samplesPerBeat, int maxBlock) -> int {
            if (cancel.load())
                return 0;
            addMarker(position);
            progress.store((float) std::clamp((position - options.startBeat) / rangeBeats, 0.0, 1.0) * 0.8f);
            if (! reachedEnd)
            {
                const double remainingBeats = options.endBeat - position;
                if (remainingBeats > 1e-9)
                    return std::max(1, std::min(maxBlock, (int) std::ceil(remainingBeats * samplesPerBeat - 1e-9)));
                reachedEnd = true;
            }
            // after the range: plugin latency, then the tail
            const juce::int64 budget = latencyFrames + tailFrames - framesAfterEnd;
            if (budget <= 0)
                return 0;
            if (options.autoTail && framesAfterEnd > latencyFrames && silentFrames > (juce::int64) (sampleRate * 0.5))
                return 0; // silent for half a second
            return (int) std::min<juce::int64>(maxBlock, budget);
        },
        [&](const AudioEngine::OfflineBlock& block) {
            inputFrames += block.numFrames;
            if (reachedEnd)
            {
                framesAfterEnd += block.numFrames;
                float level = 0.0f;
                const bool trackTail = options.target == ExportTarget::SingleTrack && block.stems && ! block.stems->empty();
                const auto* left = trackTail ? (*block.stems)[0][0] : block.mainL;
                const auto* right = trackTail ? (*block.stems)[0][1] : block.mainR;
                for (int i = 0; i < block.numFrames; ++i)
                    level = std::max({level, std::abs(left[i]), std::abs(right[i])});
                silentFrames = level < 1.0e-4f ? silentFrames + block.numFrames : 0; // -80 dBFS
            }
            else
                beyondEnd += block.numFrames;
            size_t stemIndex = 0;
            for (size_t si = 0; si < streams.size(); ++si)
            {
                auto& s = streams[si];
                const float* l = block.mainL;
                const float* r = block.mainR;
                if (s.node >= 0)
                {
                    l = (*block.stems)[stemIndex][0];
                    r = (*block.stems)[stemIndex][1];
                    ++stemIndex;
                }
                int offset = 0, count = block.numFrames;
                if (skipLeft[si] > 0)
                {
                    const int drop = (int) std::min<juce::int64>(skipLeft[si], count);
                    skipLeft[si] -= drop;
                    offset = drop;
                    count -= drop;
                }
                if (count <= 0)
                    continue;
                const float* ptr[2] = {l + offset, r + offset};
                s.writer->writeFromFloatArrays(ptr, 2, count);
                s.frames += count;
                for (int i = 0; i < count; ++i)
                {
                    s.peak = std::max({s.peak, std::abs(ptr[0][i]), std::abs(ptr[1][i])});
                    s.monoPeak = std::max(s.monoPeak, std::abs(0.5f * (ptr[0][i] + ptr[1][i])));
                }
            }
        }, options.preMixer, options.sessionScene);
    for (auto& s : streams)
        s.writer.reset();
    if (cancel.load())
    {
        cleanup();
        result.cancelled = true;
        result.error = "Cancelled";
        return result;
    }

    // ---- pass 2: normalise / mono / dither / bit depth into the final files
    const bool toFolder = options.target == ExportTarget::AllTracks;
    if (toFolder && ! options.destination.createDirectory().wasOk())
    {
        cleanup();
        result.error = "Cannot create the folder " + options.destination.getFullPathName().toStdString();
        return result;
    }
    const int bits = options.format == ExportFormat::Wav ? (options.bitDepth == 16 || options.bitDepth == 24 ? options.bitDepth : 32)
                                                          : (options.bitDepth == 16 ? 16 : 24);
    juce::AudioFormatManager scratchFormats;
    scratchFormats.registerBasicFormats();
    std::mt19937 rng(0x5eed);
    std::uniform_real_distribution<float> uniform(0.0f, 1.0f);
    const float lsb = bits < 32 ? 1.0f / (float) (1 << (bits - 1)) : 0.0f;
    result.files.reserve(streams.size());

    for (size_t si = 0; si < streams.size(); ++si)
    {
        auto& s = streams[si];
        progress.store(0.8f + 0.2f * (float) si / (float) streams.size());
        if (cancel.load())
        {
            cleanup();
            result.cancelled = true;
            result.error = "Cancelled";
            return result;
        }
        juce::File target = options.destination;
        if (toFolder)
        {
            char number[16];
            std::snprintf(number, sizeof number, "%02d", (int) si + 1);
            target = options.destination.getChildFile(juce::String(safeFileName(options.baseName)) + " - " + juce::String(number) + " "
                                                     + juce::String(safeFileName(s.name)) + juce::String(extensionFor(options.format)));
        }
        else if (target.getFileExtension().isEmpty())
            target = target.withFileExtension(juce::String(extensionFor(options.format)).substring(1));
        target.deleteFile();

        std::unique_ptr<juce::AudioFormatReader> reader(scratchFormats.createReaderFor(s.scratch));
        std::unique_ptr<juce::OutputStream> out(target.createOutputStream());
        if (reader == nullptr || out == nullptr)
        {
            cleanup();
            result.error = "Cannot write " + target.getFullPathName().toStdString();
            return result;
        }
        const int channels = options.mono ? 1 : 2;
        auto format = formatFor(options.format);
        std::unique_ptr<juce::AudioFormatWriter> writer(format->createWriterFor(
            out, juce::AudioFormatWriterOptions{}.withSampleRate(sampleRate).withNumChannels(channels).withBitsPerSample(bits)));
        if (writer == nullptr)
        {
            cleanup();
            target.deleteFile();
            result.error = "The file format does not support " + std::to_string(bits) + " bit";
            return result;
        }

        const float peak = options.mono ? s.monoPeak : s.peak;
        const float gain = options.normalize && peak > 1.0e-6f ? 1.0f / peak : 1.0f;
        dsp::LoudnessMeter meter(sampleRate);
        const bool measure = si == 0;
        juce::AudioBuffer<float> block(2, 16384);
        juce::int64 position = 0;
        bool failed = false;
        while (position < reader->lengthInSamples && ! failed)
        {
            const int n = (int) std::min<juce::int64>(block.getNumSamples(), reader->lengthInSamples - position);
            reader->read(&block, 0, n, position, true, true);
            position += n;
            float* l = block.getWritePointer(0);
            float* r = block.getWritePointer(1);
            for (int i = 0; i < n; ++i)
            {
                l[i] *= gain;
                r[i] *= gain;
            }
            if (measure)
                meter.process(l, r, n);
            if (options.mono)
                for (int i = 0; i < n; ++i)
                    l[i] = 0.5f * (l[i] + r[i]);
            if (options.dither && lsb > 0.0f)
                for (int c = 0; c < channels; ++c)
                {
                    float* d = block.getWritePointer(c);
                    for (int i = 0; i < n; ++i)
                        d[i] = std::clamp(d[i] + (uniform(rng) - uniform(rng)) * lsb, -1.0f, 1.0f); // TPDF
                }
            const float* ptr[2] = {l, options.mono ? l : r};
            failed = ! writer->writeFromFloatArrays(ptr, channels, n);
        }
        writer.reset(); // flush
        if (failed)
        {
            cleanup();
            result.error = "Writing " + target.getFullPathName().toStdString() + " failed (disk full?)";
            return result;
        }
        if (measure)
        {
            result.integratedLufs = meter.integrated();
            result.loudnessRange = meter.loudnessRange();
            result.truePeakDb = meter.truePeakDb();
            result.samplePeakDb = meter.samplePeakDb();
            result.seconds = (double) reader->lengthInSamples / sampleRate;
        }
        result.files.push_back({target, s.name});
    }
    cleanup();
    progress.store(1.0f);
    result.ok = true;
    result.renderSeconds = (juce::Time::getMillisecondCounterHiRes() - wallStart) / 1000.0;
    return result;
}

} // namespace sonora::engine

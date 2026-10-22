#include "engine/ClipRender.h"
#include "io/als/AlsReader.h"
#include <cmath>
namespace sonora::engine
{
ClipRenderResult renderClipRange(const model::Song& song, const std::vector<model::Clip>& clips, double start, double end,
                                 double rate, SampleCache& cache, const std::atomic<bool>& cancel)
{
    ClipRenderResult result;
    const double frames = (end - start) * 60.0 / song.tempo * rate;
    if (! std::isfinite(frames) || frames <= 0 || frames > rate * 600 || rate < 1000 || song.tempo <= 0)
    { result.error = "Choose an audio range between one sample and ten minutes"; return result; }
    auto rendered = std::make_shared<SampleData>(); rendered->sampleRate = rate;
    rendered->audio.setSize(2, (int) std::ceil(frames)); rendered->audio.clear();
    constexpr int block = 512;
    juce::AudioBuffer<float> scratch(2, block);
    const double spb = rate * 60 / song.tempo;
    for (const auto& clip : clips)
    {
        if (clip.disabled || clip.kind != model::Clip::Kind::Audio || clip.currentEnd <= start || clip.currentStart >= end) continue;
        const auto path = io::resolveSamplePath(song, clip.sample);
        const auto data = path.empty() ? nullptr : cache.load(path);
        if (! data) { result.error = "Sample not found: " + clip.name; return result; }
        const auto prepared = prepareClip(clip, data);
        AudioPlayState player; player.prepare(rate, block);
        const int first = std::max(0, (int) std::floor((clip.currentStart - start) * spb));
        const int last = std::min(rendered->audio.getNumSamples(), (int) std::ceil((clip.currentEnd - start) * spb));
        for (int pos = first; pos < last; pos += block)
        {
            if (cancel.load()) { result.error = "Cancelled"; return result; }
            const int count = std::min(block, last - pos); scratch.clear();
            player.render(prepared, {0, count, start + pos / spb, spb}, song.tempo, scratch);
            for (int ch = 0; ch < 2; ++ch) rendered->audio.addFrom(ch, pos, scratch, ch, 0, count);
        }
    }
    result.sample = std::move(rendered);
    return result;
}
}

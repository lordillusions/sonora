#include "core/ResynthModel.h"
#include <juce_core/juce_core.h>
#include <algorithm>
#include <cmath>

namespace sonora::model
{
namespace
{
constexpr int magic = 0x53525331;
uint32_t checksum(const uint8_t* bytes, size_t size)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}
bool finite(float value, float low, float high) { return std::isfinite(value) && value >= low && value <= high; }
}

bool validateResynth(const ResynthModel& model, std::string& error)
{
    error.clear();
    const auto fail = [&] { error = "Invalid or unsupported Resynth model"; return false; };
    if (! std::isfinite(model.sampleRate) || model.sampleRate < 8000 || model.sampleRate > 96000
        || model.sourceFrames < 0 || model.duration() <= 0 || model.duration() > ResynthModel::maxSeconds
        || (! model.source.empty() && model.sourceFrames > 0 && model.source.size() != (size_t) model.sourceFrames)
        || model.source.size() > (size_t) (model.sampleRate * ResynthModel::maxSeconds)
        || model.notes.empty() || model.notes.size() > ResynthModel::maxNotes || model.name.size() > 1024
        || model.analysisChannel < 0 || model.analysisChannel > 1) return fail();
    if (! juce::CharPointer_UTF8::isValidString(model.name.c_str(), (int) model.name.size())) return fail();
    for (float sample : model.source) if (! finite(sample, -16, 16)) return fail();
    size_t frameCount = 0;
    float previousEnd = 0;
    for (const auto& note : model.notes)
    {
        if (! finite(note.start, previousEnd, (float) ResynthModel::maxSeconds)
            || ! finite(note.end, note.start + 0.01f, (float) model.duration() + 0.0001f)
            || ! finite(note.root, 0, 127) || ! finite(note.confidence, 0, 1)
            || ! finite(note.sustain, 0, note.end - note.start)
            || note.frames.size() < 2 || note.frames.size() > 128) return fail();
        previousEnd = note.end;
        float previousTime = -1;
        frameCount += note.frames.size();
        if (frameCount > ResynthModel::maxFrames) return fail();
        for (const auto& frame : note.frames)
        {
            if (! finite(frame.time, 0, note.end - note.start + 0.0001f) || frame.time <= previousTime
                || ! finite(frame.pitch, 0, 127) || ! finite(frame.confidence, 0, 1)) return fail();
            previousTime = frame.time;
            for (int i = 0; i < ResynthFrame::harmonics; ++i)
                if (! finite(frame.cosine[(size_t) i], -32, 32) || ! finite(frame.sine[(size_t) i], -32, 32)) return fail();
            for (float band : frame.noise) if (! finite(band, 0, 16)) return fail();
        }
    }
    return true;
}

std::shared_ptr<const NativeContent> encodeResynth(const ResynthModel& model, std::string& error)
{
    if (! validateResynth(model, error)) return {};
    juce::MemoryOutputStream output;
    output.writeInt(magic); output.writeInt(ResynthModel::version);
    output.writeString(juce::String::fromUTF8(model.name.c_str()));
    output.writeDouble(model.sampleRate); output.writeInt(model.analysisChannel);
    output.writeInt(model.sourceFrames > 0 ? model.sourceFrames : (int) model.source.size());
    output.writeInt((int) model.source.size()); output.writeInt((int) model.notes.size());
    for (float sample : model.source) output.writeFloat(sample);
    for (const auto& note : model.notes)
    {
        output.writeFloat(note.start); output.writeFloat(note.end); output.writeFloat(note.root);
        output.writeFloat(note.confidence); output.writeFloat(note.sustain); output.writeInt((int) note.frames.size());
        for (const auto& frame : note.frames)
        {
            output.writeFloat(frame.time); output.writeFloat(frame.pitch); output.writeFloat(frame.confidence);
            for (int i = 0; i < ResynthFrame::harmonics; ++i)
            { output.writeFloat(frame.cosine[(size_t) i]); output.writeFloat(frame.sine[(size_t) i]); }
            for (float band : frame.noise) output.writeFloat(band);
        }
    }
    auto content = std::make_shared<NativeContent>(); content->format = "sonora-resynth-v2";
    const auto* bytes = static_cast<const uint8_t*>(output.getData());
    output.writeInt((int) checksum(bytes, output.getDataSize()));
    bytes = static_cast<const uint8_t*>(output.getData());
    content->data.assign(bytes, bytes + output.getDataSize());
    return content;
}

bool trimResynthNote(ResynthModel& model, int index, float start, float end)
{
    if (index < 0 || (size_t) index >= model.notes.size() || ! std::isfinite(start) || ! std::isfinite(end)) return false;
    auto& note = model.notes[(size_t) index];
    if (start < note.start || end > note.end || end - start < 0.02f || note.frames.size() < 2) return false;
    const auto old = note;
    const size_t count = note.frames.size();
    for (size_t i = 0; i < count; ++i)
    {
        const float time = (end - start) * (float) i / (float) (count - 1);
        const float at = time + start - old.start;
        const auto found = std::upper_bound(old.frames.begin(), old.frames.end(), at, [](float value, const auto& frame) { return value < frame.time; });
        const size_t b = std::clamp((size_t) (found - old.frames.begin()), (size_t) 1, count - 1), a = b - 1;
        const float mix = std::clamp((at - old.frames[a].time) / (old.frames[b].time - old.frames[a].time), 0.0f, 1.0f);
        auto& frame = note.frames[i]; frame.time = time;
        frame.pitch = std::lerp(old.frames[a].pitch, old.frames[b].pitch, mix);
        frame.confidence = std::lerp(old.frames[a].confidence, old.frames[b].confidence, mix);
        for (size_t h = 0; h < frame.cosine.size(); ++h)
        {
            frame.cosine[h] = std::lerp(old.frames[a].cosine[h], old.frames[b].cosine[h], mix);
            frame.sine[h] = std::lerp(old.frames[a].sine[h], old.frames[b].sine[h], mix);
        }
        for (size_t band = 0; band < frame.noise.size(); ++band) frame.noise[band] = std::lerp(old.frames[a].noise[band], old.frames[b].noise[band], mix);
    }
    note.start = start; note.end = end; note.sustain = std::clamp(old.sustain - (start - old.start), 0.0f, end - start);
    return true;
}

void removeResynthSource(ResynthModel& model)
{
    if (model.sourceFrames <= 0) model.sourceFrames = (int) model.source.size();
    std::vector<float>().swap(model.source);
}

std::shared_ptr<const ResynthModel> decodeResynth(const NativeContent& content, std::string& error)
{
    error = "Resynth model is missing, corrupt or from an unsupported version";
    const auto& data = content.data;
    if ((content.format != "sonora-resynth-v1" && content.format != "sonora-resynth-v2")
        || data.size() < 36 || data.size() > 16 * 1024 * 1024) return {};
    juce::MemoryInputStream input(data.data(), data.size(), false);
    if (input.readInt() != magic) return {};
    const int version = input.readInt();
    if ((version != 1 && version != ResynthModel::version)
        || content.format != (version == 1 ? "sonora-resynth-v1" : "sonora-resynth-v2")) return {};
    // Bound the zero-terminated name before letting String allocate.
    const auto nameEnd = std::find(data.begin() + 8, data.begin() + (std::ptrdiff_t) std::min<size_t>(1033, data.size()), 0);
    if (nameEnd == data.begin() + (std::ptrdiff_t) std::min<size_t>(1033, data.size())) return {};
    if (! juce::CharPointer_UTF8::isValidString(reinterpret_cast<const char*>(data.data() + 8), (int) (nameEnd - data.begin() - 8))) return {};
    auto model = std::make_shared<ResynthModel>(); model->name = input.readString().toStdString();
    model->sampleRate = input.readDouble(); model->analysisChannel = input.readInt();
    model->sourceFrames = input.readInt();
    const int samples = version == 1 ? model->sourceFrames : input.readInt(), notes = input.readInt();
    if (samples < 0 || samples > 2880000 || model->sourceFrames <= 0 || model->sourceFrames > 2880000
        || (samples > 0 && samples != model->sourceFrames) || notes <= 0 || notes > ResynthModel::maxNotes
        || (uint64_t) samples * 4 + (uint64_t) input.getPosition() + 4 > data.size()) return {};
    model->source.resize((size_t) samples);
    for (auto& sample : model->source) sample = input.readFloat();
    int totalFrames = 0;
    for (int n = 0; n < notes; ++n)
    {
        if ((size_t) input.getPosition() + 24 + 4 > data.size()) return {};
        ResynthNote note;
        note.start = input.readFloat(); note.end = input.readFloat(); note.root = input.readFloat();
        note.confidence = input.readFloat(); note.sustain = input.readFloat();
        const int frames = input.readInt(); totalFrames += frames;
        constexpr int frameBytes = (3 + 2 * ResynthFrame::harmonics + ResynthFrame::noiseBands) * 4;
        if (frames < 2 || frames > 128 || totalFrames > ResynthModel::maxFrames
            || (uint64_t) input.getPosition() + (uint64_t) frames * frameBytes + 4 > data.size()) return {};
        note.frames.resize((size_t) frames);
        for (auto& frame : note.frames)
        {
            frame.time = input.readFloat(); frame.pitch = input.readFloat(); frame.confidence = input.readFloat();
            for (int i = 0; i < ResynthFrame::harmonics; ++i)
            { frame.cosine[(size_t) i] = input.readFloat(); frame.sine[(size_t) i] = input.readFloat(); }
            for (auto& band : frame.noise) band = input.readFloat();
        }
        model->notes.push_back(std::move(note));
    }
    if ((size_t) input.getPosition() + 4 != data.size()) return {};
    const auto stored = (uint32_t) input.readInt();
    if (stored != checksum(data.data(), data.size() - 4) || ! validateResynth(*model, error)) return {};
    error.clear(); return model;
}
} // namespace sonora::model

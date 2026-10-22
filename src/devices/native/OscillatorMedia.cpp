#include "devices/native/OscillatorMedia.h"
#include "dsp/Dsp.h"

#include <juce_dsp/juce_dsp.h>

#include <algorithm>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <complex>
#include <cstdlib>
#include <map>
#include <new>
#include <set>
#include <stdexcept>

#include "platform/CNumeric.h"

namespace sonora::devices
{
namespace
{
constexpr double tau = 2 * dsp::kPi;
constexpr int windowResolution = 2048;
// This translation unit is first used by the immutable sample factories on the load thread.
// Dynamic initialization prepares this fixed lookup before any asset can reach next().
const std::array<float, windowResolution + 1> hannWindow = [] {
    std::array<float, windowResolution + 1> table{};
    for (int i = 0; i <= windowResolution; ++i)
        table[static_cast<size_t>(i)] = static_cast<float>(0.5 - 0.5 * std::cos(tau * i / windowResolution));
    return table;
}();
float hann(double phase)
{
    const double position = std::clamp(phase, 0.0, 1.0) * windowResolution;
    const auto first = static_cast<size_t>(position), second = std::min(first + 1, static_cast<size_t>(windowResolution));
    return hannWindow[first] + (hannWindow[second] - hannWindow[first]) * static_cast<float>(position - static_cast<double>(first));
}
OscillatorSampleResult sampleFailure(std::string error) { return {nullptr, std::move(error)}; }
OscillatorMediaResult mediaFailure(std::string error) { return {nullptr, std::move(error), {}}; }
double bounded(double value, double low, double high, double fallback)
{
    return std::isfinite(value) ? std::clamp(value, low, high) : fallback;
}
float balanceLeft(float pan) { return std::min(1.0f, 1.0f - pan); }
float balanceRight(float pan) { return std::min(1.0f, 1.0f + pan); }
}

OscillatorSampleResult OscillatorSample::fromSamples(std::span<const float> interleaved, int channels,
                                                     double sampleRate, std::string name, bool spectral, int spectralFrameLimit)
{
    if (spectralFrameLimit < 2 || spectralFrameLimit > maxSpectralFrames) return sampleFailure("Invalid spectral frame limit");
    if (channels < 1 || channels > 2) return sampleFailure("Oscillator media requires one or two channels");
    if (interleaved.empty() || interleaved.size() % static_cast<size_t>(channels) != 0)
        return sampleFailure("Oscillator media sample/channel alignment is invalid");
    if (interleaved.size() > maxPcmBytes / sizeof(float)) return sampleFailure("Oscillator PCM exceeds the 64 MiB limit");
    if (! std::isfinite(sampleRate) || sampleRate < 1 || sampleRate > 768000) return sampleFailure("Invalid oscillator media sample rate");
    for (const float sample : interleaved)
        if (! std::isfinite(sample) || std::abs(sample) > 32) return sampleFailure("Oscillator media contains nonfinite or excessive sample values");
    try
    {
        auto data = std::shared_ptr<OscillatorSample>(new OscillatorSample);
        data->channels_ = channels;
        data->sampleRate_ = sampleRate;
        data->name_ = name.empty() ? "Embedded Sample" : name.substr(0, 1024);
        data->samples_ = std::make_shared<const std::vector<float>>(interleaved.begin(), interleaved.end());
        data->pcmFingerprint_ = 0xcbf29ce484222325ULL;
        for (float sample : interleaved)
        {
            const uint32_t bits = std::bit_cast<uint32_t>(sample);
            for (int byte = 0; byte < 4; ++byte)
            {
                data->pcmFingerprint_ ^= (bits >> (byte * 8)) & 255u;
                data->pcmFingerprint_ *= 0x100000001b3ULL;
            }
        }
        if (spectral) data->analyzeSpectral(spectralFrameLimit);
        return {std::move(data), {}};
    }
    catch (const std::bad_alloc&) { return sampleFailure("Unable to allocate oscillator media"); }
    catch (const std::length_error&) { return sampleFailure("Oscillator media allocation length exceeded"); }
}
OscillatorSampleResult OscillatorSample::fromFile(const juce::File& file, bool spectral, int spectralFrameLimit)
{
    if (spectralFrameLimit < 2 || spectralFrameLimit > maxSpectralFrames) return sampleFailure("Invalid spectral frame limit");
    if (! file.existsAsFile()) return sampleFailure("Oscillator media file does not exist");
    try
    {
        juce::AudioFormatManager formats;
        formats.registerBasicFormats();
        const std::unique_ptr<juce::AudioFormatReader> reader(formats.createReaderFor(file));
        if (! reader) return sampleFailure("Unsupported or malformed oscillator audio file");
        if (reader->numChannels < 1 || reader->numChannels > 2) return sampleFailure("Oscillator media file requires one or two channels");
        if (reader->lengthInSamples <= 0 || static_cast<uint64_t>(reader->lengthInSamples) > maxPcmBytes / sizeof(float) / reader->numChannels)
            return sampleFailure("Oscillator media file exceeds the 64 MiB PCM limit");
        const int channels = static_cast<int>(reader->numChannels), count = static_cast<int>(reader->lengthInSamples);
        juce::AudioBuffer<float> decoded(channels, count);
        if (! reader->read(&decoded, 0, count, 0, true, channels == 2)) return sampleFailure("Unable to read oscillator audio file");
        std::vector<float> interleaved(static_cast<size_t>(count) * static_cast<size_t>(channels));
        for (int frame = 0; frame < count; ++frame)
            for (int channel = 0; channel < channels; ++channel)
                interleaved[static_cast<size_t>(frame) * static_cast<size_t>(channels) + static_cast<size_t>(channel)] = decoded.getSample(channel, frame);
        auto result = fromSamples(interleaved, channels, reader->sampleRate, file.getFileNameWithoutExtension().toStdString(), spectral, spectralFrameLimit);
        if (result) std::const_pointer_cast<OscillatorSample>(result.data)->sourceFile_ = file.getFullPathName().toStdString();
        return result;
    }
    catch (const std::bad_alloc&) { return sampleFailure("Unable to allocate oscillator file buffer"); }
    catch (const std::length_error&) { return sampleFailure("Oscillator file allocation length exceeded"); }
}
float OscillatorSample::sampleLinear(double position, int channel) const
{
    if (! std::isfinite(position) || position < 0 || position >= sampleFrames()) return 0;
    const int first = static_cast<int>(position), second = std::min(first + 1, sampleFrames() - 1);
    const auto ch = static_cast<size_t>(std::clamp(channel, 0, channels_ - 1)), channels = static_cast<size_t>(channels_);
    const float mix = static_cast<float>(position - first);
    const float left = (*samples_)[static_cast<size_t>(first) * channels + ch], right = (*samples_)[static_cast<size_t>(second) * channels + ch];
    return left + (right - left) * mix;
}

OscillatorSampleResult OscillatorSample::withSpectralAnalysis(std::shared_ptr<const OscillatorSample> source, int frameLimit)
{
    if (! source || frameLimit < 2 || frameLimit > maxSpectralFrames) return sampleFailure("Invalid spectral source/frame limit");
    try
    {
        auto data = std::shared_ptr<OscillatorSample>(new OscillatorSample);
        data->samples_ = source->samples_; data->channels_ = source->channels_; data->sampleRate_ = source->sampleRate_;
        data->name_ = source->name_; data->sourceFile_ = source->sourceFile_; data->pcmFingerprint_ = source->pcmFingerprint_;
        data->analyzeSpectral(frameLimit);
        return {std::move(data), {}};
    }
    catch (const std::bad_alloc&) { return sampleFailure("Unable to allocate bounded spectral analysis"); }
    catch (const std::length_error&) { return sampleFailure("Spectral analysis length exceeded"); }
}

void OscillatorSample::analyzeSpectral(int frameLimit)
{
    // Own bounded analysis: 2048-point Hann STFT and up to 64 strongest local peaks per frame.
    // The caller supplies a bounded frame budget; increasing the hop reduces time resolution.
    // Original PCM remains complete. No transient/phase-lock model is inferred.
    spectralHop_ = std::max(512, (sampleFrames() + frameLimit - 2) / (frameLimit - 1));
    const int frames = std::min(frameLimit, (sampleFrames() + spectralHop_ - 1) / spectralHop_ + 1);
    spectral_.resize(static_cast<size_t>(frames));
    const juce::dsp::FFT fft(11);
    using Complex = std::complex<float>;
    std::vector<Complex> input(spectralSize), left(spectralSize), right(spectralSize);
    std::array<float, spectralSize / 2 + 1> magnitude{};
    struct Peak { int bin = 0; float strength = 0; };
    std::array<Peak, spectralPartials> peaks{};
    for (int frame = 0; frame < frames; ++frame)
    {
        for (int channel = 0; channel < channels_; ++channel)
        {
            for (int sample = 0; sample < spectralSize; ++sample)
            {
                const int source = frame * spectralHop_ + sample - spectralSize / 2;
                const float window = static_cast<float>(0.5 - 0.5 * std::cos(tau * sample / spectralSize));
                input[static_cast<size_t>(sample)] = {sampleLinear(source, channel) * window, 0};
            }
            fft.perform(input.data(), channel == 0 ? left.data() : right.data(), false);
        }
        if (channels_ == 1) right = left;
        float maximum = 0;
        for (int bin = 0; bin <= spectralSize / 2; ++bin)
        {
            magnitude[static_cast<size_t>(bin)] = std::max(std::abs(left[static_cast<size_t>(bin)]), std::abs(right[static_cast<size_t>(bin)]));
            maximum = std::max(maximum, magnitude[static_cast<size_t>(bin)]);
        }
        peaks = {};
        int count = 0;
        auto add = [&](int bin)
        {
            const float strength = magnitude[static_cast<size_t>(bin)];
            if (strength < maximum * 0.001f || strength < 1e-7f) return;
            if (count < spectralPartials) peaks[static_cast<size_t>(count++)] = {bin, strength};
            else
            {
                auto weakest = std::min_element(peaks.begin(), peaks.end(), [](const Peak& a, const Peak& b) { return a.strength < b.strength; });
                if (strength > weakest->strength) *weakest = {bin, strength};
            }
        };
        add(0);
        for (int bin = 1; bin < spectralSize / 2; ++bin)
            if (magnitude[static_cast<size_t>(bin)] >= magnitude[static_cast<size_t>(bin - 1)]
                && magnitude[static_cast<size_t>(bin)] > magnitude[static_cast<size_t>(bin + 1)]) add(bin);
        std::sort(peaks.begin(), peaks.begin() + count, [](const Peak& a, const Peak& b) { return a.bin < b.bin; });
        auto& target = spectral_[static_cast<size_t>(frame)];
        for (int p = 0; p < count; ++p)
        {
            const int bin = peaks[static_cast<size_t>(p)].bin;
            double delta = 0;
            if (bin > 0 && bin < spectralSize / 2)
            {
                const double a = std::log(std::max(1e-12f, magnitude[static_cast<size_t>(bin - 1)]));
                const double b = std::log(std::max(1e-12f, magnitude[static_cast<size_t>(bin)]));
                const double c = std::log(std::max(1e-12f, magnitude[static_cast<size_t>(bin + 1)]));
                const double denominator = a - 2 * b + c;
                if (std::abs(denominator) > 1e-12) delta = std::clamp(0.5 * (a - c) / denominator, -0.5, 0.5);
            }
            const auto at = static_cast<size_t>(bin);
            const float scale = (bin == 0 ? 2.0f : 4.0f) / spectralSize;
            target.partial[static_cast<size_t>(p)] = {static_cast<float>((bin + delta) * sampleRate_ / spectralSize),
                std::abs(left[at]) * scale, std::abs(right[at]) * scale, std::arg(left[at]), std::arg(right[at])};
        }
    }
}

OscillatorMediaResult OscillatorMedia::fromSamples(std::span<const float> interleaved, int channels,
                                                   double sampleRate, std::string name, bool spectral)
{
    const auto sample = OscillatorSample::fromSamples(interleaved, channels, sampleRate, name, false);
    if (! sample) return mediaFailure(sample.error);
    OscillatorZone zone;
    zone.sample = sample.data;
    return fromZones({zone}, std::move(name), spectral);
}
OscillatorMediaResult OscillatorMedia::fromFile(const juce::File& file, bool spectral)
{
    if (file.hasFileExtension("sfz")) return fromSfz(file, spectral);
    const auto sample = OscillatorSample::fromFile(file, false);
    if (! sample) return mediaFailure(sample.error);
    OscillatorZone zone;
    zone.sample = sample.data;
    return fromZones({zone}, file.getFileNameWithoutExtension().toStdString(), spectral);
}
OscillatorMediaResult OscillatorMedia::fromZones(std::vector<OscillatorZone> zones, std::string name, bool spectral, size_t spectralBudgetBytes)
{
    if (zones.empty() || zones.size() > maxZones) return mediaFailure("Oscillator media requires from 1 to 256 zones");
    try
    {
        std::map<const OscillatorSample*, std::shared_ptr<const OscillatorSample>> unique;
        std::set<const float*> pcmOwners;
        size_t bytes = 0;
        if (spectralBudgetBytes > maxSpectralBytes) return mediaFailure("Spectral bank budget exceeds 64 MiB");
        for (auto& zone : zones)
        {
            if (! zone.sample) return mediaFailure("Oscillator zone has no sample");
            unique.emplace(zone.sample.get(), zone.sample);
            if (pcmOwners.insert(zone.sample->originalSamples().data()).second)
            {
                const size_t amount = zone.sample->originalSamples().size() * sizeof(float);
                if (amount > maxTotalPcmBytes - bytes) return mediaFailure("Oscillator PCM bank exceeds the 768 MiB limit");
                bytes += amount;
            }
            if (zone.lowKey < 0 || zone.highKey > 127 || zone.lowKey > zone.highKey || zone.rootKey < 0 || zone.rootKey > 127
                || zone.lowVelocity < 0 || zone.highVelocity > 127 || zone.lowVelocity > zone.highVelocity)
                return mediaFailure("Invalid oscillator key/velocity zone");
            if (! std::isfinite(zone.tuneCents) || std::abs(zone.tuneCents) > 12000 || ! std::isfinite(zone.gainDb) || std::abs(zone.gainDb) > 96
                || ! std::isfinite(zone.pan) || std::abs(zone.pan) > 1 || ! std::isfinite(zone.velocityTracking) || zone.velocityTracking < 0 || zone.velocityTracking > 1
                || ! std::isfinite(zone.keyTracking) || zone.keyTracking < 0 || zone.keyTracking > 1
                || ! std::isfinite(zone.attackSeconds) || zone.attackSeconds < 0 || zone.attackSeconds > 32
                || ! std::isfinite(zone.releaseSeconds) || zone.releaseSeconds < 0 || zone.releaseSeconds > 32)
                return mediaFailure("Invalid oscillator zone tuning/gain/envelope values");
            const int64_t length = zone.sample->sampleFrames();
            if (zone.end < 0) zone.end = length;
            if (zone.loopEnd < 0) zone.loopEnd = zone.end;
            const bool looped = zone.loop == MediaLoop::Forward || zone.loop == MediaLoop::Sustain || zone.loop == MediaLoop::PingPong;
            if (zone.loop < MediaLoop::None || zone.loop > MediaLoop::PingPong)
                return mediaFailure("Invalid oscillator loop mode");
            if (zone.offset < 0 || zone.end > length || zone.offset >= zone.end || zone.loopStart < 0 || (looped && zone.loopStart < zone.offset)
                || zone.loopEnd > zone.end || zone.loopEnd <= zone.offset || zone.loopStart >= zone.loopEnd || zone.loopCrossfade < 0
                || zone.loopCrossfade > (zone.loopEnd - zone.loopStart) / 2)
                return mediaFailure("Invalid oscillator playback/loop range");
        }
        std::vector<std::string> warnings;
        struct Analysis { std::shared_ptr<const OscillatorSample> sample; int desired = 0; };
        std::vector<Analysis> analyses;
        size_t desiredTotal = 0;
        for (const auto& [pointer, source] : unique)
        {
            static_cast<void>(pointer);
            if (! spectral && ! source->hasSpectralAnalysis()) continue;
            const int desired = spectral ? std::min(OscillatorSample::maxSpectralFrames, (source->sampleFrames() + 511) / 512 + 1)
                                         : static_cast<int>(source->spectralFrames().size());
            analyses.push_back({source, desired}); desiredTotal += static_cast<size_t>(desired);
        }
        const size_t frameBudget = spectralBudgetBytes / sizeof(OscillatorSample::SpectralFrame);
        const size_t minimum = analyses.size() * 2;
        const bool available = frameBudget >= minimum;
        const bool reduced = desiredTotal > frameBudget;
        if (! available && ! analyses.empty()) warnings.emplace_back("Spectral bank budget cannot retain two frames per sample; spectral mode uses sample playback");
        else if (reduced) warnings.emplace_back("Spectral bank analysis uses a larger hop to fit its analysis budget; original PCM is complete");
        std::map<const OscillatorSample*, std::shared_ptr<const OscillatorSample>> prepared;
        for (const auto& analysis : analyses)
        {
            const int allowed = ! available ? 0 : ! reduced ? analysis.desired
                : 2 + static_cast<int>((frameBudget - minimum) * static_cast<size_t>(analysis.desired - 2) / (desiredTotal - minimum));
            if (allowed == 0)
            {
                // Build only a lightweight PCM owner when previous analysis exceeds the requested budget.
                auto plain = std::shared_ptr<OscillatorSample>(new OscillatorSample);
                plain->samples_ = analysis.sample->samples_; plain->channels_ = analysis.sample->channels_;
                plain->sampleRate_ = analysis.sample->sampleRate_; plain->name_ = analysis.sample->name_;
                plain->sourceFile_ = analysis.sample->sourceFile_; plain->pcmFingerprint_ = analysis.sample->pcmFingerprint_;
                prepared.emplace(analysis.sample.get(), std::move(plain));
            }
            else if (analysis.sample->spectralFrames().size() == static_cast<size_t>(allowed)) prepared.emplace(analysis.sample.get(), analysis.sample);
            else
            {
                const auto loaded = OscillatorSample::withSpectralAnalysis(analysis.sample, allowed);
                if (! loaded) return mediaFailure(loaded.error);
                prepared.emplace(analysis.sample.get(), loaded.data);
            }
        }
        size_t spectralBytes = 0;
        double maximumHop = 0;
        for (const auto& [source, target] : prepared)
        {
            static_cast<void>(source);
            spectralBytes += target->spectralFrames().size() * sizeof(OscillatorSample::SpectralFrame);
            maximumHop = std::max(maximumHop, target->spectralHopSeconds());
        }
        for (auto& zone : zones) if (const auto found = prepared.find(zone.sample.get()); found != prepared.end()) zone.sample = found->second;
        auto data = std::shared_ptr<OscillatorMedia>(new OscillatorMedia);
        data->zones_ = std::move(zones);
        data->pcmBytes_ = bytes; data->spectralBytes_ = spectralBytes; data->maximumSpectralHopSeconds_ = maximumHop;
        data->name_ = name.empty() ? "Oscillator Media" : name.substr(0, 1024);
        return {std::move(data), {}, std::move(warnings)};
    }
    catch (const std::bad_alloc&) { return mediaFailure("Unable to allocate oscillator zones"); }
    catch (const std::length_error&) { return mediaFailure("Oscillator zone allocation length exceeded"); }
}

namespace
{
using Opcodes = std::map<std::string, std::string, std::less<>>;
bool whitespace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
bool identifier(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
bool identifierTail(char c) { return identifier(c) || (c >= '0' && c <= '9'); }
std::string_view trim(std::string_view s)
{
    while (! s.empty() && whitespace(s.front())) s.remove_prefix(1);
    while (! s.empty() && whitespace(s.back())) s.remove_suffix(1);
    return s;
}
bool integer(std::string_view value, int64_t& out)
{
    value = trim(value);
    if (! value.empty() && value.front() == '+') value.remove_prefix(1);
    if (value.empty()) return false;
    const auto result = std::from_chars(value.data(), value.data() + value.size(), out);
    return result.ec == std::errc{} && result.ptr == value.data() + value.size();
}
bool noteNumber(std::string_view value, int& out)
{
    int64_t number = 0;
    if (integer(value, number))
    {
        if (number < 0 || number > 127) return false;
        out = static_cast<int>(number);
        return true;
    }
    value = trim(value);
    if (value.size() < 2) return false;
    char letter = value.front();
    if (letter >= 'A' && letter <= 'G') letter = static_cast<char>(letter - 'A' + 'a');
    static constexpr int pitch[]{9, 11, 0, 2, 4, 5, 7};
    if (letter < 'a' || letter > 'g') return false;
    int semitone = pitch[static_cast<size_t>(letter - 'a')];
    value.remove_prefix(1);
    if (! value.empty() && (value.front() == '#' || value.front() == 'b'))
    { semitone += value.front() == '#' ? 1 : -1; value.remove_prefix(1); }
    int64_t octave = 0;
    if (! integer(value, octave) || octave < -1 || octave > 9) return false;
    const int64_t key = (octave + 1) * 12 + semitone; // SFZ convention: C4 is MIDI60.
    if (key < 0 || key > 127) return false;
    out = static_cast<int>(key);
    return true;
}

class SfzLoader
{
public:
    SfzLoader(std::string_view text, const juce::File& directory)
        : text_(text), directory_(directory) {}
    bool read()
    {
        if (! locale_.valid()) return fail("Unable to create SFZ numeric locale");
        while (skip())
        {
            if (position_ >= text_.size()) break;
            if (text_[position_] == '<')
            {
                const auto end = text_.find('>', position_ + 1);
                if (end == std::string_view::npos) return fail("Truncated SFZ header");
                finalize();
                if (! error.empty()) return false;
                const auto tag = trim(text_.substr(position_ + 1, end - position_ - 1));
                position_ = end + 1;
                if (tag == "control") { scope_ = Scope::Control; control_.clear(); }
                else if (tag == "global") { scope_ = Scope::Global; global_.clear(); group_.clear(); }
                else if (tag == "group") { scope_ = Scope::Group; group_.clear(); }
                else if (tag == "region") { scope_ = Scope::Region; region_.clear(); }
                else { scope_ = Scope::Ignored; warn("Unsupported SFZ header: " + std::string(tag)); }
                continue;
            }
            if (text_[position_] == '#')
            {
                const auto end = text_.find('\n', position_);
                warn("SFZ preprocessor directives are unsupported");
                position_ = end == std::string_view::npos ? text_.size() : end + 1;
                continue;
            }
            if (! identifier(text_[position_])) return fail("Invalid SFZ opcode at byte " + std::to_string(position_));
            const size_t start = position_++;
            while (position_ < text_.size() && identifierTail(text_[position_])) ++position_;
            const std::string key(text_.substr(start, position_ - start));
            while (position_ < text_.size() && whitespace(text_[position_])) ++position_;
            if (position_ == text_.size() || text_[position_++] != '=') return fail("SFZ opcode requires '='");
            while (position_ < text_.size() && whitespace(text_[position_])) ++position_;
            std::string value;
            if (position_ < text_.size() && (text_[position_] == '"' || text_[position_] == '\''))
            {
                const char quote = text_[position_++];
                const size_t first = position_;
                while (position_ < text_.size() && text_[position_] != quote) ++position_;
                if (position_ == text_.size()) return fail("Unterminated SFZ quoted value");
                value = std::string(text_.substr(first, position_ - first));
                ++position_;
            }
            else
            {
                const size_t first = position_;
                while (position_ < text_.size())
                {
                    // A double separator inside an unquoted sample path is part of the path.
                    // SFZ producers may emit it; a comment after whitespace still terminates the value.
                    const bool pathValue = key == "sample" || key == "default_path";
                    const bool valueComment = comment(position_) && (! pathValue
                        || (position_ > first && whitespace(text_[position_ - 1])));
                    if (text_[position_] == '<' || valueComment) break;
                    if (whitespace(text_[position_]))
                    {
                        size_t next = position_;
                        while (next < text_.size() && whitespace(text_[next])) ++next;
                        if (next == text_.size() || text_[next] == '<' || text_[next] == '#' || comment(next) || opcodeAt(next)) break;
                    }
                    ++position_;
                }
                value = std::string(trim(text_.substr(first, position_ - first)));
            }
            if (++opcodes_ > 65536) return fail("SFZ opcode count limit exceeded");
            switch (scope_)
            {
                case Scope::Control: control_[key] = std::move(value); break;
                case Scope::Global: global_[key] = std::move(value); break;
                case Scope::Group: group_[key] = std::move(value); break;
                case Scope::Region: region_[key] = std::move(value); break;
                case Scope::Ignored: break;
            }
        }
        if (! error.empty()) return false;
        finalize();
        return error.empty();
    }
    std::vector<OscillatorZone> zones;
    std::vector<std::string> warnings;
    std::string error;
private:
    enum class Scope { Control, Global, Group, Region, Ignored };
    bool fail(std::string message) { if (error.empty()) error = std::move(message); return false; }
    void warn(std::string message)
    {
        if (warnings.size() >= 128 || ! warningSet_.insert(message).second) return;
        warnings.push_back(std::move(message));
    }
    bool comment(size_t position) const
    {
        return position + 1 < text_.size() && text_[position] == '/' && (text_[position + 1] == '/' || text_[position + 1] == '*');
    }
    bool opcodeAt(size_t position) const
    {
        if (position >= text_.size() || ! identifier(text_[position])) return false;
        ++position;
        while (position < text_.size() && identifierTail(text_[position])) ++position;
        while (position < text_.size() && whitespace(text_[position])) ++position;
        return position < text_.size() && text_[position] == '=';
    }
    bool skip()
    {
        for (;;)
        {
            while (position_ < text_.size() && whitespace(text_[position_])) ++position_;
            if (! comment(position_)) return true;
            if (text_[position_ + 1] == '/')
            {
                const auto end = text_.find('\n', position_ + 2);
                position_ = end == std::string_view::npos ? text_.size() : end + 1;
            }
            else
            {
                const auto end = text_.find("*/", position_ + 2);
                if (end == std::string_view::npos) return fail("Unterminated SFZ comment");
                position_ = end + 2;
            }
        }
    }
    bool real(std::string_view value, double& result) const
    {
        value = trim(value);
        if (value.empty()) return false;
        for (const char c : value) if ((c < '0' || c > '9') && c != '-' && c != '+' && c != '.' && c != 'e' && c != 'E') return false;
        const std::string token(value);
        char* end = nullptr;
        errno = 0;
        result = locale_.strtod(token.c_str(), &end);
        return errno != ERANGE && end == token.c_str() + token.size() && std::isfinite(result);
    }
    void finalize()
    {
        if (scope_ != Scope::Region) return;
        scope_ = Scope::Ignored;
        if (++regions_ > OscillatorMedia::maxZones) { fail("SFZ zone count limit exceeded"); return; }
        Opcodes all;
        const auto apply = [&all](const Opcodes& source)
        {
            if (const auto key = source.find("key"); key != source.end())
            { all["lokey"] = key->second; all["hikey"] = key->second; all["pitch_keycenter"] = key->second; }
            for (const auto& [key, value] : source) if (key != "key") all[key] = value;
        };
        apply(global_); apply(group_); apply(region_);
        const auto sample = all.find("sample");
        if (sample == all.end() || sample->second.empty()) { warn("SFZ region has no sample"); return; }
        std::string relative = sample->second;
        if (! juce::File::isAbsolutePath(juce::String::fromUTF8(relative.data(), static_cast<int>(relative.size()))))
            relative = (control_.contains("default_path") ? control_["default_path"] : "") + relative;
        std::replace(relative.begin(), relative.end(), '\\', '/');
        const juce::File path = directory_.getChildFile(juce::String::fromUTF8(relative.data(), static_cast<int>(relative.size())));
        const auto pathString = path.getFullPathName().toStdString();
        auto cached = cache_.find(pathString);
        if (cached == cache_.end())
        {
            const auto loaded = OscillatorSample::fromFile(path, false);
            if (! loaded) { warn("SFZ sample unavailable: " + sample->second + " (" + loaded.error + ")"); return; }
            if (loaded.data->storageBytes() > OscillatorMedia::maxTotalPcmBytes - loadedBytes_)
            { fail("SFZ PCM bank exceeds the 768 MiB limit"); return; }
            loadedBytes_ += loaded.data->storageBytes();
            cached = cache_.emplace(pathString, loaded.data).first;
        }
        OscillatorZone zone;
        zone.sample = cached->second;
        zone.end = zone.sample->sampleFrames();
        zone.loopEnd = zone.end;
        bool valid = true;
        for (const auto& [key, value] : all)
        {
            double numeric = 0;
            int64_t whole = 0;
            if (key == "sample") continue;
            if (key == "key" || key == "lokey" || key == "hikey" || key == "pitch_keycenter")
            {
                int note = 0;
                if (! noteNumber(value, note)) { valid = false; break; }
                if (key == "key") { zone.lowKey = note; zone.highKey = note; zone.rootKey = note; }
                else if (key == "lokey") zone.lowKey = note;
                else if (key == "hikey") zone.highKey = note;
                else zone.rootKey = note;
            }
            else if (key == "lovel" || key == "hivel" || key == "offset" || key == "end" || key == "loop_start" || key == "loop_end")
            {
                if (! integer(value, whole) || whole < 0 || whole > 0x7fffffff) { valid = false; break; }
                if (key == "lovel") zone.lowVelocity = static_cast<int>(whole);
                else if (key == "hivel") zone.highVelocity = static_cast<int>(whole);
                else if (key == "offset") zone.offset = whole;
                else if (key == "end") zone.end = whole + 1; // SFZ endpoint is inclusive.
                else if (key == "loop_start") zone.loopStart = whole;
                else zone.loopEnd = whole + 1;
            }
            else if (key == "tune" || key == "transpose" || key == "volume" || key == "pan" || key == "amp_veltrack" || key == "pitch_keytrack"
                     || key == "ampeg_attack" || key == "ampeg_release" || key == "loop_crossfade")
            {
                if (! real(value, numeric)) { valid = false; break; }
                if (key == "tune") zone.tuneCents += static_cast<float>(numeric);
                else if (key == "transpose") zone.tuneCents += static_cast<float>(numeric * 100);
                else if (key == "volume") zone.gainDb = static_cast<float>(numeric);
                else if (key == "pan") zone.pan = static_cast<float>(numeric * 0.01);
                else if (key == "amp_veltrack") zone.velocityTracking = static_cast<float>(numeric * 0.01);
                else if (key == "pitch_keytrack") zone.keyTracking = static_cast<float>(numeric * 0.01);
                else if (key == "ampeg_attack") zone.attackSeconds = numeric;
                else if (key == "ampeg_release") zone.releaseSeconds = numeric;
                else
                {
                    if (numeric < 0 || numeric > 32) { valid = false; break; }
                    zone.loopCrossfade = static_cast<int64_t>(numeric * zone.sample->sampleRate());
                }
            }
            else if (key == "loop_mode")
            {
                if (value == "no_loop") zone.loop = MediaLoop::None;
                else if (value == "one_shot") zone.loop = MediaLoop::OneShot;
                else if (value == "loop_continuous") zone.loop = MediaLoop::Forward;
                else if (value == "loop_sustain") zone.loop = MediaLoop::Sustain;
                else if (value == "pingpong" || value == "loop_pingpong") zone.loop = MediaLoop::PingPong;
                else { warn("Unsupported SFZ loop mode: " + value); valid = false; break; }
            }
            else if (key == "loop_type")
            {
                if (value == "alternate" && (zone.loop == MediaLoop::Forward || zone.loop == MediaLoop::Sustain))
                {
                    zone.loopWhileHeld = zone.loop == MediaLoop::Sustain;
                    zone.loop = MediaLoop::PingPong;
                }
                else if (value != "forward" && value != "alternate") warn("Unsupported SFZ loop type: " + value);
            }
            else warn("Unsupported SFZ opcode: " + key);
        }
        if (! all.contains("loop_start")) zone.loopStart = zone.offset;
        if (! all.contains("loop_end")) zone.loopEnd = zone.end;
        // Common exporters (including the factory multisample banks) write end/loop_end as a frame count rather than the
        // inclusive last index; like other SFZ players, clamp endpoints that run past the file to its length.
        const int64_t frames = zone.sample->sampleFrames();
        if (zone.end > frames) zone.end = frames;
        if (zone.loopEnd > zone.end) zone.loopEnd = zone.end;
        const auto checked = valid ? OscillatorMedia::fromZones({zone}) : mediaFailure("Invalid SFZ numeric/range value");
        if (! checked) { warn("Skipped SFZ region: " + checked.error); return; }
        zones.push_back(checked.data->zones().front());
    }
    std::string_view text_;
    const juce::File& directory_;
    platform::CNumericLocale locale_;
    size_t position_ = 0, opcodes_ = 0, loadedBytes_ = 0;
    int regions_ = 0;
    Scope scope_ = Scope::Ignored;
    Opcodes control_, global_, group_, region_;
    std::map<std::string, std::shared_ptr<const OscillatorSample>, std::less<>> cache_;
    std::set<std::string> warningSet_;
};
}

OscillatorMediaResult OscillatorMedia::fromSfz(const juce::File& file, bool spectral)
{
    if (! file.existsAsFile()) return mediaFailure("SFZ file does not exist");
    const auto length = file.getSize();
    if (length <= 0 || static_cast<uint64_t>(length) > maxSfzBytes) return mediaFailure("SFZ file exceeds the 1 MiB text limit");
    try
    {
        juce::FileInputStream stream(file);
        if (! stream.openedOk()) return mediaFailure("Unable to open SFZ file");
        std::vector<char> bytes(static_cast<size_t>(length) + 1);
        if (stream.read(bytes.data(), static_cast<int>(bytes.size())) != length)
            return mediaFailure("Unable to read complete bounded SFZ file");
        return fromSfzText(std::string_view(bytes.data(), static_cast<size_t>(length)), file.getParentDirectory(),
                           file.getFileNameWithoutExtension().toStdString(), spectral);
    }
    catch (const std::bad_alloc&) { return mediaFailure("Unable to allocate SFZ file text"); }
    catch (const std::length_error&) { return mediaFailure("SFZ file allocation length exceeded"); }
}
OscillatorMediaResult OscillatorMedia::fromSfzText(std::string_view text, const juce::File& baseDirectory, std::string name, bool spectral)
{
    if (text.empty() || text.size() > maxSfzBytes) return mediaFailure("SFZ text exceeds the 1 MiB limit or is empty");
    if (text.find('\0') != std::string_view::npos || ! juce::CharPointer_UTF8::isValidString(text.data(), static_cast<int>(text.size())))
        return mediaFailure("Invalid UTF-8 or embedded NUL in SFZ text");
    if (text.starts_with("\xef\xbb\xbf")) text.remove_prefix(3);
    try
    {
        SfzLoader loader(text, baseDirectory);
        const bool complete = loader.read();
        if (! complete)
        {
            if (loader.warnings.size() == 128) loader.warnings.back() = loader.error;
            else loader.warnings.push_back(loader.error);
        }
        if (loader.zones.empty()) return {nullptr, complete ? "SFZ contains no playable regions" : loader.error, std::move(loader.warnings)};
        auto result = fromZones(std::move(loader.zones), std::move(name), spectral);
        result.warnings.insert(result.warnings.end(), loader.warnings.begin(), loader.warnings.end());
        return result;
    }
    catch (const std::bad_alloc&) { return mediaFailure("Unable to allocate SFZ media"); }
    catch (const std::length_error&) { return mediaFailure("SFZ allocation length exceeded"); }
}

namespace
{
void movePosition(double& position, int& direction, bool& engaged, bool& active,
                  const OscillatorZone& zone, double increment, bool released, MediaLoop loop, bool loopWhileHeld)
{
    const bool looped = loop == MediaLoop::Forward
        || (loop == MediaLoop::PingPong && (! loopWhileHeld || ! released))
        || (loop == MediaLoop::Sustain && ! released);
    const double first = static_cast<double>(std::max(zone.offset, zone.loopStart)), last = static_cast<double>(zone.loopEnd - 1);
    if (! looped)
    {
        position += increment * direction;
        active = position >= static_cast<double>(zone.offset) && position < static_cast<double>(zone.end);
        return;
    }
    if (! engaged)
    {
        position += increment * direction;
        if (position >= first && position <= last) engaged = true;
        else if ((increment >= 0 && position > last && position - increment < first)
                 || (increment < 0 && position < first && position - increment > last)) engaged = true;
        if (! engaged)
        {
            active = position >= static_cast<double>(zone.offset) && position < static_cast<double>(zone.end);
            return;
        }
        // Position was already advanced on entering the loop.
        increment = 0;
    }
    if (loop == MediaLoop::PingPong)
    {
        const double length = last - first;
        if (length <= 0) { position = first; return; }
        const double advanced = position + increment * direction;
        if (advanced >= first && advanced <= last) { position = advanced; return; }
        const double period = 2 * length;
        double travel = direction >= 0 ? position - first : period - (position - first);
        travel = std::fmod(travel + increment, period);
        if (travel < 0) travel += period;
        if (travel < length) { position = first + travel; direction = 1; }
        else { position = first + period - travel; direction = -1; }
    }
    else
    {
        const double length = static_cast<double>(zone.loopEnd) - first;
        const double advanced = position + increment * direction;
        if (advanced >= first && advanced < static_cast<double>(zone.loopEnd)) { position = advanced; return; }
        double travel = std::fmod(advanced - first, length);
        if (travel < 0) travel += length;
        position = first + travel;
    }
}
float loopedSample(const OscillatorZone& zone, double position, int channel, bool released, MediaLoop loop)
{
    float sample = zone.sample->sampleLinear(position, channel);
    const bool forward = loop == MediaLoop::Forward || (loop == MediaLoop::Sustain && ! released);
    if (forward && position >= static_cast<double>(zone.loopEnd - 1) && position < static_cast<double>(zone.loopEnd))
    {
        const float end = zone.sample->sampleLinear(static_cast<double>(zone.loopEnd - 1), channel);
        const float start = zone.sample->sampleLinear(static_cast<double>(std::max(zone.offset, zone.loopStart)), channel);
        sample = end + (start - end) * static_cast<float>(position - static_cast<double>(zone.loopEnd - 1));
    }
    if (zone.loopCrossfade > 0 && forward)
    {
        const double fade = static_cast<double>(zone.loopCrossfade), end = static_cast<double>(zone.loopEnd);
        if (position >= end - fade && position < end)
        {
            const float mix = static_cast<float>((position - (end - fade)) / fade);
            const float beginning = zone.sample->sampleLinear(static_cast<double>(std::max(zone.offset, zone.loopStart)) + position - (end - fade), channel);
            sample += (beginning - sample) * mix;
        }
    }
    return sample;
}
}

void OscillatorMediaVoice::reset()
{
    media_ = nullptr; layers_ = {}; grains_ = {};
    layerCount_ = 0; nextGrainLayer_ = 0; spawnCountdown_ = 0;
    released_ = false; firstGrain_ = true; seed_ = 1;
}
void OscillatorMediaVoice::start(const OscillatorMedia* media, Mode mode, int note, int velocity, double outputSampleRate, uint32_t seed)
{
    reset();
    if (media == nullptr || note < 0 || note > 127 || velocity < 1 || velocity > 127) return;
    media_ = media; mode_ = mode; seed_ = seed;
    outputRate_ = bounded(outputSampleRate, 1, 768000, 44100);
    for (const auto& zone : media->zones())
    {
        if (note < zone.lowKey || note > zone.highKey || velocity < zone.lowVelocity || velocity > zone.highVelocity) continue;
        if (layerCount_ == maxLayers) break;
        auto& layer = layers_[static_cast<size_t>(layerCount_++)];
        layer.zone = &zone;
        layer.loop = zone.loop; layer.loopWhileHeld = zone.loopWhileHeld;
        layer.active = true;
        layer.spectral = mode == Mode::Spectral && zone.sample->hasSpectralAnalysis();
        layer.position = static_cast<double>(zone.offset);
        layer.basePitch = std::exp2((static_cast<double>(note - zone.rootKey) * zone.keyTracking + zone.tuneCents * 0.01) / 12);
        layer.baseStep = zone.sample->sampleRate() / outputRate_ * layer.basePitch;
        layer.gain = std::pow(10.0, zone.gainDb / 20.0);
        // The independently implemented SFZ response interpolates between unity and the default squared velocity curve.
        const double relativeVelocity = static_cast<double>(velocity) / 127;
        layer.velocityGain = 1 - zone.velocityTracking + zone.velocityTracking * relativeVelocity * relativeVelocity;
        layer.relativeVelocity = relativeVelocity;
        layer.envelope = zone.attackSeconds <= 0 ? 1 : 0;
        if (layer.spectral && layer.active)
        {
            const auto& frame = zone.sample->spectralFrames()[static_cast<size_t>(std::min(static_cast<int>(layer.position / zone.sample->spectralHop()), static_cast<int>(zone.sample->spectralFrames().size()) - 1))];
            for (size_t partial = 0; partial < layer.partial.size(); ++partial)
            {
                layer.partial[partial].cosine = std::cos(frame.partial[partial].phaseLeft);
                layer.partial[partial].sine = std::sin(frame.partial[partial].phaseLeft);
            }
        }
    }
}
void OscillatorMediaVoice::release() { released_ = true; }
void OscillatorMediaVoice::seek(double normalizedPosition)
{
    const double position = bounded(normalizedPosition, 0, 1, 0);
    for (int l = 0; l < layerCount_; ++l)
    {
        auto& layer = layers_[static_cast<size_t>(l)];
        if (layer.zone == nullptr) continue;
        layer.position = static_cast<double>(layer.zone->offset) + position * static_cast<double>(layer.zone->end - layer.zone->offset - 1);
        layer.loopEngaged = false; layer.direction = 1; layer.spectralCountdown = 0;
        layer.played = 1; // An explicit seek takes priority over automatic reverse-at-note-start.
    }
    grains_ = {}; spawnCountdown_ = 0; firstGrain_ = true;
}
bool OscillatorMediaVoice::active() const { return activeLayers() > 0 || activeGrains() > 0; }
int OscillatorMediaVoice::activeLayers() const
{
    int count = 0;
    for (const auto& layer : layers_) if (layer.active) ++count;
    return count;
}
int OscillatorMediaVoice::activeGrains() const
{
    int count = 0;
    for (const auto& grain : grains_) if (grain.active) ++count;
    return count;
}
double OscillatorMediaVoice::position() const
{
    if (layerCount_ == 0 || layers_[0].zone == nullptr) return 0;
    const auto& layer = layers_[0];
    return std::clamp((layer.position - static_cast<double>(layer.zone->offset)) / static_cast<double>(std::max<int64_t>(1, layer.zone->end - layer.zone->offset)), 0.0, 1.0);
}
float OscillatorMediaVoice::randomSigned(uint32_t& seed)
{
    seed = seed * 1664525u + 1013904223u;
    return static_cast<float>(seed >> 8) / 8388608.0f - 1;
}
void OscillatorMediaVoice::advance(Layer& layer, double increment)
{
    movePosition(layer.position, layer.direction, layer.loopEngaged, layer.active, *layer.zone, increment, released_, layer.loop, layer.loopWhileHeld);
}
void OscillatorMediaVoice::spawnGrain(const Controls& controls, int layer, double initialAge)
{
    if (layer < 0 || layer >= layerCount_ || ! layers_[static_cast<size_t>(layer)].active || activeGrains() >= controls.grainLimit) return;
    auto available = std::find_if(grains_.begin(), grains_.end(), [](const Grain& grain) { return ! grain.active; });
    if (available == grains_.end()) return;
    const auto& source = layers_[static_cast<size_t>(layer)];
    const auto& zone = *source.zone;
    *available = Grain{};
    available->active = true; available->layer = layer;
    const double offset = randomSigned(seed_) * controls.positionJitter * static_cast<double>(zone.end - zone.offset);
    available->position = std::clamp(source.position + offset, static_cast<double>(zone.offset), static_cast<double>(zone.end - 1));
    available->baseStep = source.baseStep * std::exp2(randomSigned(seed_) * controls.pitchJitter / 12);
    const float directionChance = 0.5f + 0.5f * randomSigned(seed_);
    available->direction = (controls.reverse ? -1 : 1) * (directionChance < controls.directionJitter ? -1 : 1);
    available->duration = std::max(2.0, controls.grainSeconds * outputRate_ * (1 + randomSigned(seed_) * controls.lengthJitter));
    available->elapsed = std::min(initialAge, available->duration - 1);
    // Jump-start grains represent grains which were already playing before this first output sample.
    // Advance their read position as well as their window age, using the same bounded loop path.
    if (available->elapsed > 0)
        movePosition(available->position, available->direction, available->loopEngaged, available->active,
                     zone, available->elapsed * available->baseStep * std::exp2(controls.pitchSemitones / 12), released_, source.loop, source.loopWhileHeld);
    available->pan = static_cast<float>(bounded(zone.pan + randomSigned(seed_) * controls.panJitter, -1, 1, 0));
    available->gain = std::max(0.0f, 1 + randomSigned(seed_) * controls.gainJitter);
}
void OscillatorMediaVoice::updateSpectral(Layer& layer, double pitchScale)
{
    const auto& sample = *layer.zone->sample;
    const auto& frames = sample.spectralFrames();
    if (frames.empty()) { layer.active = false; return; }
    const double frame = std::clamp(layer.position / sample.spectralHop(), 0.0, static_cast<double>(frames.size() - 1));
    const auto first = static_cast<size_t>(frame), second = std::min(first + 1, frames.size() - 1);
    const float mix = static_cast<float>(frame - static_cast<double>(first));
    layer.spectralCount = 0;
    for (size_t p = 0; p < layer.partial.size(); ++p)
    {
        auto& target = layer.partial[p];
        const auto& a = frames[first].partial[p];
        const auto& b = frames[second].partial[p];
        const double frequency = (a.frequency + (b.frequency - a.frequency) * mix) * layer.basePitch * pitchScale;
        const double angle = tau * frequency / outputRate_;
        target.stepCosine = std::cos(angle); target.stepSine = std::sin(angle);
        const bool audible = frequency >= 0 && frequency < outputRate_ * 0.45;
        target.left = audible ? a.amplitudeLeft + (b.amplitudeLeft - a.amplitudeLeft) * mix : 0;
        target.right = audible ? a.amplitudeRight + (b.amplitudeRight - a.amplitudeRight) * mix : 0;
        if (std::abs(target.left) + std::abs(target.right) > 1e-12f) layer.spectralCount = static_cast<int>(p) + 1;
        const double phaseA = a.phaseRight - a.phaseLeft, phaseB = b.phaseRight - b.phaseLeft;
        const double difference = std::remainder(phaseB - phaseA, tau);
        const double relativePhase = phaseA + difference * mix;
        target.rightCosine = static_cast<float>(std::cos(relativePhase));
        target.rightSine = static_cast<float>(std::sin(relativePhase));
    }
    layer.spectralCountdown = 16;
}

OscillatorMediaVoice::Stereo OscillatorMediaVoice::next(const Controls& raw)
{
    if (media_ == nullptr) return {};
    Controls controls = raw;
    controls.pitchSemitones = bounded(controls.pitchSemitones, -96, 96, 0);
    controls.timeRate = bounded(controls.timeRate, -8, 8, 0);
    controls.grainSeconds = bounded(controls.grainSeconds, 0.001, 8, 0.05);
    controls.density = bounded(controls.density, 0.1, 1000, 8);
    controls.tempo = bounded(controls.tempo, 1, 1000, 120);
    controls.densityBeats = bounded(controls.densityBeats, 0.015625, 64, 0.25);
    controls.grainLimit = std::clamp(controls.grainLimit, 1, maxGrains);
    controls.positionJitter = static_cast<float>(bounded(controls.positionJitter, 0, 1, 0));
    controls.pitchJitter = static_cast<float>(bounded(controls.pitchJitter, 0, 24, 0));
    controls.panJitter = static_cast<float>(bounded(controls.panJitter, 0, 1, 0));
    controls.directionJitter = static_cast<float>(bounded(controls.directionJitter, 0, 1, 0));
    controls.lengthJitter = static_cast<float>(bounded(controls.lengthJitter, 0, 0.95, 0));
    controls.gainJitter = static_cast<float>(bounded(controls.gainJitter, 0, 1, 0));
    controls.windowAmount = static_cast<float>(bounded(controls.windowAmount, 0, 1, 1));
    controls.windowSkew = static_cast<float>(bounded(controls.windowSkew, -0.95, 0.95, 0));
    const double pitchScale = std::exp2(controls.pitchSemitones / 12);
    for (int l = 0; l < layerCount_; ++l)
    {
        auto& layer = layers_[static_cast<size_t>(l)];
        if (! layer.active) continue;
        MediaLoop loop = layer.zone->loop;
        bool held = layer.zone->loopWhileHeld;
        switch (controls.loopOverride)
        {
            case LoopOverride::Asset: break;
            case LoopOverride::None: loop = MediaLoop::None; held = false; break;
            case LoopOverride::Forward: loop = MediaLoop::Forward; held = false; break;
            case LoopOverride::Sustain: loop = MediaLoop::Sustain; held = false; break;
            case LoopOverride::PingPong: loop = MediaLoop::PingPong; held = false; break;
        }
        if (controls.oneShot) { loop = MediaLoop::OneShot; held = false; }
        if (loop != layer.loop || held != layer.loopWhileHeld) layer.loopEngaged = false;
        layer.loop = loop; layer.loopWhileHeld = held;
        if (layer.played == 0 && controls.reverse) layer.position = static_cast<double>(layer.zone->end - 1);
        const double releaseSeconds = controls.releaseOverride >= 0 ? controls.releaseOverride : layer.zone->releaseSeconds;
        const double attackSeconds = controls.attackOverride >= 0 ? controls.attackOverride : layer.zone->attackSeconds;
        if (released_ && layer.loop != MediaLoop::OneShot)
        {
            if (controls.serumProfile && releaseSeconds > 0)
            {
                // -80 dB over the release time, then silent.
                layer.envelope *= std::exp(std::log(1e-4) / (releaseSeconds * outputRate_));
                if (layer.envelope < 1e-4) layer.envelope = 0;
            }
            else layer.envelope = std::max(0.0, layer.envelope - (releaseSeconds <= 0 ? 1 : 1 / (releaseSeconds * outputRate_)));
        }
        else if (layer.envelope < 1)
            layer.envelope = std::min(1.0, layer.envelope + (attackSeconds <= 0 ? 1 : 1 / (attackSeconds * outputRate_)));
        if (released_ && layer.envelope <= 0 && layer.loop != MediaLoop::OneShot)
        {
            layer.active = false;
            for (auto& grain : grains_) if (grain.layer == l) grain.active = false;
        }
    }
    Stereo result;
    if (mode_ == Mode::Granular)
    {
        const double hertz = controls.densityMode == DensityMode::GrainCount ? std::min<double>(controls.grainLimit, controls.density) / controls.grainSeconds
            : controls.densityMode == DensityMode::Tempo ? controls.tempo / (60 * controls.densityBeats) : controls.density;
        const double interval = std::max(1.0, outputRate_ / std::max(0.1, hertz));
        if (firstGrain_)
        {
            const int count = controls.jumpStart ? std::clamp(static_cast<int>(std::ceil(hertz * controls.grainSeconds)), 1, controls.grainLimit) : 1;
            for (int i = 0; i < count; ++i)
            {
                if (layerCount_ > 0)
                {
                    spawnGrain(controls, nextGrainLayer_, controls.jumpStart ? controls.grainSeconds * outputRate_ * i / count : 0);
                    nextGrainLayer_ = (nextGrainLayer_ + 1) % layerCount_;
                }
            }
            spawnCountdown_ = interval; firstGrain_ = false;
        }
        else if (spawnCountdown_ <= 0 && layerCount_ > 0)
        {
            spawnGrain(controls, nextGrainLayer_);
            nextGrainLayer_ = (nextGrainLayer_ + 1) % layerCount_;
            spawnCountdown_ += interval;
        }
        --spawnCountdown_;
        float weightSum = 0;
        for (auto& grain : grains_)
        {
            if (! grain.active) continue;
            const auto& layer = layers_[static_cast<size_t>(grain.layer)];
            const auto& zone = *layer.zone;
            double phase = std::clamp(grain.elapsed / grain.duration, 0.0, 1.0);
            const double skew = 0.5 + controls.windowSkew * 0.49;
            phase = phase < skew ? 0.5 * phase / skew : 0.5 + 0.5 * (phase - skew) / (1 - skew);
            float window = controls.window == Window::Rectangle ? 1 : controls.window == Window::Triangle ? static_cast<float>(1 - std::abs(2 * phase - 1))
                : hann(phase);
            window = 1 - controls.windowAmount + controls.windowAmount * window;
            const float gain = static_cast<float>(layer.gain * layer.envelope * (controls.applyVelocity ? layer.velocityGain : 1)) * grain.gain * window;
            const float left = loopedSample(zone, grain.position, 0, released_, layer.loop);
            const float right = zone.sample->channels() == 1 ? left : loopedSample(zone, grain.position, 1, released_, layer.loop);
            result.left += left * gain * balanceLeft(grain.pan);
            result.right += right * gain * balanceRight(grain.pan);
            weightSum += window;
            movePosition(grain.position, grain.direction, grain.loopEngaged, grain.active, zone, grain.baseStep * pitchScale, released_, layer.loop, layer.loopWhileHeld);
            ++grain.elapsed;
            if (grain.elapsed >= grain.duration) grain.active = false;
        }
        const float normalization = 1 / std::max(1.0f, weightSum);
        result.left *= normalization; result.right *= normalization;
        for (int l = 0; l < layerCount_; ++l)
        {
            auto& layer = layers_[static_cast<size_t>(l)];
            if (layer.active) advance(layer, layer.zone->sample->sampleRate() / outputRate_ * controls.timeRate * (controls.reverse ? -1 : 1));
            ++layer.played;
        }
    }
    else
    {
        for (int l = 0; l < layerCount_; ++l)
        {
            auto& layer = layers_[static_cast<size_t>(l)];
            if (! layer.active) continue;
            const auto& zone = *layer.zone;
            Stereo sample;
            if (mode_ == Mode::Spectral && layer.spectral)
            {
                if (layer.spectralCountdown <= 0) updateSpectral(layer, pitchScale);
                --layer.spectralCountdown;
                for (int p = 0; p < layer.spectralCount; ++p)
                {
                    auto& partial = layer.partial[static_cast<size_t>(p)];
                    sample.left += partial.left * static_cast<float>(partial.cosine);
                    sample.right += partial.right * static_cast<float>(partial.cosine * partial.rightCosine - partial.sine * partial.rightSine);
                    const double cosine = partial.cosine * partial.stepCosine - partial.sine * partial.stepSine;
                    partial.sine = partial.sine * partial.stepCosine + partial.cosine * partial.stepSine;
                    partial.cosine = cosine;
                    if (layer.played % 1024 == 0)
                    {
                        const double norm = std::hypot(partial.cosine, partial.sine);
                        if (norm > 1e-12) { partial.cosine /= norm; partial.sine /= norm; }
                    }
                }
                advance(layer, zone.sample->sampleRate() / outputRate_ * controls.timeRate * (controls.reverse ? -1 : 1));
            }
            else
            {
                sample.left = loopedSample(zone, layer.position, 0, released_, layer.loop);
                sample.right = zone.sample->channels() == 1 ? sample.left : loopedSample(zone, layer.position, 1, released_, layer.loop);
                advance(layer, layer.baseStep * pitchScale * controls.timeRate * (controls.reverse ? -1 : 1));
            }
            const double velocityGain = controls.serumProfile ? std::pow(layer.relativeVelocity, 2 * zone.velocityTracking) : layer.velocityGain;
            const double profileGain = controls.serumProfile && mode_ == Mode::Multisample ? 2.840 : 1;
            const float gain = static_cast<float>(layer.gain * layer.envelope * profileGain * (controls.applyVelocity ? velocityGain : 1));
            result.left += sample.left * gain * balanceLeft(zone.pan); result.right += sample.right * gain * balanceRight(zone.pan);
            ++layer.played;
        }
    }
    if (! std::isfinite(result.left) || ! std::isfinite(result.right)) { reset(); return {}; }
    return result;
}

} // namespace sonora::devices

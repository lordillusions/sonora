#include "devices/native/WavetableContent.h"
#include "devices/native/WavetableData.h"
#include "devices/native/WavetableSynth.h"
#include "devices/native/OscillatorMedia.h"
#include "plugins/SerumPatch.h"

#include <juce_core/juce_core.h>
#include <bit>
#include <cmath>
#include <array>
#include <algorithm>
#include <set>
#include <map>
#include <charconv>

namespace sonora::devices
{
namespace
{
constexpr const char* format = "sonora-wavetable-v1";
juce::var object() { return juce::var(new juce::DynamicObject()); }
juce::var property(const juce::var& value, const juce::Identifier& key)
{
    const auto* o = value.getDynamicObject();
    return o != nullptr ? o->getProperty(key) : juce::var();
}
void set(juce::var& value, const juce::Identifier& key, const juce::var& entry)
{
    value.getDynamicObject()->setProperty(key, entry);
}
bool numeric(const juce::var& value)
{
    return (value.isDouble() || value.isInt() || value.isInt64()) && std::isfinite(static_cast<double>(value));
}
bool indexValue(const juce::var& value, int count, int& index)
{
    if (! numeric(value)) return false;
    const auto number = static_cast<double>(value);
    if (number < 0 || number >= count || std::floor(number) < number) return false;
    index = static_cast<int>(number); return true;
}
juce::String encode(const std::vector<uint8_t>& bytes)
{
    return juce::MemoryBlock(bytes.data(), bytes.size()).toBase64Encoding();
}
bool decode(const juce::var& value, std::vector<uint8_t>& bytes)
{
    if (! value.isString() || value.toString().length() > static_cast<int>(model::NativeContent::maxBytes * 2)) return false;
    juce::MemoryBlock data;
    if (! data.fromBase64Encoding(value.toString()) || data.getSize() > model::NativeContent::maxBytes) return false;
    const auto* begin = static_cast<const uint8_t*>(data.getData());
    if (data.getSize() == 0) bytes.clear();
    else bytes.assign(begin, begin + data.getSize());
    return true;
}
bool root(const model::Device& device, juce::var& value, std::string& error)
{
    if (! device.nativeContent)
    {
        value = object(); set(value, "version", 1);
        return true;
    }
    const auto& content = *device.nativeContent;
    if (content.data.size() > model::NativeContent::maxBytes
        || content.controllerData.size() > model::NativeContent::maxBytes - content.data.size())
    { error = "Native content exceeds its size limit"; return false; }
    if (content.format == format)
    {
        const auto text = juce::String::fromUTF8(reinterpret_cast<const char*>(content.data.data()), static_cast<int>(content.data.size()));
        const auto result = juce::JSON::parse(text, value);
        if (result.failed() || ! value.isObject() || property(value, "version") != juce::var(1))
        { error = "Invalid native wavetable content"; return false; }
        return true;
    }
    if (content.format != "serum2-xferjson" && content.format != "serum2-preset")
    { error = "Unsupported native content format: " + content.format; return false; }
    value = object(); set(value, "version", 1);
    auto origin = object();
    set(origin, "format", juce::String(content.format)); set(origin, "data", encode(content.data));
    set(value, "origin", origin);
    return true;
}
bool commit(model::Device& device, const juce::var& value, std::string& error)
{
    const auto encoded = juce::JSON::toString(value, true);
    const auto size = encoded.getNumBytesAsUTF8();
    if (size > model::NativeContent::maxBytes) { error = "Native content exceeds its size limit"; return false; }
    auto content = std::make_shared<model::NativeContent>();
    content->format = format;
    if (device.nativeContent)
    {
        content->sourceDirectory = device.nativeContent->sourceDirectory;
        content->controllerData = device.nativeContent->controllerData;
    }
    if (content->controllerData.size() > model::NativeContent::maxBytes - size)
    { error = "Native content exceeds its size limit"; return false; }
    const auto* bytes = reinterpret_cast<const uint8_t*>(encoded.toRawUTF8());
    content->data.assign(bytes, bytes + size);
    device.nativeContent = std::move(content); device.nativeContentEdited = true;
    return true;
}
void parameter(model::Device& device, const std::string& path, double number)
{
    for (auto& p : device.parameters) if (p.path == path) { p.value = number; return; }
    model::Parameter p; p.path = path; p.value = number;
    device.parameters.push_back(std::move(p));
}
void replaceEntry(juce::var& value, const char* key, int index, const juce::var& entry)
{
    juce::Array<juce::var> values;
    const auto existing = property(value, key);
    if (const auto* array = existing.getArray())
        for (const auto& item : *array)
            if (static_cast<int>(property(item, "index")) != index) values.add(item);
    values.add(entry); set(value, key, values);
}
bool curveFromValue(const juce::var& value, dsp::CurveGraph& curve)
{
    const auto points = property(value, "points");
    const auto* array = points.getArray();
    if (array == nullptr || array->size() < 2 || array->size() > dsp::CurveGraph::kMaxPoints) return false;
    curve = {}; curve.count = array->size();
    for (int i = 0; i < curve.count; ++i)
    {
        const auto& p = array->getReference(i);
        const auto x = property(p, "x"), y = property(p, "y"), tension = property(p, "tension");
        if (! numeric(x) || ! numeric(y) || ! numeric(tension)) return false;
        curve.x[i] = static_cast<float>(static_cast<double>(x));
        curve.y[i] = static_cast<float>(static_cast<double>(y));
        curve.curve[i] = static_cast<float>(static_cast<double>(tension));
    }
    return true;
}
bool validCurve(const dsp::CurveGraph& curve)
{
    if (curve.count < 2 || curve.count > dsp::CurveGraph::kMaxPoints) return false;
    if (std::abs(curve.x[0]) > 1e-6f || std::abs(curve.x[curve.count - 1] - 1) > 1e-6f) return false;
    for (int i = 0; i < curve.count; ++i)
        if (! std::isfinite(curve.x[i]) || ! std::isfinite(curve.y[i]) || ! std::isfinite(curve.curve[i])
            || curve.x[i] < 0 || curve.x[i] > 1 || curve.y[i] < 0 || curve.y[i] > 1 || std::abs(curve.curve[i]) > 1
            || (i > 0 && curve.x[i] < curve.x[i - 1])) return false;
    return true;
}
juce::String encodeSamples(const std::vector<float>& samples)
{
    std::vector<uint8_t> bytes;
    bytes.reserve(samples.size() * 4);
    for (const float sample : samples)
    {
        const auto bits = std::bit_cast<uint32_t>(sample);
        for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<uint8_t>(bits >> shift));
    }
    return encode(bytes);
}
bool decodeSamples(const juce::var& value, std::vector<float>& samples)
{
    std::vector<uint8_t> bytes;
    if (! decode(value, bytes) || bytes.empty() || bytes.size() % 4 != 0) return false;
    samples.resize(bytes.size() / 4);
    for (size_t i = 0; i < samples.size(); ++i)
    {
        uint32_t bits = 0;
        for (unsigned j = 0; j < 4; ++j) bits |= static_cast<uint32_t>(bytes[i * 4 + j]) << (8 * j);
        samples[i] = std::bit_cast<float>(bits);
    }
    return true;
}
bool numberField(const juce::var& value, const char* key, double& number)
{
    const auto field = property(value, key);
    if (! numeric(field)) return false;
    number = static_cast<double>(field); return true;
}
bool integerField(const juce::var& value, const char* key, int64_t& number)
{
    double field = 0;
    if (! numberField(value, key, field) || field < -1 || field > INT32_MAX || std::floor(field) < field) return false;
    number = static_cast<int64_t>(field); return true;
}
OscillatorMediaResult mediaFromValue(const juce::var& value)
{
    const auto pcm = property(value, "samples"), regions = property(value, "zones");
    const auto* samples = pcm.getArray(); const auto* zones = regions.getArray();
    if (! value.isObject() || samples == nullptr || samples->isEmpty() || samples->size() > OscillatorMedia::maxZones
        || zones == nullptr || zones->isEmpty() || zones->size() > OscillatorMedia::maxZones)
        return {nullptr, "Invalid native sample bank", {}};
    std::vector<std::shared_ptr<const OscillatorSample>> assets;
    std::map<std::string, std::shared_ptr<const OscillatorSample>> external;
    size_t totalBytes = 0, analysisBytes = 0;
    for (const auto& entry : *samples)
    {
        int channels = 0; double rate = 0; std::vector<float> decoded;
        const auto spectral = property(entry, "spectral");
        if (! entry.isObject() || ! indexValue(property(entry, "channels"), 3, channels) || channels == 0
            || ! numberField(entry, "sampleRate", rate) || ! spectral.isBool())
            return {nullptr, "Invalid native PCM sample", {}};
        int frameLimit = OscillatorSample::maxSpectralFrames;
        const auto limit = property(entry, "spectralFrameLimit");
        if (! limit.isVoid() && (! indexValue(limit, OscillatorSample::maxSpectralFrames + 1, frameLimit) || frameLimit < 2))
            return {nullptr, "Invalid native sample analysis limit", {}};
        OscillatorSampleResult loaded;
        const auto encoding = property(entry, "encoding").toString();
        const auto analysisFits = [&](size_t frames) {
            if (! static_cast<bool>(spectral)) return true;
            const auto count = std::min(static_cast<size_t>(frameLimit), (frames + 511) / 512 + 1);
            const auto bytes = count * sizeof(OscillatorSample::SpectralFrame);
            if (bytes > OscillatorMedia::maxSpectralBytes - analysisBytes) return false;
            analysisBytes += bytes; return true;
        };
        if (encoding == "audio-file")
        {
            const auto source = property(entry, "sourceFile");
            int64_t frames = 0;
            const auto fingerprint = property(entry, "pcmFingerprint").toString().toStdString();
            uint64_t expected = 0;
            const auto parsed = std::from_chars(fingerprint.data(), fingerprint.data() + fingerprint.size(), expected, 16);
            if (! source.isString() || source.toString().isEmpty() || ! juce::File::isAbsolutePath(source.toString())
                || ! integerField(entry, "frames", frames) || frames < 1
                || static_cast<uint64_t>(frames) > OscillatorSample::maxPcmBytes / sizeof(float) / static_cast<size_t>(channels)
                || property(entry, "fingerprintAlgorithm").toString() != "fnv1a64-f32le"
                || fingerprint.empty() || fingerprint.size() > 16 || parsed.ec != std::errc() || parsed.ptr != fingerprint.data() + fingerprint.size())
                return {nullptr, "Invalid native sample file reference", {}};
            const auto file = source.toString().toStdString();
            const auto cached = external.find(file);
            if (cached != external.end()) loaded.data = cached->second;
            else loaded = OscillatorSample::fromFile(juce::File(source.toString()));
            if (! loaded) return {nullptr, loaded.error, {}};
            if (loaded.data->channels() != channels || loaded.data->sampleFrames() != frames
                || std::abs(loaded.data->sampleRate() - rate) > 1e-6 || loaded.data->pcmFingerprint() != expected)
                return {nullptr, "Native sample file differs from the saved PCM reference: " + loaded.data->name(), {}};
            if (cached == external.end())
            {
                totalBytes += loaded.data->originalSamples().size() * sizeof(float);
                if (totalBytes > OscillatorMedia::maxTotalPcmBytes) return {nullptr, "Native sample bank exceeds its PCM limit", {}};
                external.emplace(file, loaded.data);
            }
            if (! analysisFits(static_cast<size_t>(loaded.data->sampleFrames())))
                return {nullptr, "Native sample bank exceeds its analysis limit", {}};
            if (static_cast<bool>(spectral)) loaded = OscillatorSample::withSpectralAnalysis(loaded.data, frameLimit);
        }
        else if (encoding == "juce-base64-f32le")
        {
            if (! decodeSamples(property(entry, "pcm"), decoded)) return {nullptr, "Invalid native PCM sample", {}};
            if (! analysisFits(decoded.size() / static_cast<size_t>(channels)))
                return {nullptr, "Native sample bank exceeds its analysis limit", {}};
            totalBytes += decoded.size() * sizeof(float);
            if (totalBytes > OscillatorMedia::maxTotalPcmBytes) return {nullptr, "Native sample bank exceeds its PCM limit", {}};
            loaded = OscillatorSample::fromSamples(decoded, channels, rate, property(entry, "name").toString().toStdString(), static_cast<bool>(spectral), frameLimit);
        }
        else return {nullptr, "Unsupported native PCM encoding", {}};
        if (! loaded) return {nullptr, loaded.error, {}};
        assets.push_back(std::move(loaded.data));
    }
    std::vector<OscillatorZone> mapped;
    for (const auto& entry : *zones)
    {
        OscillatorZone zone;
        int index = -1, loop = -1;
        if (! entry.isObject() || ! indexValue(property(entry, "sample"), static_cast<int>(assets.size()), index)
            || ! indexValue(property(entry, "loop"), 5, loop) || ! property(entry, "loopWhileHeld").isBool())
            return {nullptr, "Invalid native sample zone identity", {}};
        zone.sample = assets[static_cast<size_t>(index)]; zone.loop = static_cast<MediaLoop>(loop);
        zone.loopWhileHeld = static_cast<bool>(property(entry, "loopWhileHeld"));
        const auto smallInteger = [&](const char* key, int& result) {
            int64_t number = 0; if (! integerField(entry, key, number)) return false;
            result = static_cast<int>(number); return true;
        };
        const auto scalar = [&](const char* key, float& result) {
            double number = 0; if (! numberField(entry, key, number) || std::abs(number) > 1000000) return false;
            result = static_cast<float>(number); return true;
        };
        if (! smallInteger("lowKey", zone.lowKey) || ! smallInteger("highKey", zone.highKey)
            || ! smallInteger("lowVelocity", zone.lowVelocity) || ! smallInteger("highVelocity", zone.highVelocity)
            || ! smallInteger("rootKey", zone.rootKey) || ! scalar("tuneCents", zone.tuneCents)
            || ! scalar("gainDb", zone.gainDb) || ! scalar("pan", zone.pan)
            || ! scalar("velocityTracking", zone.velocityTracking) || ! scalar("keyTracking", zone.keyTracking)
            || ! numberField(entry, "attackSeconds", zone.attackSeconds) || ! numberField(entry, "releaseSeconds", zone.releaseSeconds)
            || ! integerField(entry, "offset", zone.offset) || ! integerField(entry, "end", zone.end)
            || ! integerField(entry, "loopStart", zone.loopStart) || ! integerField(entry, "loopEnd", zone.loopEnd)
            || ! integerField(entry, "loopCrossfade", zone.loopCrossfade))
            return {nullptr, "Invalid native sample zone metadata", {}};
        mapped.push_back(std::move(zone));
    }
    return OscillatorMedia::fromZones(std::move(mapped), property(value, "name").toString().toStdString());
}

using Effects = WavetableEffects;
constexpr std::array<const char*, 6> routingIds{"MainToBus1", "MainToBus2", "Bus1ToMain", "Bus2ToMain", "MainGain", "DirectGain"};
std::array<float, 6> routingValues(const Effects::Routing& routing)
{
    return {routing.mainToBus1, routing.mainToBus2, routing.bus1ToMain, routing.bus2ToMain, routing.mainGain, routing.directGain};
}
bool validEffects(const Effects::Configuration& configuration, std::string& error)
{
    if (configuration.mixLaw != Effects::MixLaw::EqualPower && configuration.mixLaw != Effects::MixLaw::Shaped)
    { error = "Invalid native effect mix law"; return false; }
    const auto gains = routingValues(configuration.routing);
    for (size_t i = 0; i < gains.size(); ++i)
        if (! std::isfinite(gains[i]) || gains[i] < 0 || gains[i] > (i == 2 || i == 3 || i == 5 ? 4 : 2))
        { error = "Invalid native effect routing gain"; return false; }
    std::set<Effects::ModuleId> identities;
    std::set<const ConvolutionIR*> impulses;
    size_t rawBytes = 0;
    const auto visit = [&](const auto& self, const std::vector<Effects::Module>& modules, int depth) -> bool {
        if (depth > Effects::maxDepth || modules.size() > static_cast<size_t>(depth == 0 ? Effects::maxBusModules : Effects::maxBandModules))
        { error = "Native effect chain depth/count limit exceeded"; return false; }
        for (const auto& module : modules)
        {
            if (module.id == 0 || ! identities.insert(module.id).second || identities.size() > Effects::maxModules)
            { error = "Invalid or duplicate native effect identity"; return false; }
            const auto descriptors = Effects::parameterDescriptors(module.type);
            if (descriptors.empty() || module.values.size() > descriptors.size())
            { error = "Unsupported native effect type or parameter count"; return false; }
            for (size_t i = 0; i < module.values.size(); ++i)
                if (! std::isfinite(module.values[i]) || module.values[i] < descriptors[i].minimum || module.values[i] > descriptors[i].maximum)
                { error = "Native effect parameter is outside its range"; return false; }
            if (module.impulse && module.type != Effects::Type::Convolution)
            { error = "Only a convolution effect can contain an impulse response"; return false; }
            if (module.impulse && impulses.insert(module.impulse.get()).second)
            {
                rawBytes += module.impulse->originalSamples().size() * sizeof(float);
                if (rawBytes > (model::NativeContent::maxBytes / 4) * 3)
                { error = "Impulse responses exceed the project content size limit"; return false; }
            }
            for (const auto& band : module.bands)
            {
                if (band.empty()) continue;
                if (module.type != Effects::Type::Split3)
                { error = "Only a three-band splitter can contain band effects"; return false; }
                if (! self(self, band, depth + 1)) return false;
            }
        }
        return true;
    };
    for (const auto& bus : configuration.buses) if (! visit(visit, bus, 0)) return false;
    return true;
}

juce::var effectsToValue(const Effects::Configuration& configuration, const juce::var& previous)
{
    auto value = previous.isObject() ? previous : object();
    std::map<Effects::ModuleId, juce::var> retained;
    size_t visited = 0;
    const auto collect = [&](const auto& self, const juce::var& chain, int depth) -> void {
        const auto* list = chain.getArray();
        if (list == nullptr || depth > Effects::maxDepth || list->size() > Effects::maxBusModules) return;
        for (const auto& entry : *list)
        {
            if (++visited > Effects::maxModules) return;
            double id = 0;
            if (entry.isObject() && numberField(entry, "id", id) && id >= 1 && id <= UINT32_MAX && std::floor(id) >= id)
                retained.emplace(static_cast<Effects::ModuleId>(id), entry);
            const auto bands = property(entry, "bands");
            if (const auto* branches = bands.getArray(); branches != nullptr && branches->size() == 3)
                for (const auto& branch : *branches) self(self, branch, depth + 1);
        }
    };
    const auto oldBuses = property(previous, "buses");
    if (const auto* list = oldBuses.getArray(); list != nullptr && list->size() == Effects::busCount)
        for (const auto& bus : *list) collect(collect, bus, 0);
    set(value, "version", 1); set(value, "alignLatency", configuration.alignLatency);
    set(value, "mixLaw", configuration.mixLaw == Effects::MixLaw::Shaped ? "shaped" : "equal-power");
    juce::Array<juce::var> beforeMain;
    for (const bool before : configuration.returnBeforeMain) beforeMain.add(before);
    set(value, "returnBeforeMain", beforeMain);
    juce::Array<juce::var> routing, impulses, buses;
    for (const float gain : routingValues(configuration.routing)) routing.add(gain);
    set(value, "routing", routing);
    std::vector<const ConvolutionIR*> unique;
    const auto chainValue = [&](const auto& self, const std::vector<Effects::Module>& modules) -> juce::var {
        juce::Array<juce::var> chain;
        for (const auto& module : modules)
        {
            const auto old = retained.find(module.id);
            auto entry = old != retained.end() && property(old->second, "type") == juce::var(static_cast<int>(module.type))
                ? old->second : object();
            set(entry, "id", static_cast<juce::int64>(module.id));
            set(entry, "type", static_cast<int>(module.type)); set(entry, "enabled", module.enabled);
            juce::Array<juce::var> values;
            const auto descriptors = Effects::parameterDescriptors(module.type);
            for (size_t i = 0; i < descriptors.size(); ++i)
                values.add(i < module.values.size() ? module.values[i] : descriptors[i].initial);
            set(entry, "values", values);
            if (module.impulse)
            {
                const auto* impulse = module.impulse.get();
                const auto found = std::find(unique.begin(), unique.end(), impulse);
                const int index = static_cast<int>(found - unique.begin());
                if (found == unique.end())
                {
                    auto asset = object(); set(asset, "name", juce::String(impulse->name()));
                    set(asset, "channels", impulse->channels()); set(asset, "sampleRate", impulse->sampleRate());
                    set(asset, "encoding", "juce-base64-f32le"); set(asset, "pcm", encodeSamples(impulse->originalSamples()));
                    impulses.add(asset); unique.push_back(impulse);
                }
                set(entry, "impulse", index);
            }
            else entry.getDynamicObject()->removeProperty("impulse");
            if (module.type == Effects::Type::Split3)
            {
                juce::Array<juce::var> bands;
                for (const auto& band : module.bands) bands.add(self(self, band));
                set(entry, "bands", bands);
            }
            else entry.getDynamicObject()->removeProperty("bands");
            chain.add(entry);
        }
        return chain;
    };
    for (const auto& bus : configuration.buses) buses.add(chainValue(chainValue, bus));
    set(value, "buses", buses); set(value, "impulses", impulses);
    return value;
}

bool effectsFromValue(const juce::var& value, Effects::Configuration& result, std::string& error)
{
    error = "Invalid native effect graph";
    const auto routing = property(value, "routing"), buses = property(value, "buses"), impulses = property(value, "impulses");
    const auto* routes = routing.getArray(); const auto* chains = buses.getArray(); const auto* assets = impulses.getArray();
    if (! value.isObject() || property(value, "version") != juce::var(1) || ! property(value, "alignLatency").isBool()
        || routes == nullptr || (routes->size() != 5 && routes->size() != 6) || chains == nullptr || chains->size() != Effects::busCount
        || assets == nullptr || assets->size() > Effects::maxModules) return false;
    Effects::Configuration next;
    const auto beforeMain = property(value, "returnBeforeMain");
    if (! beforeMain.isVoid())
    {
        const auto* flags = beforeMain.getArray();
        if (! flags || flags->size() != 2 || ! flags->getReference(0).isBool() || ! flags->getReference(1).isBool()) return false;
        for (int bus = 0; bus < 2; ++bus) next.returnBeforeMain[static_cast<size_t>(bus)] = static_cast<bool>(flags->getReference(bus));
    }
    const auto mixLaw = property(value, "mixLaw");
    if (! mixLaw.isVoid())
    {
        if (! mixLaw.isString() || (mixLaw.toString() != "shaped" && mixLaw.toString() != "equal-power")) return false;
        next.mixLaw = mixLaw.toString() == "shaped" ? Effects::MixLaw::Shaped : Effects::MixLaw::EqualPower;
    }
    std::array<float, 6> gains{0, 0, 1, 1, 1, 1};
    for (int i = 0; i < routes->size(); ++i)
    {
        if (! numeric(routes->getReference(i))) return false;
        const double gain = static_cast<double>(routes->getReference(i));
        if (gain < 0 || gain > (i == 2 || i == 3 || i == 5 ? 4 : 2)) return false;
        gains[static_cast<size_t>(i)] = static_cast<float>(gain);
    }
    next.routing = {gains[0], gains[1], gains[2], gains[3], gains[4], gains[5]};
    next.alignLatency = static_cast<bool>(property(value, "alignLatency"));
    std::vector<std::shared_ptr<const ConvolutionIR>> decoded;
    size_t totalPcm = 0;
    for (const auto& asset : *assets)
    {
        int channels = 0; double rate = 0; std::vector<float> pcm;
        if (! asset.isObject() || ! indexValue(property(asset, "channels"), 3, channels) || channels == 0
            || ! numberField(asset, "sampleRate", rate) || property(asset, "encoding").toString() != "juce-base64-f32le"
            || ! decodeSamples(property(asset, "pcm"), pcm)) return false;
        totalPcm += pcm.size() * sizeof(float);
        if (totalPcm > model::NativeContent::maxBytes) return false;
        auto loaded = ConvolutionIR::fromSamples(pcm, channels, rate, property(asset, "name").toString().toStdString());
        if (! loaded) { error = loaded.error; return false; }
        decoded.push_back(std::move(loaded.data));
    }
    size_t moduleCount = 0;
    const auto readChain = [&](const auto& self, const juce::var& encoded, std::vector<Effects::Module>& modules, int depth) -> bool {
        const auto* list = encoded.getArray();
        if (list == nullptr || (depth > Effects::maxDepth && ! list->isEmpty())
            || list->size() > (depth == 0 ? Effects::maxBusModules : Effects::maxBandModules)) return false;
        for (const auto& entry : *list)
        {
            if (++moduleCount > Effects::maxModules || ! entry.isObject()) return false;
            double id = 0; int type = -1;
            if (! numberField(entry, "id", id) || id < 1 || id > UINT32_MAX || std::floor(id) < id
                || ! indexValue(property(entry, "type"), 15, type) || ! property(entry, "enabled").isBool()) return false;
            const auto effectType = Effects::typeFromCode(type); if (! effectType) return false;
            Effects::Module module; module.id = static_cast<Effects::ModuleId>(id); module.type = *effectType;
            module.enabled = static_cast<bool>(property(entry, "enabled"));
            const auto values = property(entry, "values"); const auto* controls = values.getArray();
            const auto descriptors = Effects::parameterDescriptors(module.type);
            if (controls == nullptr || static_cast<size_t>(controls->size()) > descriptors.size()) return false;
            for (const auto& control : *controls)
            {
                if (! numeric(control) || std::abs(static_cast<double>(control)) > 1e12) return false;
                module.values.push_back(static_cast<float>(static_cast<double>(control)));
            }
            const auto impulse = property(entry, "impulse");
            if (! impulse.isVoid())
            {
                int index = -1;
                if (! indexValue(impulse, static_cast<int>(decoded.size()), index)) return false;
                module.impulse = decoded[static_cast<size_t>(index)];
            }
            const auto bands = property(entry, "bands");
            if (! bands.isVoid())
            {
                const auto* branch = bands.getArray();
                if (branch == nullptr || branch->size() != 3 || module.type != Effects::Type::Split3) return false;
                for (int b = 0; b < 3; ++b)
                    if (! self(self, branch->getReference(b), module.bands[static_cast<size_t>(b)], depth + 1)) return false;
            }
            modules.push_back(std::move(module));
        }
        return true;
    };
    for (int bus = 0; bus < Effects::busCount; ++bus)
        if (! readChain(readChain, chains->getReference(bus), next.buses[static_cast<size_t>(bus)], 0)) return false;
    if (! validEffects(next, error)) return false;
    result = std::move(next); error.clear(); return true;
}
} // namespace

bool readWavetableEffects(const model::Device& device, WavetableEffects::Configuration& configuration, std::string& error)
{
    error.clear();
    if (device.type != "SonoraWavetable" || ! device.nativeContent || device.nativeContent->format != format)
    { error = "No native effect graph is stored in this device"; return false; }
    juce::var value;
    if (! root(device, value, error)) return false;
    return effectsFromValue(property(value, "effects"), configuration, error);
}

bool storeWavetableEffects(model::Device& device, const WavetableEffects::Configuration& configuration, std::string& error)
{
    error.clear();
    if (device.type != "SonoraWavetable") { error = "Invalid wavetable device"; return false; }
    if (! validEffects(configuration, error)) return false;
    juce::var value;
    if (! root(device, value, error)) return false;
    set(value, "effects", effectsToValue(configuration, property(value, "effects")));
    auto next = device;
    if (! commit(next, value, error)) return false;
    std::set<std::string> paths;
    const auto write = [&](const std::string& path, float number, bool toggle = false) {
        paths.insert(path); parameter(next, path, number);
        for (auto& p : next.parameters) if (p.path == path) { p.booleanValue = toggle; break; }
    };
    const auto routing = routingValues(configuration.routing);
    for (size_t i = 0; i < routing.size(); ++i) write(std::string("FxRouting/") + routingIds[i], routing[i]);
    const auto visit = [&](const auto& self, const std::vector<Effects::Module>& modules) -> void {
        for (const auto& module : modules)
        {
            const std::string prefix = "Fx." + std::to_string(module.id) + "/";
            write(prefix + "On", module.enabled ? 1.0f : 0.0f, true);
            const auto descriptors = Effects::parameterDescriptors(module.type);
            for (size_t i = 0; i < descriptors.size(); ++i)
                write(prefix + std::string(descriptors[i].id), i < module.values.size() ? module.values[i] : descriptors[i].initial);
            for (const auto& band : module.bands) self(self, band);
        }
    };
    for (const auto& bus : configuration.buses) visit(visit, bus);
    std::erase_if(next.parameters, [&](const auto& p) {
        return (p.path.starts_with("Fx.") || p.path.starts_with("FxRouting/")) && ! paths.contains(p.path);
    });
    device = std::move(next); return true;
}

bool storeWavetableVoiceRate(model::Device& device, int factor, std::string& error)
{
    if (device.type != "SonoraWavetable" || (factor != 1 && factor != 2))
    { error = "Invalid native voice rate"; return false; }
    juce::var value;
    if (! root(device, value, error)) return false;
    set(value, "voiceRateFactor", factor);
    return commit(device, value, error);
}
bool storeWavetableTable(model::Device& device, int oscillator, const WavetableData& table, std::string& error)
{
    if (device.type != "SonoraWavetable" || oscillator < 0 || oscillator >= WavetableSynth::kOscillators)
    { error = "Invalid wavetable device or oscillator"; return false; }
    juce::var value;
    if (! root(device, value, error)) return false;
    std::vector<uint8_t> bytes;
    bytes.reserve(table.originalSamples().size() * 4);
    for (float sample : table.originalSamples())
    {
        const auto bits = std::bit_cast<uint32_t>(sample);
        for (unsigned shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<uint8_t>(bits >> shift));
    }
    auto entry = object();
    set(entry, "index", oscillator); set(entry, "name", juce::String(table.name()));
    set(entry, "channels", table.channels()); set(entry, "frameLength", table.frameLength());
    set(entry, "sampleRate", table.sampleRate()); set(entry, "encoding", "juce-base64-f32le");
    set(entry, "playbackProfile", table.playbackProfile() == WavetableData::PlaybackProfile::Dense12 ? "dense-12"
        : table.playbackProfile() == WavetableData::PlaybackProfile::Octave16Minimax ? "octave-16-minimax" : "octave-16-approx");
    if (table.frameLayout() != WavetableFrameLayout::Continuous)
        set(entry, "frameLayout", table.frameLayout() == WavetableFrameLayout::Steps ? "steps" : "interpolated-256");
    set(entry, "samples", encode(bytes)); replaceEntry(value, "tables", oscillator, entry);
    if (! commit(device, value, error)) return false;
    parameter(device, "Osc." + std::to_string(oscillator) + "/Table", WavetableSynth::Custom);
    return true;
}

bool storeWavetableLfo(model::Device& device, int lfo, const dsp::CurveGraph& curve, std::string& error)
{
    if (device.type != "SonoraWavetable" || lfo < 0 || lfo >= WavetableSynth::kLfos || ! validCurve(curve))
    { error = "Invalid wavetable device or LFO curve"; return false; }
    juce::var value;
    if (! root(device, value, error)) return false;
    juce::Array<juce::var> points;
    for (int i = 0; i < curve.count; ++i)
    {
        auto point = object();
        set(point, "x", curve.x[i]); set(point, "y", curve.y[i]); set(point, "tension", curve.curve[i]);
        points.add(point);
    }
    auto entry = object(); set(entry, "index", lfo); set(entry, "points", points);
    replaceEntry(value, "lfos", lfo, entry);
    if (! commit(device, value, error)) return false;
    parameter(device, "Lfo." + std::to_string(lfo) + "/Shape", WavetableSynth::CustomLfoShape);
    return true;
}

bool storeWavetableModulationCurve(model::Device& device, int slot, bool auxiliary, const dsp::CurveGraph& curve,
                                  bool enabled, std::string& error)
{
    if (device.type != "SonoraWavetable" || slot < 0 || slot >= WavetableSynth::kModSlots || ! validCurve(curve))
    { error = "Invalid wavetable device or modulation curve"; return false; }
    juce::var value;
    if (! root(device, value, error)) return false;
    // Read the curve profile without rebuilding potentially large oscillator banks.
    bool exponential = device.nativeContent && (device.nativeContent->format == "serum2-xferjson" || device.nativeContent->format == "serum2-preset");
    const auto origin = property(value, "origin");
    if (origin.isObject()) exponential = property(origin, "format").toString().startsWith("serum2-");
    const auto existing = property(value, "modulationCurves");
    if (const auto* entries = existing.getArray())
        for (const auto& item : *entries)
        {
            int storedIndex = -1;
            if (indexValue(property(item, "index"), WavetableSynth::kModSlots * 2, storedIndex)
                && storedIndex == slot * 2 + (auxiliary ? 1 : 0))
                exponential = property(item, "law").toString() == "exponential";
        }
    juce::Array<juce::var> points;
    for (int i = 0; i < curve.count; ++i)
    {
        auto point = object();
        set(point, "x", curve.x[i]); set(point, "y", curve.y[i]); set(point, "tension", curve.curve[i]);
        points.add(point);
    }
    auto entry = object(); set(entry, "index", slot * 2 + (auxiliary ? 1 : 0));
    set(entry, "points", points); set(entry, "enabled", enabled);
    set(entry, "law", exponential ? "exponential" : "power");
    replaceEntry(value, "modulationCurves", slot * 2 + (auxiliary ? 1 : 0), entry);
    return commit(device, value, error);
}

bool storeOscillatorMedia(model::Device& device, int oscillator, const OscillatorMedia& media, std::string& error, bool preferFileReferences)
{
    if (device.type != "SonoraWavetable" || oscillator < 0 || oscillator >= WavetableSynth::kOscillators)
    { error = "Invalid wavetable device or oscillator"; return false; }
    juce::var value;
    if (! root(device, value, error)) return false;
    std::vector<const OscillatorSample*> unique;
    juce::Array<juce::var> pcm, zones;
    size_t bankBytes = 0;
    std::set<const OscillatorSample*> seen;
    for (const auto& zone : media.zones())
        if (seen.insert(zone.sample.get()).second) bankBytes += zone.sample->originalSamples().size() * sizeof(float);
    const auto existingBytes = juce::JSON::toString(value, true).getNumBytesAsUTF8();
    const auto controllerBytes = device.nativeContent ? device.nativeContent->controllerData.size() : 0;
    const auto available = model::NativeContent::maxBytes - std::min(model::NativeContent::maxBytes, existingBytes + controllerBytes);
    // Leave room for zone metadata. Large banks retain exact validated references to their existing files.
    const bool references = preferFileReferences || bankBytes > (available > 131072 ? (available - 131072) / 4 * 3 : 0);
    for (const auto& zone : media.zones())
    {
        const auto* sample = zone.sample.get();
        const auto found = std::find(unique.begin(), unique.end(), sample);
        const int index = static_cast<int>(found - unique.begin());
        if (found == unique.end())
        {
            auto entry = object(); set(entry, "channels", sample->channels()); set(entry, "sampleRate", sample->sampleRate());
            set(entry, "name", juce::String(sample->name())); set(entry, "spectral", sample->hasSpectralAnalysis());
            if (sample->hasSpectralAnalysis()) set(entry, "spectralFrameLimit", static_cast<int>(sample->spectralFrames().size()));
            if (references && ! sample->sourceFile().empty())
            {
                set(entry, "encoding", "audio-file"); set(entry, "sourceFile", juce::String(sample->sourceFile()));
                set(entry, "frames", sample->sampleFrames()); set(entry, "fingerprintAlgorithm", "fnv1a64-f32le");
                set(entry, "pcmFingerprint", juce::String::toHexString(static_cast<juce::int64>(sample->pcmFingerprint())));
            }
            else
            {
                if (references) { error = "Large embedded sample banks need source audio files before saving"; return false; }
                set(entry, "encoding", "juce-base64-f32le"); set(entry, "pcm", encodeSamples(sample->originalSamples()));
            }
            pcm.add(entry); unique.push_back(sample);
        }
        auto entry = object(); set(entry, "sample", index);
        set(entry, "lowKey", zone.lowKey); set(entry, "highKey", zone.highKey);
        set(entry, "lowVelocity", zone.lowVelocity); set(entry, "highVelocity", zone.highVelocity); set(entry, "rootKey", zone.rootKey);
        set(entry, "tuneCents", zone.tuneCents); set(entry, "gainDb", zone.gainDb); set(entry, "pan", zone.pan);
        set(entry, "velocityTracking", zone.velocityTracking); set(entry, "keyTracking", zone.keyTracking);
        set(entry, "attackSeconds", zone.attackSeconds); set(entry, "releaseSeconds", zone.releaseSeconds);
        set(entry, "offset", static_cast<juce::int64>(zone.offset)); set(entry, "end", static_cast<juce::int64>(zone.end));
        set(entry, "loopStart", static_cast<juce::int64>(zone.loopStart)); set(entry, "loopEnd", static_cast<juce::int64>(zone.loopEnd));
        set(entry, "loopCrossfade", static_cast<juce::int64>(zone.loopCrossfade));
        set(entry, "loop", static_cast<int>(zone.loop)); set(entry, "loopWhileHeld", zone.loopWhileHeld); zones.add(entry);
    }
    auto entry = object(); set(entry, "index", oscillator); set(entry, "name", juce::String(media.name()));
    set(entry, "samples", pcm); set(entry, "zones", zones); replaceEntry(value, "media", oscillator, entry);
    if (! commit(device, value, error)) return false;
    const std::string path = "Media." + std::to_string(oscillator) + "/Mode";
    bool hasMode = false;
    for (const auto& p : device.parameters) if (p.path == path && p.value > 0) hasMode = true;
    if (! hasMode) parameter(device, path, media.zones().size() > 1 ? WavetableSynth::MultisampleMode : WavetableSynth::SampleMode);
    return true;
}

bool storeWavetableNoise(model::Device& device, std::shared_ptr<const OscillatorSample> sample,
                         std::string& error, bool preferFileReferences)
{
    if (device.type != "SonoraWavetable") { error = "Invalid wavetable device"; return false; }
    juce::var value;
    if (! root(device, value, error)) return false;
    if (sample == nullptr)
    {
        auto entry = object(); set(entry, "cleared", true); set(value, "noise", entry);
        return commit(device, value, error);
    }
    OscillatorZone zone; zone.sample = std::move(sample); zone.loop = MediaLoop::Forward;
    const auto media = OscillatorMedia::fromZones({zone}, zone.sample->name());
    if (! media) { error = media.error; return false; }
    // Reuse the bounded PCM/reference codec transactionally in a temporary model.
    // Only its encoded asset is copied; oscillator0 and its parameters stay intact.
    auto staged = device;
    if (! storeOscillatorMedia(staged, 0, *media.data, error, preferFileReferences)) return false;
    juce::var prepared;
    if (! root(staged, prepared, error)) return false;
    const auto entries = property(prepared, "media");
    const auto* list = entries.getArray();
    if (list == nullptr) { error = "Could not encode Noise sample"; return false; }
    for (const auto& entry : *list)
    {
        int index = -1;
        if (indexValue(property(entry, "index"), WavetableSynth::kOscillators, index) && index == 0)
        { set(value, "noise", entry); return commit(device, value, error); }
    }
    error = "Could not encode Noise sample"; return false;
}

void prepareWavetableContent(WavetableSynth& synth, const model::Device& device, std::vector<std::string>& warnings)
{
    if (! device.nativeContent) return;
    if (device.nativeContent->format != format)
    {
        plugins::prepareSerumAssets(synth, *device.nativeContent, warnings);
        return;
    }
    juce::var value; std::string error;
    if (! root(device, value, error)) { warnings.push_back(std::move(error)); return; }
    const auto origin = property(value, "origin");
    if (origin.isObject())
    {
        model::NativeContent source;
        source.format = property(origin, "format").toString().toStdString();
        source.sourceDirectory = device.nativeContent->sourceDirectory;
        if (decode(property(origin, "data"), source.data)) plugins::prepareSerumAssets(synth, source, warnings);
        else warnings.push_back("Invalid preserved preset data");
    }
    const auto effects = property(value, "effects");
    const auto voiceRate = property(value, "voiceRateFactor");
    if (! voiceRate.isVoid())
    {
        int factor = -1;
        if (! indexValue(voiceRate, 3, factor) || factor < 1)
            warnings.push_back("Invalid native voice rate; previous preparation retained");
        else synth.setVoiceRateFactor(factor);
    }
    if (! effects.isVoid())
    {
        WavetableEffects::Configuration configuration;
        if (! effectsFromValue(effects, configuration, error) || ! synth.setEffectsConfiguration(configuration, error))
            warnings.push_back("Could not restore native effects: " + error);
    }
    const auto tables = property(value, "tables");
    if (! tables.isVoid() && ! tables.isArray()) warnings.push_back("Invalid native wavetable asset list");
    if (const auto* array = tables.getArray())
    {
        if (array->size() > WavetableSynth::kOscillators) warnings.push_back("Too many native wavetable assets");
        else
        {
          std::array<bool, WavetableSynth::kOscillators> seen{};
          for (const auto& entry : *array)
          {
            int index = -1, channels = -1, frameLength = -1;
            const auto rate = property(entry, "sampleRate");
            const auto profileValue = property(entry, "playbackProfile");
            auto profile = WavetableData::PlaybackProfile::Dense12;
            if (! profileValue.isVoid())
            {
                if (! profileValue.isString() || (profileValue.toString() != "dense-12" && profileValue.toString() != "octave-16-approx"
                    && profileValue.toString() != "octave-16-minimax"))
                { warnings.push_back("Invalid native wavetable playback profile"); continue; }
                if (profileValue.toString() == "octave-16-approx") profile = WavetableData::PlaybackProfile::Octave16Approx;
                if (profileValue.toString() == "octave-16-minimax") profile = WavetableData::PlaybackProfile::Octave16Minimax;
            }
            if (! entry.isObject() || ! indexValue(property(entry, "index"), WavetableSynth::kOscillators, index)
                || seen[static_cast<size_t>(index)] || ! indexValue(property(entry, "channels"), 3, channels)
                || ! indexValue(property(entry, "frameLength"), 2049, frameLength) || ! numeric(rate))
            { warnings.push_back("Invalid or duplicate native wavetable metadata"); continue; }
            seen[static_cast<size_t>(index)] = true;
            std::vector<uint8_t> bytes;
            if (property(entry, "encoding").toString() != "juce-base64-f32le" || ! decode(property(entry, "samples"), bytes) || bytes.size() % 4 != 0)
            { warnings.push_back("Invalid native wavetable sample encoding"); continue; }
            std::vector<float> samples(bytes.size() / 4);
            for (size_t i = 0; i < samples.size(); ++i)
            {
                uint32_t bits = 0;
                for (unsigned j = 0; j < 4; ++j) bits |= static_cast<uint32_t>(bytes[i * 4 + j]) << (8 * j);
                samples[i] = std::bit_cast<float>(bits);
            }
            auto layout = WavetableFrameLayout::Continuous;
            const auto layoutValue = property(entry, "frameLayout");
            if (layoutValue.toString() == "steps") layout = WavetableFrameLayout::Steps;
            else if (layoutValue.toString() == "interpolated-256") layout = WavetableFrameLayout::Interpolated256;
            else if (! layoutValue.isVoid()) warnings.push_back("Unknown native wavetable frame layout; using continuous");
            const auto loaded = WavetableData::fromSamples(samples, channels,
                frameLength, static_cast<double>(rate),
                property(entry, "name").toString().toStdString(), profile, {layout, 1, false});
            if (! loaded || ! synth.setCustomTable(index, loaded.data))
                warnings.push_back(loaded ? "Invalid native wavetable oscillator" : loaded.error);
          }
        }
    }
    const auto lfos = property(value, "lfos");
    if (! lfos.isVoid() && ! lfos.isArray()) warnings.push_back("Invalid native LFO curve list");
    if (const auto* array = lfos.getArray())
    {
        if (array->size() > WavetableSynth::kLfos) warnings.push_back("Too many native LFO curves");
        else
        {
          std::array<bool, WavetableSynth::kLfos> seen{};
          for (const auto& entry : *array)
          {
            int index = -1;
            dsp::CurveGraph curve;
            if (! entry.isObject() || ! indexValue(property(entry, "index"), WavetableSynth::kLfos, index)
                || seen[static_cast<size_t>(index)] || ! curveFromValue(entry, curve) || ! validCurve(curve)
                || ! synth.setLfoCurve(index, curve))
                warnings.push_back("Invalid native LFO curve");
            else seen[static_cast<size_t>(index)] = true;
          }
        }
    }
    const auto modulation = property(value, "modulationCurves");
    if (! modulation.isVoid() && ! modulation.isArray()) warnings.push_back("Invalid native modulation curve list");
    if (const auto* array = modulation.getArray())
    {
        if (array->size() > WavetableSynth::kModSlots * 2) warnings.push_back("Too many native modulation curves");
        else
        {
            std::array<bool, WavetableSynth::kModSlots * 2> seen{};
            for (const auto& entry : *array)
            {
                int index = -1; dsp::CurveGraph curve;
                const auto enabled = property(entry, "enabled");
                const auto law = property(entry, "law").toString();
                if (! entry.isObject() || ! indexValue(property(entry, "index"), WavetableSynth::kModSlots * 2, index)
                    || seen[static_cast<size_t>(index)] || ! enabled.isBool() || (law != "power" && law != "exponential")
                    || ! curveFromValue(entry, curve) || ! validCurve(curve)
                    || ! synth.setModulationCurve(index / 2, index % 2 != 0, curve, static_cast<bool>(enabled),
                        law == "exponential" ? WavetableSynth::CurveLaw::ImportedExponential : WavetableSynth::CurveLaw::NativePower))
                    warnings.push_back("Invalid native modulation curve");
                else seen[static_cast<size_t>(index)] = true;
            }
        }
    }
    const auto media = property(value, "media");
    const auto noise = property(value, "noise");
    if (! noise.isVoid())
    {
        if (property(noise, "cleared").isBool() && static_cast<bool>(property(noise, "cleared")))
        { synth.setNoiseSample(nullptr); synth.setImportedNoiseProfile(false); }
        else
        {
            synth.setNoiseSampleExpected(true);
            const auto sampleEntries = property(noise, "samples"), zoneEntries = property(noise, "zones");
            const auto* samples = sampleEntries.getArray();
            const auto* zones = zoneEntries.getArray();
            if (samples == nullptr || zones == nullptr || samples->size() != 1 || zones->size() != 1)
                warnings.push_back("Invalid native Noise sample; previous preparation retained");
            else
            {
                const auto loaded = mediaFromValue(noise);
                if (! loaded || loaded.data->zones().size() != 1)
                    warnings.push_back("Could not restore native Noise sample: " + loaded.error);
                else synth.setNoiseSample(loaded.data->zones().front().sample);
            }
        }
    }
    if (! media.isVoid() && ! media.isArray()) warnings.push_back("Invalid native sample bank list");
    if (const auto* array = media.getArray())
    {
        if (array->size() > WavetableSynth::kOscillators) warnings.push_back("Too many native sample banks");
        else
        {
            std::array<bool, WavetableSynth::kOscillators> seen{};
            for (const auto& entry : *array)
            {
                int index = -1;
                if (! indexValue(property(entry, "index"), WavetableSynth::kOscillators, index) || seen[static_cast<size_t>(index)])
                { warnings.push_back("Invalid or duplicate native sample bank index"); continue; }
                auto loaded = mediaFromValue(entry);
                warnings.insert(warnings.end(), loaded.warnings.begin(), loaded.warnings.end());
                if (! loaded || ! synth.setOscillatorMedia(index, loaded.data))
                    warnings.push_back(loaded ? "Invalid native sample oscillator" : loaded.error);
                else seen[static_cast<size_t>(index)] = true;
            }
        }
    }
}
} // namespace sonora::devices

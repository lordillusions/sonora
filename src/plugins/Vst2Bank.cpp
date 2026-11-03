#include "plugins/Vst2Bank.h"

#include <algorithm>

namespace sonora::plugins
{
namespace
{
constexpr size_t nameBytes = 28, futureBytes = 128;

void putBE(std::vector<uint8_t>& out, uint32_t v)
{
    for (int shift = 24; shift >= 0; shift -= 8) out.push_back((uint8_t) (v >> shift));
}

uint32_t getBE(const uint8_t* p)
{
    return (uint32_t) p[0] << 24 | (uint32_t) p[1] << 16 | (uint32_t) p[2] << 8 | p[3];
}

void patchSize(std::vector<uint8_t>& out, size_t start)
{
    const auto size = (uint32_t) (out.size() - start - 8); // excludes magic and the size field itself
    for (int i = 0; i < 4; ++i) out[start + 4 + (size_t) i] = (uint8_t) (size >> (24 - 8 * i));
}

// One FxCk program from Live's { name[28], little-endian floats }.
void putProgram(std::vector<uint8_t>& out, const uint8_t* src, int numParams, int32_t uniqueId, int version)
{
    const auto start = out.size();
    putBE(out, fourCC("CcnK"));
    putBE(out, 0);
    putBE(out, fourCC("FxCk"));
    putBE(out, 1);
    putBE(out, (uint32_t) uniqueId);
    putBE(out, (uint32_t) version);
    putBE(out, (uint32_t) numParams);
    out.insert(out.end(), src, src + nameBytes);
    for (int i = 0; i < numParams; ++i)
    {
        const auto* f = src + nameBytes + 4 * (size_t) i;
        putBE(out, (uint32_t) f[3] << 24 | (uint32_t) f[2] << 16 | (uint32_t) f[1] << 8 | f[0]);
    }
    patchSize(out, start);
}

void putHeader(std::vector<uint8_t>& out, uint32_t magic, int32_t uniqueId, int version, int numPrograms)
{
    putBE(out, fourCC("CcnK"));
    putBE(out, 0);
    putBE(out, magic);
    putBE(out, 1);
    putBE(out, (uint32_t) uniqueId);
    putBE(out, (uint32_t) version);
    putBE(out, (uint32_t) numPrograms);
}
} // namespace

std::vector<uint8_t> buildVst2Bank(const model::PluginInfo& info, int32_t uniqueId)
{
    const auto& state = info.state;
    if (state.empty()) return {};
    std::vector<uint8_t> out;
    const auto type = info.vst2PresetType;
    if (type == fourCC("FBCh") || type == fourCC("FPCh"))
    {
        out.reserve(state.size() + 200);
        putHeader(out, type, uniqueId, info.vst2PluginVersion, info.vst2ProgramCount);
        out.insert(out.end(), type == fourCC("FBCh") ? futureBytes : nameBytes, 0);
        putBE(out, (uint32_t) state.size());
        out.insert(out.end(), state.begin(), state.end());
    }
    else if (type == fourCC("FxBk") || type == fourCC("FxCk"))
    {
        const int params = info.vst2ParameterCount;
        const size_t programBytes = nameBytes + 4 * (size_t) std::max(0, params);
        if (params <= 0 || state.size() % programBytes != 0) return {};
        const int programs = (int) (state.size() / programBytes);
        if (type == fourCC("FxCk"))
        {
            putProgram(out, state.data(), params, uniqueId, info.vst2PluginVersion);
            return out;
        }
        putHeader(out, type, uniqueId, info.vst2PluginVersion, programs);
        out.insert(out.end(), futureBytes, 0);
        for (int p = 0; p < programs; ++p) putProgram(out, state.data() + programBytes * (size_t) p, params, uniqueId, info.vst2PluginVersion);
    }
    else
        return {};
    patchSize(out, 0);
    return out;
}

bool readVst2Bank(const std::vector<uint8_t>& fxb, model::PluginInfo& info)
{
    constexpr size_t header = 28;
    if (fxb.size() < header || getBE(fxb.data()) != fourCC("CcnK")) return false;
    const auto* d = fxb.data();
    const auto magic = getBE(d + 8);
    const int version = (int) getBE(d + 20), programs = (int) getBE(d + 24);
    std::vector<uint8_t> state;
    int params = -1;
    if (magic == fourCC("FBCh") || magic == fourCC("FPCh"))
    {
        const size_t sizeAt = header + (magic == fourCC("FBCh") ? futureBytes : nameBytes);
        if (fxb.size() < sizeAt + 4) return false;
        const size_t size = getBE(d + sizeAt);
        if (size > fxb.size() - sizeAt - 4) return false;
        state.assign(d + sizeAt + 4, d + sizeAt + 4 + size);
    }
    else if (magic == fourCC("FxBk"))
    {
        size_t at = header + futureBytes;
        for (int p = 0; p < programs; ++p)
        {
            constexpr size_t programHeader = 28 + nameBytes;
            if (fxb.size() < at + programHeader || getBE(d + at) != fourCC("CcnK") || getBE(d + at + 8) != fourCC("FxCk")) return false;
            const int n = (int) getBE(d + at + 24);
            if (n < 0 || (params >= 0 && n != params) || (size_t) n * 4 > fxb.size() - at - programHeader) return false;
            params = n;
            state.insert(state.end(), d + at + 28, d + at + programHeader);
            for (int i = 0; i < n; ++i)
            {
                const auto* f = d + at + programHeader + 4 * (size_t) i;
                state.insert(state.end(), {f[3], f[2], f[1], f[0]}); // Live keeps the floats little-endian
            }
            at += programHeader + 4 * (size_t) n;
        }
    }
    else
        return false;
    info.vst2PresetType = magic;
    info.vst2ProgramCount = programs;
    info.vst2PluginVersion = version;
    if (params >= 0) info.vst2ParameterCount = params;
    info.state = std::move(state);
    return true;
}

} // namespace sonora::plugins

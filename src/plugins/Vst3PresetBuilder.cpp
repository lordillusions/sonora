#include "plugins/Vst3PresetBuilder.h"

#include <cstdio>
#include <charconv>
#include <algorithm>

namespace sonora::plugins
{

namespace
{
void put32(std::vector<uint8_t>& out, int32_t v)
{
    for (int i = 0; i < 4; ++i)
        out.push_back((uint8_t) ((uint32_t) v >> (8 * i)));
}

void put64(std::vector<uint8_t>& out, int64_t v)
{
    for (int i = 0; i < 8; ++i)
        out.push_back((uint8_t) ((uint64_t) v >> (8 * i)));
}

void putId(std::vector<uint8_t>& out, const char* id4)
{
    out.insert(out.end(), id4, id4 + 4);
}
} // namespace

std::string vst3ClassIdString(const std::array<uint32_t, 4>& uid)
{
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%08X%08X%08X%08X", uid[0], uid[1], uid[2], uid[3]);
    return buf;
}

std::vector<uint8_t> buildVstPreset(const std::array<uint32_t, 4>& uid,
                                    const std::vector<uint8_t>& componentState,
                                    const std::vector<uint8_t>& controllerState)
{
    constexpr int64_t headerSize = 4 + 4 + 32 + 8;
    std::vector<uint8_t> out;
    out.reserve((size_t) headerSize + componentState.size() + controllerState.size() + 64);

    const int64_t compOffset = headerSize;
    const int64_t contOffset = compOffset + (int64_t) componentState.size();
    const int64_t listOffset = contOffset + (int64_t) controllerState.size();

    putId(out, "VST3");
    put32(out, 1);
    const auto cid = vst3ClassIdString(uid);
    out.insert(out.end(), cid.begin(), cid.end());
    put64(out, listOffset);

    out.insert(out.end(), componentState.begin(), componentState.end());
    out.insert(out.end(), controllerState.begin(), controllerState.end());

    const int32_t count = controllerState.empty() ? 1 : 2;
    putId(out, "List");
    put32(out, count);
    putId(out, "Comp");
    put64(out, compOffset);
    put64(out, (int64_t) componentState.size());
    if (! controllerState.empty())
    {
        putId(out, "Cont");
        put64(out, contOffset);
        put64(out, (int64_t) controllerState.size());
    }
    return out;
}

bool readVstPreset(const std::vector<uint8_t>& preset, model::PluginInfo& info)
{
    if (preset.size() < 56 || ! std::equal(preset.begin(), preset.begin() + 4, "VST3")) return false;
    auto integer = [&](size_t offset, size_t bytes) {
        uint64_t result = 0;
        for (size_t i = 0; i < bytes; ++i) result |= (uint64_t) preset[offset + i] << (8 * i);
        return result;
    };
    if (integer(4, 4) != 1) return false;
    auto candidate = info;
    for (size_t i = 0; i < 4; ++i)
    {
        const auto* start = reinterpret_cast<const char*>(preset.data() + 8 + 8 * i);
        const auto parsed = std::from_chars(start, start + 8, candidate.vst3Uid[i], 16);
        if (parsed.ec != std::errc{} || parsed.ptr != start + 8) return false;
    }
    const uint64_t list = integer(40, 8);
    if (list < 48 || list > preset.size() - 8 || ! std::equal(preset.begin() + (ptrdiff_t) list, preset.begin() + (ptrdiff_t) list + 4, "List")) return false;
    const uint64_t count = integer((size_t) list + 4, 4);
    if (count > (preset.size() - (size_t) list - 8) / 20) return false;
    candidate.state.clear();
    candidate.controllerState.clear();
    bool component = false;
    for (size_t i = 0; i < (size_t) count; ++i)
    {
        const size_t entry = (size_t) list + 8 + i * 20;
        const uint64_t offset = integer(entry + 4, 8), size = integer(entry + 12, 8);
        if (offset < 48 || offset > list || size > list - offset) return false;
        auto* bytes = std::equal(preset.begin() + (ptrdiff_t) entry, preset.begin() + (ptrdiff_t) entry + 4, "Comp")
                          ? &candidate.state
                          : std::equal(preset.begin() + (ptrdiff_t) entry, preset.begin() + (ptrdiff_t) entry + 4, "Cont")
                                ? &candidate.controllerState : nullptr;
        if (bytes) bytes->assign(preset.begin() + (ptrdiff_t) offset, preset.begin() + (ptrdiff_t) (offset + size));
        if (bytes == &candidate.state) component = true;
    }
    if (! component) return false;
    info = std::move(candidate);
    return true;
}

} // namespace sonora::plugins

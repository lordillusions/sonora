#include "platform/splice/SpliceDescramble.h"

namespace sonora::platform::splice
{

namespace
{
constexpr size_t kHeaderSize = 28;
constexpr size_t kKeyOffset = 10;
constexpr size_t kKeyLength = 18;
} // namespace

bool isScrambledPreview(const uint8_t* data, size_t size)
{
    return data != nullptr && size >= kHeaderSize && data[0] == 's' && data[1] == 'p';
}

std::vector<uint8_t> descramblePreview(const uint8_t* data, size_t size)
{
    if (! isScrambledPreview(data, size))
        return std::vector<uint8_t>(data, data + size);

    const uint8_t* key = data + kKeyOffset;
    std::vector<uint8_t> out(data + kHeaderSize, data + size);
    const size_t quarter = out.size() / 4;
    // Only the 1st and 3rd quarters are masked; the 2nd, 4th and any remainder are already clear.
    for (size_t block : {size_t(0), size_t(2)})
    {
        const size_t start = block * quarter;
        for (size_t j = 0; j < quarter; ++j)
            out[start + j] ^= key[j % kKeyLength];
    }
    return out;
}

} // namespace sonora::platform::splice

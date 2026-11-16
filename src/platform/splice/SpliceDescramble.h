#pragma once
// SPL-1: clean-room descrambler for Splice preview audio (docs/SPLICE_INTEROP.md).
//
// Splice serves previews from ".../<hash>-scrambled/<hash>.mp3": a plain mp3 behind a 28-byte header with an 18-byte
// key, where only the first and third quarters of the body are XOR-masked with that key. This reverses it, byte-for-byte
// identically to the Splice app's own output (validated on 25 samples). No Splice code is used; the transform was
// derived from observed inputs and outputs only.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace sonora::platform::splice
{

// True when `data` begins with the scrambled-preview header ("sp" magic + a 28-byte header fits).
bool isScrambledPreview(const uint8_t* data, size_t size);

// Returns the plain mp3 bytes. If `data` is not a scrambled preview it is returned unchanged (already a plain mp3).
std::vector<uint8_t> descramblePreview(const uint8_t* data, size_t size);

inline std::vector<uint8_t> descramblePreview(const std::vector<uint8_t>& data)
{
    return descramblePreview(data.data(), data.size());
}

} // namespace sonora::platform::splice

#pragma once
// Ed25519 signature checks (Monocypher, scripts/bootstrap.sh) shared by the Linux updater (COM-4l, Sparkle's EdDSA) and the
// sample-pack catalogue (LIB-1, docs/SAMPLE_PACKS.md). Keys and signatures travel as standard base64.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace sonora::crypto
{

// Standard base64 (RFC 4648 section 4, padding optional, surrounding whitespace ignored). Empty on malformed input.
std::vector<uint8_t> fromBase64(std::string_view text);

// Ed25519 over data with a base64 32-byte public key and a base64 64-byte signature. False for malformed input.
bool verifyEd25519(std::string_view publicKeyBase64, const void* data, size_t size, std::string_view signatureBase64);
bool ed25519Available(); // false when the build has no Monocypher: every check then fails

} // namespace sonora::crypto

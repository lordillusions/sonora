// Ed25519.h: Monocypher's crypto_ed25519_check behind base64 keys (COM-4l, LIB-1).
#include "core/Ed25519.h"

#include <juce_core/juce_core.h>

// The native VST3s compile sonora_core's sources without linking Monocypher: they get the "unavailable" stubs.
#if defined(SONORA_HAVE_MONOCYPHER) && SONORA_HAVE_MONOCYPHER
#define SONORA_ED25519 1
#include "monocypher-ed25519.h"
#else
#define SONORA_ED25519 0
#endif

namespace sonora::crypto
{

std::vector<uint8_t> fromBase64(std::string_view text)
{
    juce::MemoryOutputStream out;
    if (! juce::Base64::convertFromBase64(out, juce::String(std::string(text)).trim())) return {};
    const auto* bytes = static_cast<const uint8_t*>(out.getData());
    return std::vector<uint8_t>(bytes, bytes + out.getDataSize());
}

bool ed25519Available()
{
    return SONORA_ED25519 != 0;
}

bool verifyEd25519(std::string_view publicKeyBase64, const void* data, size_t size, std::string_view signatureBase64)
{
    const auto key = fromBase64(publicKeyBase64), signature = fromBase64(signatureBase64);
    if (key.size() != 32 || signature.size() != 64 || (data == nullptr && size > 0)) return false;
#if SONORA_ED25519
    return crypto_ed25519_check(signature.data(), key.data(), static_cast<const uint8_t*>(data), size) == 0;
#else
    return false;
#endif
}

} // namespace sonora::crypto

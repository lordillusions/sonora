#pragma once
// Read-only inspection of the observed Serum 2 XferJson/Zstandard/CBOR state container.
// Message/load thread only: parsing allocates. This is not a patch converter or DSP implementation.

#include <cstdint>
#include <cstddef>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sonora::plugins
{

namespace serumStateDetail { class CborReader; class JsonReader; }
enum class SerumDocumentKind { HostedState, Preset };

class SerumValue
{
public:
    enum class Type { Null, Boolean, UnsignedInteger, NegativeInteger, Number, Text, Bytes, Array, Map };
    using Array = std::vector<SerumValue>;
    using Map = std::map<std::string, SerumValue, std::less<>>;

    Type type() const;
    std::optional<bool> boolean() const;
    std::optional<uint64_t> unsignedInteger() const;
    // CBOR negative integer is exactly -1 - this magnitude, including values below INT64_MIN.
    std::optional<uint64_t> negativeIntegerMagnitude() const;
    // Numeric conversion may round large integers; exact integer accessors above retain all bits.
    std::optional<double> number() const;
    const std::string* text() const;
    const std::vector<uint8_t>* bytes() const;
    const Array* array() const;
    const Map* map() const;
    const SerumValue* find(std::string_view key) const;

private:
    struct Negative { uint64_t magnitude = 0; };
    using Data = std::variant<std::monostate, bool, uint64_t, Negative, double, std::string,
                              std::vector<uint8_t>, Array, Map>;
    Data data_;
    friend class serumStateDetail::CborReader;
    friend class serumStateDetail::JsonReader;
};

class SerumState;
struct SerumStateResult
{
    std::shared_ptr<const SerumState> state;
    std::string error;
    explicit operator bool() const { return state != nullptr; }
};

class SerumState
{
public:
    static constexpr size_t maxDecodedBytes = 64 * 1024 * 1024;
    static constexpr size_t maxMetadataBytes = 1024 * 1024;
    static constexpr size_t maxTreeBytes = 128 * 1024 * 1024;
    // Real component states can embed multiple 250k+ sample float arrays. The independent
    // tree-byte budget still bounds reservations/keys/nodes before allocation.
    static constexpr size_t maxNodes = 1048576;
    static constexpr int maxDepth = 64;

    const SerumValue& metadata() const { return metadata_; } // Null for explicitly decoded raw CBOR.
    const SerumValue& root() const { return root_; }
    const std::vector<uint8_t>& originalBytes() const { return original_; }
    const std::vector<uint8_t>& decodedBytes() const { return decoded_; }
    bool hasContainer() const { return hasContainer_; }

    // No inferred defaults: absent keys and the literal plainParams="default" return nullopt.
    std::optional<double> plainParameter(std::string_view group, std::string_view key) const;
    std::optional<double> plainParameter(std::span<const std::string_view> groups, std::string_view key) const;

private:
    SerumValue metadata_, root_;
    std::vector<uint8_t> original_, decoded_;
    bool hasContainer_ = false;
    friend SerumStateResult decodeSerumState(std::span<const uint8_t> bytes, SerumDocumentKind kind);
    friend SerumStateResult decodeSerumCbor(std::span<const uint8_t> bytes);
};

// Accepts only an observed format-version-9 Serum2/Serum2FX processor/controller container.
// The metadata hash is preserved as opaque text; its algorithm has not been verified.
SerumStateResult decodeSerumState(std::span<const uint8_t> bytes, SerumDocumentKind kind = SerumDocumentKind::HostedState);
// Explicit file route: observed SerumPreset versions 3..7; structurally decoded, not DSP admission.
inline SerumStateResult decodeSerumPreset(std::span<const uint8_t> bytes)
{
    return decodeSerumState(bytes, SerumDocumentKind::Preset);
}
// Explicit raw-CBOR entrypoint, requiring the same product/component/version identity in the root.
SerumStateResult decodeSerumCbor(std::span<const uint8_t> bytes);

} // namespace sonora::plugins

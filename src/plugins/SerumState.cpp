#include "plugins/SerumState.h"

#include <algorithm>
#include <bit>
#include <charconv>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <zstd.h>

#include "platform/CNumeric.h"

namespace sonora::plugins
{

SerumValue::Type SerumValue::type() const { return static_cast<Type>(data_.index()); }
std::optional<bool> SerumValue::boolean() const
{
    if (const auto* v = std::get_if<bool>(&data_)) return *v;
    return {};
}
std::optional<uint64_t> SerumValue::unsignedInteger() const
{
    if (const auto* v = std::get_if<uint64_t>(&data_)) return *v;
    return {};
}
std::optional<uint64_t> SerumValue::negativeIntegerMagnitude() const
{
    if (const auto* v = std::get_if<Negative>(&data_)) return v->magnitude;
    return {};
}
std::optional<double> SerumValue::number() const
{
    if (const auto* v = std::get_if<double>(&data_)) return *v;
    if (const auto* v = std::get_if<uint64_t>(&data_)) return static_cast<double>(*v);
    if (const auto* v = std::get_if<Negative>(&data_)) return -1.0 - static_cast<double>(v->magnitude);
    return {};
}
const std::string* SerumValue::text() const { return std::get_if<std::string>(&data_); }
const std::vector<uint8_t>* SerumValue::bytes() const { return std::get_if<std::vector<uint8_t>>(&data_); }
const SerumValue::Array* SerumValue::array() const { return std::get_if<Array>(&data_); }
const SerumValue::Map* SerumValue::map() const { return std::get_if<Map>(&data_); }
const SerumValue* SerumValue::find(std::string_view key) const
{
    if (const auto* m = map())
    {
        const auto it = m->find(key);
        if (it != m->end()) return &it->second;
    }
    return nullptr;
}

std::optional<double> SerumState::plainParameter(std::string_view group, std::string_view key) const
{
    return plainParameter(std::span<const std::string_view>(&group, 1), key);
}
std::optional<double> SerumState::plainParameter(std::span<const std::string_view> groups, std::string_view key) const
{
    const SerumValue* item = &root_;
    for (const auto group : groups)
    {
        item = item->find(group);
        if (item == nullptr) return {};
    }
    const auto* params = item->find("plainParams");
    if (params == nullptr) return {};
    const auto* parameter = params->find(key);
    if (parameter == nullptr) return {};
    return parameter->number();
}

namespace serumStateDetail
{
namespace
{
bool validUtf8(std::span<const uint8_t> s)
{
    for (size_t i = 0; i < s.size();)
    {
        const uint8_t first = s[i++];
        if (first < 0x80) continue;
        unsigned remaining = 0;
        uint32_t value = 0, minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) { remaining = 1; value = first & 0x1fu; minimum = 0x80; }
        else if (first >= 0xe0 && first <= 0xef) { remaining = 2; value = first & 0x0fu; minimum = 0x800; }
        else if (first >= 0xf0 && first <= 0xf4) { remaining = 3; value = first & 0x07u; minimum = 0x10000; }
        else return false;
        if (remaining > s.size() - i) return false;
        for (unsigned j = 0; j < remaining; ++j)
        {
            const uint8_t next = s[i++];
            if ((next & 0xc0u) != 0x80) return false;
            value = (value << 6) | (next & 0x3fu);
        }
        if (value < minimum || value > 0x10ffff || (value >= 0xd800 && value <= 0xdfff)) return false;
    }
    return true;
}

class Limits
{
protected:
    bool spend(size_t bytes)
    {
        if (bytes > SerumState::maxTreeBytes - allocation_) return fail("tree allocation limit exceeded");
        allocation_ += bytes;
        return true;
    }
    bool node(int depth)
    {
        if (depth > SerumState::maxDepth) return fail("nesting depth limit exceeded");
        if (nodes_ >= SerumState::maxNodes) return fail("node limit exceeded");
        ++nodes_;
        return spend(sizeof(SerumValue));
    }
    bool fail(std::string message)
    {
        if (error_.empty()) error_ = std::move(message);
        return false;
    }
    std::string error_;
private:
    size_t allocation_ = 0, nodes_ = 0;
};
} // namespace

class CborReader final : private Limits
{
public:
    explicit CborReader(std::span<const uint8_t> bytes) : bytes_(bytes) {}
    bool read(SerumValue& out)
    {
        if (! value(out, 0)) return false;
        if (position_ != bytes_.size()) return fail("trailing CBOR data");
        return true;
    }
    std::string error() const { return "CBOR at byte " + std::to_string(position_) + ": " + error_; }
private:
    bool argument(uint8_t extra, uint64_t& out)
    {
        if (extra < 24) { out = extra; return true; }
        if (extra == 31) return fail("indefinite-length CBOR is unsupported");
        if (extra > 27) return fail("reserved CBOR additional information");
        const size_t count = size_t{1} << (extra - 24);
        if (count > bytes_.size() - position_) return fail("truncated CBOR argument");
        out = 0;
        for (size_t i = 0; i < count; ++i) out = (out << 8) | bytes_[position_++];
        return true;
    }
    bool text(std::string& out, uint64_t count)
    {
        if (count > bytes_.size() - position_) return fail("truncated CBOR text");
        const auto length = static_cast<size_t>(count);
        const auto s = bytes_.subspan(position_, length);
        if (! validUtf8(s)) return fail("invalid UTF-8 in CBOR text");
        if (! spend(length + 1)) return false;
        out.assign(reinterpret_cast<const char*>(s.data()), length);
        position_ += length;
        return true;
    }
    bool value(SerumValue& out, int depth)
    {
        if (! node(depth)) return false;
        if (position_ >= bytes_.size()) return fail("truncated CBOR value");
        const uint8_t initial = bytes_[position_++];
        const uint8_t major = initial >> 5, extra = initial & 0x1f;
        uint64_t count = 0;
        if (! argument(extra, count)) return false;
        switch (major)
        {
            case 0: out.data_ = count; return true;
            case 1: out.data_ = SerumValue::Negative{count}; return true;
            case 2:
            {
                if (count > bytes_.size() - position_) return fail("truncated CBOR byte string");
                const auto length = static_cast<size_t>(count);
                if (! spend(length)) return false;
                const auto s = bytes_.subspan(position_, length);
                out.data_ = std::vector<uint8_t>(s.begin(), s.end());
                position_ += length;
                return true;
            }
            case 3:
            {
                std::string s;
                if (! text(s, count)) return false;
                out.data_ = std::move(s);
                return true;
            }
            case 4:
            {
                if (count > bytes_.size() - position_) return fail("CBOR array length exceeds available data");
                if (count > SerumState::maxNodes) return fail("CBOR array node limit exceeded");
                // Charge capacity before reserving, not just after elements have been read.
                if (! spend(static_cast<size_t>(count) * sizeof(SerumValue))) return false;
                SerumValue::Array values;
                values.reserve(static_cast<size_t>(count));
                for (uint64_t i = 0; i < count; ++i)
                {
                    SerumValue child;
                    if (! value(child, depth + 1)) return false;
                    values.push_back(std::move(child));
                }
                out.data_ = std::move(values);
                return true;
            }
            case 5:
            {
                if (count > (bytes_.size() - position_) / 2) return fail("CBOR map length exceeds available data");
                if (count > SerumState::maxNodes) return fail("CBOR map node limit exceeded");
                if (! spend(static_cast<size_t>(count) * (sizeof(SerumValue::Map::value_type) + 4 * sizeof(void*)))) return false;
                SerumValue::Map members;
                for (uint64_t i = 0; i < count; ++i)
                {
                    if (position_ >= bytes_.size()) return fail("truncated CBOR map key");
                    const uint8_t keyHeader = bytes_[position_++];
                    if ((keyHeader >> 5) != 3) return fail("CBOR map key must be text");
                    uint64_t keyLength = 0;
                    if (! argument(keyHeader & 0x1f, keyLength)) return false;
                    std::string key;
                    if (! text(key, keyLength)) return false;
                    if (members.find(key) != members.end()) return fail("duplicate CBOR map key");
                    SerumValue child;
                    if (! value(child, depth + 1)) return false;
                    members.emplace(std::move(key), std::move(child));
                }
                out.data_ = std::move(members);
                return true;
            }
            case 7:
            {
                if (extra == 20 || extra == 21) { out.data_ = extra == 21; return true; }
                if (extra == 22) { out.data_ = std::monostate{}; return true; }
                double number = 0;
                if (extra == 25)
                {
                    const auto half = static_cast<uint16_t>(count);
                    const unsigned exponent = (half >> 10) & 31u, mantissa = half & 1023u;
                    if (exponent == 31) return fail("nonfinite CBOR number");
                    number = exponent == 0 ? std::ldexp(static_cast<double>(mantissa), -24)
                                           : std::ldexp(static_cast<double>(1024u + mantissa), static_cast<int>(exponent) - 25);
                    if ((half & 0x8000u) != 0) number = -number;
                }
                else if (extra == 26) number = static_cast<double>(std::bit_cast<float>(static_cast<uint32_t>(count)));
                else if (extra == 27) number = std::bit_cast<double>(count);
                else return fail("unsupported CBOR simple value");
                if (! std::isfinite(number)) return fail("nonfinite CBOR number");
                out.data_ = number;
                return true;
            }
            default: return fail("CBOR semantic tags are unsupported");
        }
    }
    std::span<const uint8_t> bytes_;
    size_t position_ = 0;
};

class JsonReader final : private Limits
{
public:
    explicit JsonReader(std::span<const uint8_t> bytes) : bytes_(bytes) {}
    bool read(SerumValue& out)
    {
        if (! validUtf8(bytes_)) return fail("invalid UTF-8 in metadata JSON");
        if (! value(out, 0)) return false;
        whitespace();
        return position_ == bytes_.size() || fail("trailing metadata JSON data");
    }
    std::string error() const { return "metadata JSON at byte " + std::to_string(position_) + ": " + error_; }
private:
    void whitespace()
    {
        while (position_ < bytes_.size())
        {
            const auto c = bytes_[position_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++position_;
        }
    }
    bool take(uint8_t c)
    {
        whitespace();
        if (position_ == bytes_.size() || bytes_[position_] != c) return false;
        ++position_;
        return true;
    }
    bool literal(std::string_view s)
    {
        if (s.size() > bytes_.size() - position_) return fail("truncated metadata JSON literal");
        for (const char c : s)
            if (bytes_[position_++] != static_cast<uint8_t>(c)) return fail("invalid metadata JSON literal");
        return true;
    }
    bool hex4(uint32_t& value)
    {
        if (bytes_.size() - position_ < 4) return fail("truncated JSON Unicode escape");
        value = 0;
        for (int i = 0; i < 4; ++i)
        {
            const uint8_t c = bytes_[position_++];
            unsigned digit = 0;
            if (c >= '0' && c <= '9') digit = c - '0';
            else if (c >= 'a' && c <= 'f') digit = c - 'a' + 10u;
            else if (c >= 'A' && c <= 'F') digit = c - 'A' + 10u;
            else return fail("invalid JSON Unicode escape");
            value = (value << 4) | digit;
        }
        return true;
    }
    void unicode(std::string& out, uint32_t point)
    {
        if (point < 0x80) out.push_back(static_cast<char>(point));
        else if (point < 0x800)
        {
            out.push_back(static_cast<char>(0xc0u | (point >> 6)));
            out.push_back(static_cast<char>(0x80u | (point & 0x3fu)));
        }
        else if (point < 0x10000)
        {
            out.push_back(static_cast<char>(0xe0u | (point >> 12)));
            out.push_back(static_cast<char>(0x80u | ((point >> 6) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | (point & 0x3fu)));
        }
        else
        {
            out.push_back(static_cast<char>(0xf0u | (point >> 18)));
            out.push_back(static_cast<char>(0x80u | ((point >> 12) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | ((point >> 6) & 0x3fu)));
            out.push_back(static_cast<char>(0x80u | (point & 0x3fu)));
        }
    }
    bool string(std::string& out)
    {
        if (! take('"')) return fail("metadata JSON string expected");
        while (position_ < bytes_.size())
        {
            const uint8_t c = bytes_[position_++];
            if (c == '"') return spend(out.size() + 1);
            if (c < 0x20) return fail("unescaped control byte in JSON string");
            if (c != '\\') { out.push_back(static_cast<char>(c)); continue; }
            if (position_ == bytes_.size()) return fail("truncated JSON escape");
            const uint8_t escape = bytes_[position_++];
            switch (escape)
            {
                case '"': case '\\': case '/': out.push_back(static_cast<char>(escape)); break;
                case 'b': out.push_back('\b'); break;
                case 'f': out.push_back('\f'); break;
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case 'u':
                {
                    uint32_t point = 0;
                    if (! hex4(point)) return false;
                    if (point >= 0xd800 && point <= 0xdbff)
                    {
                        if (bytes_.size() - position_ < 6 || bytes_[position_] != '\\' || bytes_[position_ + 1] != 'u')
                            return fail("unpaired high surrogate in JSON string");
                        position_ += 2;
                        uint32_t low = 0;
                        if (! hex4(low)) return false;
                        if (low < 0xdc00 || low > 0xdfff) return fail("invalid low surrogate in JSON string");
                        point = 0x10000 + ((point - 0xd800) << 10) + low - 0xdc00;
                    }
                    else if (point >= 0xdc00 && point <= 0xdfff) return fail("unpaired low surrogate in JSON string");
                    unicode(out, point);
                    break;
                }
                default: return fail("invalid JSON escape");
            }
        }
        return fail("unterminated JSON string");
    }
    bool value(SerumValue& out, int depth)
    {
        if (! node(depth)) return false;
        whitespace();
        if (position_ == bytes_.size()) return fail("truncated metadata JSON value");
        const uint8_t c = bytes_[position_];
        if (c == '"')
        {
            std::string s;
            if (! string(s)) return false;
            out.data_ = std::move(s);
            return true;
        }
        if (c == '{')
        {
            ++position_;
            SerumValue::Map members;
            if (! take('}'))
            {
                for (;;)
                {
                    std::string key;
                    if (! string(key) || ! take(':')) return fail("invalid metadata JSON member");
                    if (members.find(key) != members.end()) return fail("duplicate metadata JSON key");
                    if (! spend(sizeof(SerumValue::Map::value_type) + 4 * sizeof(void*))) return false;
                    SerumValue child;
                    if (! value(child, depth + 1)) return false;
                    members.emplace(std::move(key), std::move(child));
                    if (take('}')) break;
                    if (! take(',')) return fail("metadata JSON comma expected");
                }
            }
            out.data_ = std::move(members);
            return true;
        }
        if (c == '[')
        {
            ++position_;
            SerumValue::Array values;
            if (! take(']'))
            {
                for (;;)
                {
                    // Geometric vector growth uses less than two times the element storage.
                    if (! spend(2 * sizeof(SerumValue))) return false;
                    SerumValue child;
                    if (! value(child, depth + 1)) return false;
                    values.push_back(std::move(child));
                    if (take(']')) break;
                    if (! take(',')) return fail("metadata JSON comma expected");
                }
            }
            out.data_ = std::move(values);
            return true;
        }
        if (c == 't' || c == 'f')
        {
            if (! literal(c == 't' ? "true" : "false")) return false;
            out.data_ = c == 't';
            return true;
        }
        if (c == 'n') return literal("null");
        const size_t start = position_;
        if (c == '-') ++position_;
        if (position_ == bytes_.size()) return fail("truncated metadata JSON number");
        auto digit = [](uint8_t d) { return d >= '0' && d <= '9'; };
        if (bytes_[position_] == '0') ++position_;
        else
        {
            if (bytes_[position_] < '1' || bytes_[position_] > '9') return fail("invalid metadata JSON value");
            while (position_ < bytes_.size() && digit(bytes_[position_])) ++position_;
        }
        if (position_ < bytes_.size() && bytes_[position_] == '.')
        {
            ++position_;
            const size_t fraction = position_;
            while (position_ < bytes_.size() && digit(bytes_[position_])) ++position_;
            if (position_ == fraction) return fail("JSON fraction requires digits");
        }
        if (position_ < bytes_.size() && (bytes_[position_] == 'e' || bytes_[position_] == 'E'))
        {
            ++position_;
            if (position_ < bytes_.size() && (bytes_[position_] == '+' || bytes_[position_] == '-')) ++position_;
            const size_t exponent = position_;
            while (position_ < bytes_.size() && digit(bytes_[position_])) ++position_;
            if (position_ == exponent) return fail("JSON exponent requires digits");
        }
        const auto* begin = reinterpret_cast<const char*>(bytes_.data() + start);
        const auto* end = reinterpret_cast<const char*>(bytes_.data() + position_);
        if (std::find(begin, end, '.') == end && std::find(begin, end, 'e') == end && std::find(begin, end, 'E') == end)
        {
            const bool negative = *begin == '-';
            uint64_t integer = 0;
            const auto converted = std::from_chars(begin + (negative ? 1 : 0), end, integer);
            if (converted.ec != std::errc{} || converted.ptr != end) return fail("JSON integer exceeds exact supported range");
            if (negative && integer > 0) out.data_ = SerumValue::Negative{integer - 1};
            else if (negative) out.data_ = -0.0;
            else out.data_ = integer;
            return true;
        }
        // libc++ floating from_chars needs macOS 26; the project supports macOS 13.
        // Parse the already checked JSON grammar with an explicit C locale, without changing thread/process locale.
        if (! spend(static_cast<size_t>(end - begin) + 1)) return false;
        const std::string token(begin, end);
        const platform::CNumericLocale cLocale;
        if (! cLocale.valid()) return fail("unable to create metadata numeric locale");
        char* convertedEnd = nullptr;
        errno = 0;
        const double number = cLocale.strtod(token.c_str(), &convertedEnd);
        const int conversionError = errno;
        if (conversionError == ERANGE || convertedEnd != token.c_str() + token.size() || ! std::isfinite(number))
            return fail("invalid or nonfinite JSON number");
        out.data_ = number;
        return true;
    }
    std::span<const uint8_t> bytes_;
    size_t position_ = 0;
};
} // namespace serumStateDetail

namespace
{
SerumStateResult failure(std::string error) { return {nullptr, std::move(error)}; }

bool validateIdentity(const SerumValue& value, std::string& error, SerumDocumentKind kind = SerumDocumentKind::HostedState)
{
    if (value.map() == nullptr) { error = "Serum state identity must be a map"; return false; }
    const auto* product = value.find("product");
    if (product == nullptr || product->text() == nullptr || (*product->text() != "Serum2" && *product->text() != "Serum2FX"))
    { error = "unsupported Serum state product"; return false; }
    const auto* version = value.find("version");
    if (kind == SerumDocumentKind::HostedState)
    {
        const auto* component = value.find("component");
        if (component == nullptr || component->text() == nullptr || (*component->text() != "processor" && *component->text() != "controller"))
        { error = "unsupported Serum state component"; return false; }
        if (version == nullptr || version->number() != std::optional<double>{9.0})
        { error = "unsupported Serum state format version"; return false; }
    }
    else
    {
        const auto* fileType = value.find("fileType");
        if (fileType == nullptr || fileType->text() == nullptr || *fileType->text() != "SerumPreset")
        { error = "unsupported Serum preset file type"; return false; }
        const auto number = version != nullptr ? version->number() : std::optional<double>();
        if (! number || *number < 3 || *number > 7 || std::floor(*number) < *number)
        { error = "unsupported Serum preset format version"; return false; }
    }
    const auto* productVersion = value.find("productVersion");
    if (productVersion == nullptr || productVersion->text() == nullptr || productVersion->text()->empty() || productVersion->text()->size() > 64)
    { error = "invalid Serum state productVersion"; return false; }
    // Accept dotted unsigned version components. Minor versions are evidence, not a DSP-compatibility promise.
    bool digit = false;
    for (const char c : *productVersion->text())
    {
        if (c >= '0' && c <= '9') digit = true;
        else if (c == '.' && digit) digit = false;
        else { error = "invalid Serum state productVersion"; return false; }
    }
    if (! digit) { error = "invalid Serum state productVersion"; return false; }
    return true;
}

uint64_t littleEndian(std::span<const uint8_t> bytes)
{
    uint64_t value = 0;
    for (size_t i = 0; i < bytes.size(); ++i) value |= static_cast<uint64_t>(bytes[i]) << (8 * i);
    return value;
}
} // namespace

SerumStateResult decodeSerumCbor(std::span<const uint8_t> bytes)
{
    if (bytes.empty() || bytes.size() > SerumState::maxDecodedBytes) return failure("CBOR decoded size limit exceeded or empty data");
    try
    {
        auto state = std::shared_ptr<SerumState>(new SerumState);
        serumStateDetail::CborReader reader(bytes);
        if (! reader.read(state->root_)) return failure(reader.error());
        std::string error;
        if (! validateIdentity(state->root_, error)) return failure(std::move(error));
        state->original_.assign(bytes.begin(), bytes.end());
        state->decoded_.assign(bytes.begin(), bytes.end());
        return {std::move(state), {}};
    }
    catch (const std::bad_alloc&) { return failure("Serum state allocation failed"); }
    catch (const std::length_error&) { return failure("Serum state allocation length exceeded"); }
}

SerumStateResult decodeSerumState(std::span<const uint8_t> bytes, SerumDocumentKind kind)
{
    constexpr size_t headerBytes = 17, encodingBytes = 8;
    constexpr std::string_view magic = "XferJson";
    if (bytes.size() < headerBytes + encodingBytes || ! std::equal(magic.begin(), magic.end(), bytes.begin()) || bytes[8] != 0)
        return failure("missing or truncated XferJson header");
    if (bytes.size() > SerumState::maxDecodedBytes + SerumState::maxMetadataBytes + headerBytes + encodingBytes)
        return failure("Serum container size limit exceeded");
    const uint64_t jsonBytes = littleEndian(bytes.subspan(9, 8));
    if (jsonBytes == 0 || jsonBytes > SerumState::maxMetadataBytes || jsonBytes > bytes.size() - headerBytes - encodingBytes)
        return failure("invalid or excessive Serum metadata length");
    const size_t payloadHeader = headerBytes + static_cast<size_t>(jsonBytes);
    const uint64_t decodedBytes = littleEndian(bytes.subspan(payloadHeader, 4));
    if (decodedBytes == 0 || decodedBytes > SerumState::maxDecodedBytes) return failure("Serum decoded size limit exceeded or empty data");
    if (littleEndian(bytes.subspan(payloadHeader + 4, 4)) != 2) return failure("unsupported Serum state encoding");
    const auto compressed = bytes.subspan(payloadHeader + encodingBytes);
    if (compressed.empty()) return failure("missing Serum Zstandard frame");
    try
    {
        auto state = std::shared_ptr<SerumState>(new SerumState);
        serumStateDetail::JsonReader json(bytes.subspan(headerBytes, static_cast<size_t>(jsonBytes)));
        if (! json.read(state->metadata_)) return failure(json.error());
        std::string error;
        if (! validateIdentity(state->metadata_, error, kind)) return failure("metadata: " + error);
        const auto frameBytes = ZSTD_findFrameCompressedSize(compressed.data(), compressed.size());
        if (ZSTD_isError(frameBytes)) return failure(std::string("invalid Serum Zstandard frame: ") + ZSTD_getErrorName(frameBytes));
        if (frameBytes != compressed.size()) return failure("trailing data or concatenated Serum Zstandard frames");
        const auto frameSize = ZSTD_getFrameContentSize(compressed.data(), compressed.size());
        if (frameSize == ZSTD_CONTENTSIZE_ERROR) return failure("invalid Serum Zstandard content size");
        if (frameSize != ZSTD_CONTENTSIZE_UNKNOWN && frameSize != decodedBytes) return failure("Serum declared size disagrees with Zstandard frame");
        state->decoded_.resize(static_cast<size_t>(decodedBytes));
        const std::unique_ptr<ZSTD_DCtx, decltype(&ZSTD_freeDCtx)> context(ZSTD_createDCtx(), &ZSTD_freeDCtx);
        if (! context) return failure("Serum Zstandard context allocation failed");
        const auto limitResult = ZSTD_DCtx_setParameter(context.get(), ZSTD_d_windowLogMax, 26);
        if (ZSTD_isError(limitResult)) return failure("unable to bound Serum Zstandard window");
        // Streaming decoding honors windowLogMax even when the frame does not advertise its content size.
        ZSTD_inBuffer input{compressed.data(), compressed.size(), 0};
        ZSTD_outBuffer output{state->decoded_.data(), state->decoded_.size(), 0};
        for (;;)
        {
            const size_t beforeInput = input.pos, beforeOutput = output.pos;
            const auto result = ZSTD_decompressStream(context.get(), &output, &input);
            if (ZSTD_isError(result)) return failure(std::string("Serum Zstandard decompression failed: ") + ZSTD_getErrorName(result));
            if (result == 0) break;
            if (output.pos == output.size) return failure("Serum decoded size exceeds declared size");
            if (input.pos == beforeInput && output.pos == beforeOutput) return failure("truncated Serum Zstandard frame");
        }
        if (input.pos != input.size || output.pos != decodedBytes) return failure("Serum decoded size disagrees with declared size");
        serumStateDetail::CborReader cbor(state->decoded_);
        if (! cbor.read(state->root_)) return failure(cbor.error());
        if (! validateIdentity(state->root_, error, kind)) return failure("CBOR: " + error);
        const auto identityKey = kind == SerumDocumentKind::Preset ? "fileType" : "component";
        for (const auto* key : {"product", identityKey, "productVersion"})
            if (*state->metadata_.find(key)->text() != *state->root_.find(key)->text()) return failure(std::string("Serum metadata/CBOR mismatch: ") + key);
        if (state->metadata_.find("version")->number() != state->root_.find("version")->number())
            return failure("Serum metadata/CBOR mismatch: version");
        state->original_.assign(bytes.begin(), bytes.end());
        state->hasContainer_ = true;
        return {std::move(state), {}};
    }
    catch (const std::bad_alloc&) { return failure("Serum state allocation failed"); }
    catch (const std::length_error&) { return failure("Serum state allocation length exceeded"); }
}

} // namespace sonora::plugins

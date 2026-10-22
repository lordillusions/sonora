#pragma once
// Compact binary encoding primitives (COL-2): little-endian, varints, exact doubles.
//
// ByteReader is safe on untrusted input: every read is bounds-checked, element counts are checked against the
// remaining bytes before anything is allocated, and nesting depth is limited by the caller. Errors throw
// CodecError; nothing is ever read past the end.

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace sonora::model
{

struct CodecError : std::runtime_error
{
    using std::runtime_error::runtime_error;
};

class ByteWriter
{
public:
    void u8(uint8_t v) { out_.push_back(static_cast<char>(v)); }
    void boolean(bool v) { u8(v ? 1 : 0); }
    void varint(uint64_t v)
    {
        while (v >= 0x80)
        {
            u8(static_cast<uint8_t>(v | 0x80));
            v >>= 7;
        }
        u8(static_cast<uint8_t>(v));
    }
    void svarint(int64_t v) { varint((static_cast<uint64_t>(v) << 1) ^ static_cast<uint64_t>(v >> 63)); }
    void i32(int v) { svarint(v); }
    void u32fixed(uint32_t v)
    {
        for (int i = 0; i < 4; ++i) u8(static_cast<uint8_t>(v >> (8 * i)));
    }
    void f64(double v)
    {
        uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        for (int i = 0; i < 8; ++i) u8(static_cast<uint8_t>(bits >> (8 * i)));
    }
    void f32(float v)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        u32fixed(bits);
    }
    void str(std::string_view s)
    {
        varint(s.size());
        out_.append(s.data(), s.size());
    }
    void bytes(const std::vector<uint8_t>& b)
    {
        varint(b.size());
        out_.append(reinterpret_cast<const char*>(b.data()), b.size());
    }
    void raw(std::string_view s) { out_.append(s.data(), s.size()); }

    const std::string& data() const { return out_; }
    std::string take() { return std::move(out_); }
    size_t size() const { return out_.size(); }
    void clear() { out_.clear(); }

private:
    std::string out_;
};

// ByteWriter's interface feeding a 64-bit hash instead of bytes (COL-2b): every value is mixed as one word, strings
// and byte blocks as their length then 8-byte words, so hashing an encoding allocates nothing. Equal value sequences
// give equal hashes; different ones (of the same schema) differ except for 64-bit collisions. Each step is a bijection
// of the state, so a single changed value always changes the hash. Not cryptographic and not portable: compare only
// hashes made by the same process.
class HashWriter
{
public:
    void u8(uint8_t v) { mix(v); }
    void boolean(bool v) { mix(v ? 1 : 0); }
    void varint(uint64_t v) { mix(v); }
    void svarint(int64_t v) { mix(static_cast<uint64_t>(v)); }
    void i32(int v) { svarint(v); }
    void u32fixed(uint32_t v) { mix(v); }
    void f64(double v)
    {
        uint64_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        mix(bits);
    }
    void f32(float v)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, sizeof bits);
        mix(bits);
    }
    void str(std::string_view s)
    {
        mix(s.size());
        words(s.data(), s.size());
    }
    void bytes(const std::vector<uint8_t>& b)
    {
        mix(b.size());
        words(reinterpret_cast<const char*>(b.data()), b.size());
    }

    // Murmur3's finaliser: spreads every state bit over the result (a bijection, so no collisions are added).
    uint64_t value() const
    {
        uint64_t h = h_;
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdull;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ull;
        h ^= h >> 33;
        return h;
    }

private:
    void mix(uint64_t v)
    {
        h_ = (h_ ^ v) * 0x9e3779b97f4a7c15ull;
        h_ ^= h_ >> 32; // carries high bits down, which the multiply alone never does
    }
    void words(const char* p, size_t n)
    {
        for (; n >= 8; p += 8, n -= 8)
        {
            uint64_t w = 0;
            std::memcpy(&w, p, 8);
            mix(w);
        }
        if (n > 0)
        {
            uint64_t w = 0;
            std::memcpy(&w, p, n);
            mix(w);
        }
    }
    uint64_t h_ = 0x243f6a8885a308d3ull;
};

class ByteReader
{
public:
    ByteReader(const char* data, size_t size) : p_(reinterpret_cast<const uint8_t*>(data)), end_(p_ + size) {}
    explicit ByteReader(std::string_view s) : ByteReader(s.data(), s.size()) {}

    uint8_t u8()
    {
        need(1);
        return *p_++;
    }
    bool boolean()
    {
        const auto v = u8();
        if (v > 1) throw CodecError("invalid boolean");
        return v == 1;
    }
    uint64_t varint()
    {
        uint64_t v = 0;
        for (int shift = 0; shift < 64; shift += 7)
        {
            const uint8_t b = u8();
            v |= static_cast<uint64_t>(b & 0x7f) << shift;
            if ((b & 0x80) == 0) return v;
        }
        throw CodecError("varint too long");
    }
    int64_t svarint()
    {
        const uint64_t v = varint();
        return static_cast<int64_t>((v >> 1) ^ (~(v & 1) + 1));
    }
    int i32()
    {
        const int64_t v = svarint();
        if (v < INT32_MIN || v > INT32_MAX) throw CodecError("integer out of range");
        return static_cast<int>(v);
    }
    uint32_t u32fixed()
    {
        need(4);
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(p_[i]) << (8 * i);
        p_ += 4;
        return v;
    }
    double f64()
    {
        need(8);
        uint64_t bits = 0;
        for (int i = 0; i < 8; ++i) bits |= static_cast<uint64_t>(p_[i]) << (8 * i);
        p_ += 8;
        double v = 0;
        std::memcpy(&v, &bits, sizeof v);
        return v;
    }
    float f32()
    {
        const uint32_t bits = u32fixed();
        float v = 0;
        std::memcpy(&v, &bits, sizeof v);
        return v;
    }
    // A count of elements that each take at least `minBytes` bytes: rejects counts the input cannot hold.
    size_t count(size_t minBytes = 1)
    {
        const uint64_t n = varint();
        if (minBytes > 0 && n > remaining() / minBytes) throw CodecError("element count exceeds input");
        return static_cast<size_t>(n);
    }
    std::string str(size_t maxBytes = 256u * 1024u * 1024u)
    {
        const size_t n = count(1);
        if (n > maxBytes) throw CodecError("string too long");
        std::string s(reinterpret_cast<const char*>(p_), n);
        p_ += n;
        return s;
    }
    std::string_view view(size_t n)
    {
        need(n);
        std::string_view s(reinterpret_cast<const char*>(p_), n);
        p_ += n;
        return s;
    }
    std::vector<uint8_t> bytes(size_t maxBytes = 256u * 1024u * 1024u)
    {
        const size_t n = count(1);
        if (n > maxBytes) throw CodecError("byte block too long");
        std::vector<uint8_t> b(p_, p_ + n);
        p_ += n;
        return b;
    }

    size_t remaining() const { return static_cast<size_t>(end_ - p_); }
    bool atEnd() const { return p_ == end_; }
    void expectEnd() const
    {
        if (! atEnd()) throw CodecError("trailing bytes");
    }

private:
    void need(size_t n) const
    {
        if (n > remaining()) throw CodecError("truncated input");
    }
    const uint8_t* p_;
    const uint8_t* end_;
};

// 64-bit FNV-1a, for content addressing small objects (not cryptographic).
inline uint64_t fnv1a64(std::string_view s, uint64_t h = 1469598103934665603ull)
{
    for (const char c : s)
    {
        h ^= static_cast<uint8_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

} // namespace sonora::model

#pragma once
// PLG-25a: non-parameter state of a generator device (step grids, progressions, forms) handed from the message thread to
// the audio thread without locks or allocation.
//
// The state is a plain fixed-size struct T (trivially copyable: no strings, no vectors). PatternSlot<T> keeps the
// message thread's working copy and a triple buffer:
//   message thread: edit() / set() change the working copy, publish() makes it the newest version;
//   audio thread:   update() at block start takes the newest published version (true when there was one), live() reads it.
// Wait-free on both sides, no torn reads (the audio thread never reads a buffer the message thread writes), no
// allocation after construction. Several publishes between two blocks: the audio thread sees the last one.
//
// Saving: the device encodes the working copy field by field with StateWriter / StateReader (below) into
// model::NativeContent (MidiGenerator::saveState / loadState) and the VST3 state. Never save the raw struct bytes: the
// layout changes between versions.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>
#include <type_traits>
#include <vector>

namespace sonora::devices
{
template <class T> class PatternSlot
{
    static_assert(std::is_trivially_copyable_v<T>, "PatternSlot holds plain fixed-size structs");

public:
    PatternSlot() : PatternSlot(T{}) {}
    explicit PatternSlot(const T& initial) : working_(initial), buffers_{initial, initial, initial} {}

    // ---- message thread
    const T& edited() const { return working_; }
    T& edit() { return working_; } // call publish() when done
    void set(const T& value)
    {
        working_ = value;
        publish();
    }
    void publish()
    {
        buffers_[back_] = working_;
        const int previous = middle_.exchange(back_ | kFresh, std::memory_order_acq_rel);
        back_ = previous & kIndex;
    }

    // ---- audio thread
    // Takes the newest published version; true when one was published since the last call (live() then returns it).
    bool update()
    {
        if ((middle_.load(std::memory_order_acquire) & kFresh) == 0) return false;
        const int previous = middle_.exchange(front_, std::memory_order_acq_rel);
        front_ = previous & kIndex;
        return true;
    }
    const T& live() const { return buffers_[front_]; }

private:
    static constexpr int kFresh = 4, kIndex = 3;
    T working_;
    T buffers_[3];
    int back_ = 0;                // message thread
    int front_ = 1;               // audio thread
    std::atomic<int> middle_{2};  // shared: index | kFresh
};

// Little-endian field writer / bounds-checked reader for saved generator state. Message thread.
class StateWriter
{
public:
    void u8(uint8_t v) { bytes_.push_back(v); }
    void u16(uint16_t v) { for (int i = 0; i < 2; ++i) bytes_.push_back((uint8_t) (v >> (8 * i))); }
    void u32(uint32_t v) { for (int i = 0; i < 4; ++i) bytes_.push_back((uint8_t) (v >> (8 * i))); }
    void i32(int32_t v) { u32((uint32_t) v); }
    void f32(float v)
    {
        uint32_t bits = 0;
        std::memcpy(&bits, &v, 4);
        u32(bits);
    }
    void f64(double v)
    {
        uint64_t bits = 0;
        std::memcpy(&bits, &v, 8);
        u32((uint32_t) bits);
        u32((uint32_t) (bits >> 32));
    }
    void str(const std::string& s)
    {
        u32((uint32_t) s.size());
        bytes_.insert(bytes_.end(), s.begin(), s.end());
    }
    const std::vector<uint8_t>& bytes() const { return bytes_; }
    std::vector<uint8_t> take() { return std::move(bytes_); }

private:
    std::vector<uint8_t> bytes_;
};

class StateReader
{
public:
    StateReader(const uint8_t* data, size_t size) : data_(data), size_(data ? size : 0) {}
    bool ok() const { return ok_; }          // false after any read past the end
    bool atEnd() const { return pos_ >= size_; }
    size_t remaining() const { return size_ - pos_; }
    uint8_t u8() { return need(1) ? data_[pos_++] : 0; }
    uint16_t u16()
    {
        if (! need(2)) return 0;
        const uint16_t v = (uint16_t) (data_[pos_] | (data_[pos_ + 1] << 8));
        pos_ += 2;
        return v;
    }
    uint32_t u32()
    {
        if (! need(4)) return 0;
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) v |= (uint32_t) data_[pos_ + (size_t) i] << (8 * i);
        pos_ += 4;
        return v;
    }
    int32_t i32() { return (int32_t) u32(); }
    float f32()
    {
        const uint32_t bits = u32();
        float v = 0;
        std::memcpy(&v, &bits, 4);
        return v;
    }
    double f64()
    {
        const uint64_t lo = u32(), hi = u32();
        const uint64_t bits = lo | (hi << 32);
        double v = 0;
        std::memcpy(&v, &bits, 8);
        return v;
    }
    std::string str(size_t maxLength = 4096)
    {
        const uint32_t n = u32();
        if (n > maxLength || ! need(n)) { ok_ = false; return {}; }
        std::string s(reinterpret_cast<const char*>(data_ + pos_), n);
        pos_ += n;
        return s;
    }

private:
    bool need(size_t n)
    {
        if (! ok_ || pos_ + n > size_) { ok_ = false; return false; }
        return true;
    }
    const uint8_t* data_;
    size_t size_, pos_ = 0;
    bool ok_ = true;
};
} // namespace sonora::devices

#pragma once
// Minimal in-place radix-2 complex FFT for the analysers (not for the audio thread: it allocates twiddles
// once in the constructor, then a transform is allocation free).

#include <cmath>
#include <complex>
#include <vector>

namespace sonora::dsp
{

class Fft
{
public:
    explicit Fft(int sizeLog2) : size_(1 << sizeLog2), twiddle_((size_t) size_ / 2), bitrev_((size_t) size_)
    {
        for (int i = 0; i < size_ / 2; ++i)
            twiddle_[(size_t) i] = std::polar(1.0f, -2.0f * 3.14159265358979f * (float) i / (float) size_);
        for (int i = 0; i < size_; ++i)
        {
            int r = 0;
            for (int b = 0; b < sizeLog2; ++b)
                if (i & (1 << b))
                    r |= 1 << (sizeLog2 - 1 - b);
            bitrev_[(size_t) i] = r;
        }
    }

    int size() const { return size_; }

    void transform(std::vector<std::complex<float>>& data) const
    {
        for (int i = 0; i < size_; ++i)
            if (const int j = bitrev_[(size_t) i]; j > i)
                std::swap(data[(size_t) i], data[(size_t) j]);
        for (int len = 2; len <= size_; len <<= 1)
        {
            const int half = len / 2, step = size_ / len;
            for (int base = 0; base < size_; base += len)
                for (int k = 0; k < half; ++k)
                {
                    const auto t = data[(size_t) (base + k + half)] * twiddle_[(size_t) (k * step)];
                    const auto u = data[(size_t) (base + k)];
                    data[(size_t) (base + k)] = u + t;
                    data[(size_t) (base + k + half)] = u - t;
                }
        }
    }

private:
    int size_;
    std::vector<std::complex<float>> twiddle_;
    std::vector<int> bitrev_;
};

} // namespace sonora::dsp

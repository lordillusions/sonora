#pragma once

namespace sonora::dsp
{

// Fixed per-unison octave-bank transition (DEV-18d). A bank change retains
// its preceding level for 64 internal samples, independently of host blocks.
class WavetableMipTransition final
{
public:
    static constexpr int length = 64;
    void reset() noexcept { current_ = previous_ = -1; remaining_ = 0; }
    void select(int level) noexcept
    {
        if (current_ < 0) { current_ = previous_ = level; return; }
        if (level == current_) return;
        previous_ = current_; current_ = level; remaining_ = length;
    }
    int current() const noexcept { return current_; }
    int previous() const noexcept { return previous_; }
    bool active() const noexcept { return remaining_ > 0; }
    float blend(float current, float previous) const noexcept
    {
        return remaining_ > 0 ? current + static_cast<float>(remaining_) / length * (previous - current) : current;
    }
    void advance() noexcept { if (remaining_ > 0) --remaining_; }

private:
    int current_ = -1, previous_ = -1, remaining_ = 0;
};

} // namespace sonora::dsp

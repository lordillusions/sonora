#include "devices/native/WavetableVoiceMode.h"

#include <algorithm>
#include <cmath>

namespace sonora::devices
{
float WavetableVoiceMode::observedPortamentoSeconds(float normalized)
{
    const float u = std::isfinite(normalized) ? std::clamp(normalized, 0.0f, 1.0f) : 0;
    const float squared = u * u;
    return 8 * squared * squared * u;
}
void WavetableVoiceMode::PitchGlide::prepare(double sampleRate)
{
    sampleRate_ = std::isfinite(sampleRate) ? std::clamp(sampleRate, 8000.0, 384000.0) : 44100;
    reset(current_);
}
void WavetableVoiceMode::PitchGlide::reset(double pitch)
{
    current_ = std::isfinite(pitch) ? std::clamp(pitch, -256.0, 256.0) : 60;
    from_ = target_ = current_; elapsed_ = duration_ = 0;
}
bool WavetableVoiceMode::PitchGlide::start(double fromPitch, double targetPitch, double seconds, float curve, bool exponential)
{
    if (! std::isfinite(fromPitch) || ! std::isfinite(targetPitch) || ! std::isfinite(seconds) || ! std::isfinite(curve)) return false;
    from_ = current_ = std::clamp(fromPitch, -256.0, 256.0);
    target_ = std::clamp(targetPitch, -256.0, 256.0);
    duration_ = static_cast<uint64_t>(std::llround(std::clamp(seconds, 0.0, 128.0) * sampleRate_));
    elapsed_ = 0;
    curveFactor_ = std::exp2(std::clamp(static_cast<double>(curve), -100.0, 100.0) / 25);
    exponential_ = exponential;
    exponentialCurve_ = dsp::ExponentialEnvelopeCurve::fromTension(std::clamp(curve / 100, -1.0f, 1.0f));
    if (duration_ == 0 || std::abs(target_ - from_) < 1e-12) reset(target_);
    return true;
}
double WavetableVoiceMode::PitchGlide::next()
{
    if (! active()) return current_;
    if (++elapsed_ == duration_) { current_ = target_; return current_; }
    const double phase = static_cast<double>(elapsed_) / static_cast<double>(duration_);
    const double shaped = exponential_ ? exponentialCurve_.eval(static_cast<float>(phase))
                                      : phase / (phase + (1 - phase) * curveFactor_);
    current_ = from_ + (target_ - from_) * shaped;
    return current_;
}
void WavetableVoiceMode::prepare(double sampleRate)
{
    glide_.prepare(sampleRate); reset();
}
void WavetableVoiceMode::reset()
{
    notes_ = {}; pedals_ = {}; selected_ = {}; age_ = 0; pitchHistory_ = false;
    glide_.reset();
}
WavetableVoiceMode::Transition WavetableVoiceMode::configure(const Settings& settings)
{
    settings_ = settings;
    settings_.portaSeconds = std::isfinite(settings.portaSeconds) ? std::clamp(settings.portaSeconds, 0.0, 8.0) : 0;
    settings_.portaCurve = std::isfinite(settings.portaCurve) ? std::clamp(settings.portaCurve, -100.0f, 100.0f) : 0;
    if (settings_.priority != Priority::Last && settings_.priority != Priority::Low && settings_.priority != Priority::High) settings_.priority = Priority::Last;
    return update();
}
WavetableVoiceMode::Transition WavetableVoiceMode::unchanged() const
{
    Transition result; result.before = result.after = selected_; return result;
}
bool WavetableVoiceMode::preferred(const Selection& candidate, const Selection& current) const
{
    if (! current.active()) return true;
    if (settings_.priority == Priority::Low && candidate.note != current.note) return candidate.note < current.note;
    if (settings_.priority == Priority::High && candidate.note != current.note) return candidate.note > current.note;
    return candidate.age > current.age;
}
WavetableVoiceMode::Selection WavetableVoiceMode::selection(int channel) const
{
    Selection result;
    if (! validChannel(channel)) return result;
    for (const auto& note : notes_[static_cast<size_t>(channel)])
        if (note.note >= 0)
        {
            Selection candidate{note.note, channel, note.velocity, note.age, note.keyDown};
            if (preferred(candidate, result)) result = candidate;
        }
    return result;
}
WavetableVoiceMode::Transition WavetableVoiceMode::update()
{
    Transition result; result.before = selected_;
    Selection next;
    for (int channel = 0; channel < channels; ++channel)
    {
        const auto candidate = selection(channel);
        if (candidate.active() && preferred(candidate, next)) next = candidate;
    }
    result.after = next;
    result.identityChanged = next.age != selected_.age || next.channel != selected_.channel || next.note != selected_.note;
    result.changed = result.identityChanged || next.keyDown != selected_.keyDown || next.velocity != selected_.velocity;
    result.release = selected_.active() && ! next.active();
    if (result.identityChanged && next.active())
    {
        const bool overlap = selected_.active();
        result.legato = overlap && settings_.legato;
        result.retrigger = ! result.legato;
        const bool glideAllowed = overlap || (settings_.portaAlways && pitchHistory_);
        double duration = glideAllowed ? settings_.portaSeconds : 0;
        if (settings_.portaScaled) duration *= std::abs(next.note - glide_.current()) / 12;
        glide_.start(pitchHistory_ ? glide_.current() : next.note, next.note, duration, settings_.portaCurve, settings_.exponentialPortamento);
        result.portamento = glide_.active();
        pitchHistory_ = true;
    }
    else if (result.release) glide_.reset(glide_.current()); // Freeze audible pitch for a later Always glide.
    selected_ = next;
    return result;
}
WavetableVoiceMode::Transition WavetableVoiceMode::noteOn(int channel, int pitch, int velocity)
{
    if (! validChannel(channel) || ! validNote(pitch) || velocity < 0 || velocity > 127) return unchanged();
    if (velocity == 0) return noteOff(channel, pitch);
    auto& notes = notes_[static_cast<size_t>(channel)];
    Note* destination = nullptr;
    // Deduplicated (imported) stacks keep one entry per pitch; a repeated pitch re-stamps it, which re-selects it like a new note
    // (measured: the reference retriggers, or changes legato, on a repeated held pitch and one note-off then releases it).
    if (settings_.deduplicate)
        for (auto& note : notes) if (note.note == pitch) { destination = &note; break; }
    if (destination == nullptr)
        for (auto& note : notes) if (note.note < 0) { destination = &note; break; }
    if (destination == nullptr)
    {
        destination = &notes[0];
        for (auto& note : notes) if (note.age < destination->age) destination = &note;
    }
    // A 64-bit event sequence spans centuries of continuous high-rate MIDI traffic.
    *destination = {pitch, velocity, ++age_, true};
    return update();
}
WavetableVoiceMode::Transition WavetableVoiceMode::noteOff(int channel, int pitch)
{
    if (! validChannel(channel) || ! validNote(pitch)) return unchanged();
    Note* oldest = nullptr;
    for (auto& note : notes_[static_cast<size_t>(channel)])
        if (note.note == pitch && note.keyDown && (oldest == nullptr || note.age < oldest->age)) oldest = &note;
    if (oldest == nullptr) return unchanged();
    if (pedals_[static_cast<size_t>(channel)]) oldest->keyDown = false;
    else *oldest = {};
    return update();
}
WavetableVoiceMode::Transition WavetableVoiceMode::sustain(int channel, bool down)
{
    if (! validChannel(channel)) return unchanged();
    for (int index = 0; index < channels; ++index)
        if (settings_.globalSustain || index == channel)
        {
            pedals_[static_cast<size_t>(index)] = down;
            if (! down) for (auto& note : notes_[static_cast<size_t>(index)]) if (! note.keyDown) note = {};
        }
    return update();
}
WavetableVoiceMode::Transition WavetableVoiceMode::allNotesOff(int channel)
{
    if (! validChannel(channel)) return unchanged();
    for (auto& note : notes_[static_cast<size_t>(channel)])
        if (pedals_[static_cast<size_t>(channel)]) note.keyDown = false;
        else note = {};
    return update();
}
WavetableVoiceMode::Transition WavetableVoiceMode::allSoundOff(int channel)
{
    if (! validChannel(channel)) return unchanged();
    notes_[static_cast<size_t>(channel)] = {};
    return update();
}
WavetableVoiceMode::Transition WavetableVoiceMode::resetControllers(int channel) { return sustain(channel, false); }
int WavetableVoiceMode::heldCount(int channel) const
{
    if (! validChannel(channel)) return 0;
    int count = 0; for (const auto& note : notes_[static_cast<size_t>(channel)]) if (note.note >= 0) ++count; return count;
}
int WavetableVoiceMode::physicalHeldCount(int channel) const
{
    if (! validChannel(channel)) return 0;
    int count = 0; for (const auto& note : notes_[static_cast<size_t>(channel)]) if (note.note >= 0 && note.keyDown) ++count; return count;
}
bool WavetableVoiceMode::sustainDown(int channel) const { return validChannel(channel) && pedals_[static_cast<size_t>(channel)]; }
double WavetableVoiceMode::nextPitch() { return selected_.active() ? glide_.next() : glide_.current(); }
} // namespace sonora::devices

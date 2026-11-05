#include "collab/Voice.h"

#include <juce_audio_basics/juce_audio_basics.h>

#if SONORA_HAVE_OPUS
#include <opus.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>

namespace sonora::collab
{
namespace
{
int64_t nowMs() { return juce::Time::currentTimeMillis(); }
constexpr float talkLevel = 0.02f;          // about -34 dBFS peak counts as talking
constexpr double playbackLeadSeconds = 0.06; // keep this much voice queued for the audio thread
constexpr int resampleChunk = 256;           // device frames per playback write
} // namespace

bool voiceBuiltIn() { return SONORA_HAVE_OPUS != 0; }

// ------------------------------------------------------------------------------------------------ RTP

std::string encodeRtp(const RtpPacket& p)
{
    std::string out(12, '\0');
    out[0] = static_cast<char>(0x80);
    out[1] = static_cast<char>((p.marker ? 0x80 : 0) | (p.payloadType & 0x7f));
    out[2] = static_cast<char>(p.seq >> 8);
    out[3] = static_cast<char>(p.seq & 0xff);
    for (int i = 0; i < 4; ++i) out[(size_t) (4 + i)] = static_cast<char>((p.timestamp >> (24 - 8 * i)) & 0xff);
    for (int i = 0; i < 4; ++i) out[(size_t) (8 + i)] = static_cast<char>((p.ssrc >> (24 - 8 * i)) & 0xff);
    out += p.payload;
    return out;
}

std::optional<RtpPacket> decodeRtp(std::string_view b)
{
    auto byte = [&](size_t i) { return static_cast<uint8_t>(b[i]); };
    if (b.size() < 12 || (byte(0) >> 6) != 2) return std::nullopt;
    const uint8_t pt = byte(1) & 0x7f;
    if (pt >= 72 && pt <= 76) return std::nullopt; // RTCP (200-204 with the marker bit)
    size_t offset = 12 + 4u * (byte(0) & 0x0f);   // CSRCs
    if ((byte(0) & 0x10) != 0)                     // header extension
    {
        if (offset + 4 > b.size()) return std::nullopt;
        offset += 4 + 4u * (size_t) ((byte(offset + 2) << 8) | byte(offset + 3));
    }
    size_t end = b.size();
    if ((byte(0) & 0x20) != 0) // padding: the last byte counts it
    {
        const size_t pad = byte(b.size() - 1);
        if (pad == 0 || pad > end) return std::nullopt;
        end -= pad;
    }
    if (offset > end) return std::nullopt;
    RtpPacket p;
    p.marker = (byte(1) & 0x80) != 0;
    p.payloadType = pt;
    p.seq = static_cast<uint16_t>((byte(2) << 8) | byte(3));
    p.timestamp = (uint32_t) byte(4) << 24 | (uint32_t) byte(5) << 16 | (uint32_t) byte(6) << 8 | byte(7);
    p.ssrc = (uint32_t) byte(8) << 24 | (uint32_t) byte(9) << 16 | (uint32_t) byte(10) << 8 | byte(11);
    p.payload.assign(b.substr(offset, end - offset));
    return p;
}

std::optional<AuthorId> voiceSpeaker(uint32_t ssrc)
{
    if ((ssrc & ~0xffu) != voiceSsrcBase || (ssrc & 0xffu) > 63) return std::nullopt;
    return static_cast<AuthorId>(ssrc & 0xffu);
}

// ------------------------------------------------------------------------------------------------ jitter buffer

void JitterBuffer::push(uint16_t seq, std::string payload)
{
    int64_t ext = 0;
    if (last_ < 0) ext = (int64_t) seq + (1 << 20);
    else ext = last_ + (int16_t) (uint16_t) (seq - (uint16_t) (last_ & 0xffff));
    if (ext > last_) last_ = ext;
    if (playing_ && ext < next_) { ++late_; return; } // its turn has passed
    if (payload.empty()) return;
    frames_[ext] = std::move(payload);
    if (frames_.size() > maxDepth)
    {
        // Far behind (a stall or a clock running fast): skip ahead to a short queue.
        while (frames_.size() > startDepth + 1) frames_.erase(frames_.begin());
        next_ = frames_.begin()->first;
    }
}

JitterBuffer::Next JitterBuffer::pop()
{
    Next out;
    if (! playing_)
    {
        if (frames_.size() < startDepth) return out;
        playing_ = true;
        next_ = frames_.begin()->first;
        missing_ = 0;
    }
    if (auto it = frames_.find(next_); it != frames_.end())
    {
        out.kind = Next::Kind::Frame;
        out.payload = std::move(it->second);
        frames_.erase(it);
        ++next_;
        missing_ = 0;
        return out;
    }
    out.kind = Next::Kind::Missing;
    if (frames_.empty())
    {
        // Nothing has arrived yet: conceal without moving on, so the frame still plays when it comes (the delay grows
        // by a frame instead of losing it). After idleAfter frames the talk spurt is over: buffer up again.
        if (++missing_ > idleAfter)
        {
            playing_ = false;
            missing_ = 0;
            out.kind = Next::Kind::Idle;
        }
        return out;
    }
    // A gap with later frames here: that frame is lost.
    if (auto following = frames_.find(next_ + 1); following != frames_.end()) out.payload = following->second;
    ++next_;
    while (! frames_.empty() && frames_.begin()->first < next_) frames_.erase(frames_.begin());
    return out;
}

// ------------------------------------------------------------------------------------------------ codec

struct VoiceChannel::Codec
{
#if SONORA_HAVE_OPUS
    OpusEncoder* encoder = nullptr;
    Codec()
    {
        int error = 0;
        encoder = opus_encoder_create(voiceRate, 1, OPUS_APPLICATION_VOIP, &error);
        if (error != OPUS_OK) encoder = nullptr;
        if (encoder)
        {
            opus_encoder_ctl(encoder, OPUS_SET_BITRATE(voiceBitrate));
            opus_encoder_ctl(encoder, OPUS_SET_INBAND_FEC(1));
            opus_encoder_ctl(encoder, OPUS_SET_PACKET_LOSS_PERC(10));
            opus_encoder_ctl(encoder, OPUS_SET_COMPLEXITY(5));
            opus_encoder_ctl(encoder, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
        }
    }
    ~Codec()
    {
        if (encoder) opus_encoder_destroy(encoder);
    }
#endif
    juce::LagrangeInterpolator captureResampler, playbackResampler;
};

struct VoiceChannel::Speaker
{
    JitterBuffer jitter;
#if SONORA_HAVE_OPUS
    OpusDecoder* decoder = nullptr;
    Speaker()
    {
        int error = 0;
        decoder = opus_decoder_create(voiceRate, 1, &error);
        if (error != OPUS_OK) decoder = nullptr;
    }
    ~Speaker()
    {
        if (decoder) opus_decoder_destroy(decoder);
    }
#endif
    bool muted = false;
    int64_t loudMs = 0;
};

VoiceChannel::VoiceChannel(engine::VoiceTap& tap, AuthorId self, bool threaded) : tap_(tap), self_(self), codec_(std::make_unique<Codec>())
{
    tap_.active = true;
    if (threaded) worker_ = std::thread([this] { run(); });
}

VoiceChannel::~VoiceChannel()
{
    stop_ = true;
    if (worker_.joinable()) worker_.join();
    tap_.capturing = false;
    tap_.active = false;
}

void VoiceChannel::setSelf(AuthorId self) { self_ = self; }

void VoiceChannel::setTransmit(bool on)
{
    if (transmit_.exchange(on) == on) return;
    tap_.capturing = on;
}

void VoiceChannel::receive(AuthorId speaker, std::string_view rtp)
{
    auto packet = decodeRtp(rtp);
    if (! packet || packet->payloadType != voicePayloadType || speaker == self_.load()) return;
    const std::lock_guard<std::mutex> l(lock_);
    auto& s = speakers_[speaker];
    if (! s) s = std::make_unique<Speaker>();
    ++stats_.received;
    if (s->muted) return;
    s->jitter.push(packet->seq, std::move(packet->payload));
}

std::vector<std::string> VoiceChannel::takeOutgoing()
{
    const std::lock_guard<std::mutex> l(lock_);
    return std::exchange(outgoing_, {});
}

void VoiceChannel::forget(AuthorId speaker)
{
    const std::lock_guard<std::mutex> l(lock_);
    speakers_.erase(speaker);
}

void VoiceChannel::setMuted(AuthorId speaker, bool muted)
{
    const std::lock_guard<std::mutex> l(lock_);
    auto& s = speakers_[speaker];
    if (! s) s = std::make_unique<Speaker>();
    s->muted = muted;
    if (muted) s->jitter = {};
}

bool VoiceChannel::muted(AuthorId speaker) const
{
    const std::lock_guard<std::mutex> l(lock_);
    const auto it = speakers_.find(speaker);
    return it != speakers_.end() && it->second->muted;
}

bool VoiceChannel::talking(AuthorId speaker) const
{
    const std::lock_guard<std::mutex> l(lock_);
    const auto it = speakers_.find(speaker);
    return it != speakers_.end() && ! it->second->muted && nowMs() - it->second->loudMs < 300;
}

bool VoiceChannel::micTalking() const { return transmit_ && nowMs() - micLoudMs_.load() < 300; }

VoiceChannel::Stats VoiceChannel::stats() const
{
    const std::lock_guard<std::mutex> l(lock_);
    auto s = stats_;
    for (const auto& [id, speaker] : speakers_) s.late += speaker->jitter.late();
    return s;
}

void VoiceChannel::run()
{
    while (! stop_)
    {
        pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

void VoiceChannel::pump()
{
    capture();
    playback();
}

// ------------------------------------------------------------------------------------------------ worker

void VoiceChannel::capture()
{
    if (! transmit_)
    {
        tap_.dropCapture();
        captured_.clear();
        return;
    }
    const double rate = tap_.sampleRate();
    if (rate <= 0.0) return;
    const double ratio = rate / voiceRate;
    if (std::abs(ratio - captureRatio_) > 1.0e-9)
    {
        captureRatio_ = ratio;
        codec_->captureResampler.reset();
        captured_.clear();
    }
    const size_t queued = tap_.captureAvailable();
    if (queued > 0)
    {
        const size_t at = captured_.size();
        captured_.resize(at + queued);
        captured_.resize(at + tap_.readCapture(captured_.data() + at, queued));
    }
    const auto needed = (size_t) std::ceil(voiceFrameSamples * ratio) + 8;
    float frame[voiceFrameSamples];
    while (captured_.size() >= needed)
    {
        const int used = codec_->captureResampler.process(ratio, captured_.data(), frame, voiceFrameSamples);
        captured_.erase(captured_.begin(), captured_.begin() + std::min<std::ptrdiff_t>(used, (std::ptrdiff_t) captured_.size()));
        float peak = 0.0f;
        for (float v : frame) peak = std::max(peak, std::abs(v));
        if (peak > talkLevel) micLoudMs_ = nowMs();
        RtpPacket p;
        p.seq = seq_++;
        p.timestamp = timestamp_;
        timestamp_ += voiceFrameSamples;
        p.ssrc = voiceSsrc(self_.load());
#if SONORA_HAVE_OPUS
        if (! codec_->encoder) return;
        unsigned char bytes[1276];
        const int n = opus_encode_float(codec_->encoder, frame, voiceFrameSamples, bytes, (opus_int32) sizeof bytes);
        if (n <= 0) continue;
        p.payload.assign(reinterpret_cast<const char*>(bytes), (size_t) n);
#else
        return;
#endif
        const std::lock_guard<std::mutex> l(lock_);
        if (outgoing_.size() < 50) // ~1 s; the session takes them every 10 ms
        {
            outgoing_.push_back(encodeRtp(p));
            ++stats_.sent;
        }
        else ++stats_.dropped;
    }
}

bool VoiceChannel::mixFrame(std::vector<float>& out)
{
    float mix[voiceFrameSamples] = {};
    bool any = false;
    const std::lock_guard<std::mutex> l(lock_);
    for (auto& [id, s] : speakers_)
    {
        if (s->muted) continue;
        auto next = s->jitter.pop();
        if (next.kind == JitterBuffer::Next::Kind::Idle) continue;
        float pcm[voiceFrameSamples] = {};
        int n = 0;
#if SONORA_HAVE_OPUS
        if (! s->decoder) continue;
        const auto* data = reinterpret_cast<const unsigned char*>(next.payload.data());
        if (next.kind == JitterBuffer::Next::Kind::Frame)
        {
            n = opus_decode_float(s->decoder, data, (opus_int32) next.payload.size(), pcm, voiceFrameSamples, 0);
            ++stats_.decoded;
        }
        else
        {
            // A lost frame: rebuilt from the next packet's in-band FEC when it is here, else concealed.
            n = next.payload.empty() ? opus_decode_float(s->decoder, nullptr, 0, pcm, voiceFrameSamples, 0)
                                     : opus_decode_float(s->decoder, data, (opus_int32) next.payload.size(), pcm, voiceFrameSamples, 1);
            ++stats_.concealed;
        }
#endif
        if (n <= 0) continue;
        any = true;
        float peak = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            mix[i] += pcm[i];
            peak = std::max(peak, std::abs(pcm[i]));
        }
        if (peak > talkLevel) s->loudMs = nowMs();
    }
    if (! any) return false;
    for (float& v : mix) v = std::clamp(v, -1.0f, 1.0f);
    out.insert(out.end(), mix, mix + voiceFrameSamples);
    return true;
}

void VoiceChannel::playback()
{
    const double rate = tap_.sampleRate();
    if (rate <= 0.0) return;
    const double ratio = voiceRate / rate;
    if (std::abs(ratio - playbackRatio_) > 1.0e-9)
    {
        playbackRatio_ = ratio;
        codec_->playbackResampler.reset();
        mix48_.clear();
    }
    const auto lead = (size_t) (rate * playbackLeadSeconds);
    const auto needed = (size_t) std::ceil(resampleChunk * ratio) + 8;
    float out[resampleChunk];
    while (tap_.playbackQueued() < lead)
    {
        if (mix48_.size() < needed && ! mixFrame(mix48_))
        {
            if (mix48_.size() < needed)
            {
                mix48_.clear(); // everyone stopped: the tail is silence anyway
                codec_->playbackResampler.reset();
                return;
            }
        }
        if (mix48_.size() < needed) continue;
        const int used = codec_->playbackResampler.process(ratio, mix48_.data(), out, resampleChunk);
        mix48_.erase(mix48_.begin(), mix48_.begin() + std::min<std::ptrdiff_t>(used, (std::ptrdiff_t) mix48_.size()));
        tap_.writePlayback(out, resampleChunk);
    }
}

} // namespace sonora::collab

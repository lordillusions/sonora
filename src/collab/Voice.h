#pragma once
// COL-7: collab voice, between the audio thread (engine::VoiceTap) and the session's media channel.
//
// Each person's mic is mono Opus at 48 kHz in 20 ms frames, carried as RTP (payload type 111) on the link's SRTP audio
// track (docs/collab/PEER_PROTOCOL.md "Voice"). The SSRC names the speaker (voiceSsrcBase + author id), so the host
// forwards a guest's packets unchanged to the others and every receiver keeps one decoder and jitter buffer per
// speaker. VoiceChannel runs a worker thread: it resamples the tap's capture ring to 48 kHz and encodes, and it decodes,
// conceals losses (Opus in-band FEC, then PLC), mixes the speakers and resamples into the tap's playback ring. The
// session and the app only move packets on the message thread.

#include "collab/Session.h"
#include "engine/VoiceTap.h"

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace sonora::collab
{

inline constexpr uint8_t voicePayloadType = 111;
inline constexpr uint32_t voiceSsrcBase = 0x534e5600; // "SNV" + author id in the low byte
inline constexpr int voiceRate = 48000;
inline constexpr int voiceFrameSamples = 960;           // 20 ms
inline constexpr int voiceBitrate = 24000;

bool voiceBuiltIn(); // libopus was built in (SONORA_HAVE_OPUS)

struct RtpPacket
{
    bool marker = false;
    uint8_t payloadType = voicePayloadType;
    uint16_t seq = 0;
    uint32_t timestamp = 0;
    uint32_t ssrc = 0;
    std::string payload;
};
std::string encodeRtp(const RtpPacket&);
// nullopt: not RTP version 2, RTCP, or malformed (CSRCs, header extension and padding are skipped).
std::optional<RtpPacket> decodeRtp(std::string_view);
inline uint32_t voiceSsrc(AuthorId author) { return voiceSsrcBase + (author & 0xffu); }
std::optional<AuthorId> voiceSpeaker(uint32_t ssrc);

// One speaker's frames in sequence order, started once a few are in and concealed when one is missing.
class JitterBuffer
{
public:
    static constexpr size_t startDepth = 3;  // 60 ms before playing
    static constexpr size_t maxDepth = 15;   // 300 ms: beyond, skip ahead to keep latency bounded
    static constexpr int idleAfter = 10;     // 200 ms without frames: stop, start again on the next talk spurt

    void push(uint16_t seq, std::string payload);
    struct Next
    {
        enum class Kind { Idle, Frame, Missing } kind = Kind::Idle;
        std::string payload; // Frame: this frame; Missing: the following frame when it is here (its in-band FEC)
    };
    Next pop();
    size_t depth() const { return frames_.size(); }
    bool playing() const { return playing_; }
    uint64_t late() const { return late_; }

private:
    std::map<int64_t, std::string> frames_; // extended sequence -> payload
    int64_t last_ = -1, next_ = 0;
    bool playing_ = false;
    int missing_ = 0;
    uint64_t late_ = 0;
};

class VoiceChannel
{
public:
    // `threaded` false: no worker; tests call pump() themselves.
    VoiceChannel(engine::VoiceTap& tap, AuthorId self, bool threaded = true);
    ~VoiceChannel();
    VoiceChannel(const VoiceChannel&) = delete;
    VoiceChannel& operator=(const VoiceChannel&) = delete;

    void setSelf(AuthorId);
    void setTransmit(bool on);   // the mic goes out (also starts/stops the tap's capture)
    bool transmitting() const { return transmit_.load(); }
    void receive(AuthorId speaker, std::string_view rtp); // a packet from the session (any thread)
    std::vector<std::string> takeOutgoing();              // encoded packets for the session
    void forget(AuthorId speaker);                         // they left
    void setMuted(AuthorId speaker, bool muted);           // local only
    bool muted(AuthorId speaker) const;
    bool talking(AuthorId speaker) const;                  // heard in the last 300 ms
    float micLevel() const { return tap_.micPeak(); }
    bool micTalking() const;                               // own mic above the talk threshold, transmitting
    struct Stats
    {
        uint64_t sent = 0, received = 0, decoded = 0, concealed = 0, late = 0, dropped = 0;
    };
    Stats stats() const;
    void pump(); // one worker step

private:
    struct Codec;
    struct Speaker;
    void run();
    void capture();
    void playback();
    bool mixFrame(std::vector<float>& out);

    engine::VoiceTap& tap_;
    std::atomic<AuthorId> self_;
    std::atomic<bool> transmit_{false}, stop_{false};
    std::unique_ptr<Codec> codec_;
    mutable std::mutex lock_; // speakers_, outgoing_, stats_
    std::map<AuthorId, std::unique_ptr<Speaker>> speakers_;
    std::vector<std::string> outgoing_;
    Stats stats_;
    // Worker only.
    std::vector<float> captured_, mix48_;
    double captureRatio_ = 0.0, playbackRatio_ = 0.0;
    uint16_t seq_ = 0;
    uint32_t timestamp_ = 0;
    std::atomic<int64_t> micLoudMs_{0};
    std::thread worker_;
};

} // namespace sonora::collab

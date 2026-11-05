#pragma once
// COL-8: collab camera video. macOS sends and shows it (platform/Video.h: AVFoundation + VideoToolbox, ADR 0011).
//
// Each person's camera is H.264 (constrained baseline, 480x360, 15 fps, ~350 kbit/s, a keyframe every 2 s) in RTP
// (payload type 96, RFC 6184 single NAL units and FU-A) on the link's second SRTP track. The SSRC names the speaker
// (videoSsrcBase + author), so the host forwards packets unchanged, like voice. Video never goes through TURN: the host
// does not forward it over a relayed link and a guest on one does not send it (owner decision, COL-10 pays for relays).
// VideoChannel encodes on the camera's thread and decodes on its own worker; the session moves packets on the message
// thread and the UI draws the latest frame of each person.

#include "collab/Voice.h"
#include "platform/Video.h"

#include <condition_variable>
#include <deque>

namespace sonora::collab
{

inline constexpr uint8_t videoPayloadType = 96;
inline constexpr uint32_t videoSsrcBase = 0x534e5800; // "SNX" + author id in the low byte
inline constexpr int videoWidth = 480, videoHeight = 360, videoFps = 15, videoBitrate = 350000;
inline constexpr size_t videoMtu = 1200; // RTP payload bytes per packet

inline uint32_t videoSsrc(AuthorId author) { return videoSsrcBase + (author & 0xffu); }
std::optional<AuthorId> videoSpeaker(uint32_t ssrc);

// One access unit (its NAL units, no start codes) as RTP packets; the last one carries the marker. `seq` advances.
std::vector<std::string> packetizeH264(const std::vector<std::string>& nals, uint32_t ssrc, uint16_t& seq, uint32_t timestamp,
                                       size_t mtu = videoMtu);

// Rebuilds access units from one sender's packets. A frame with a gap, or one that never saw its marker, is dropped.
class H264Assembler
{
public:
    std::optional<std::vector<std::string>> push(const RtpPacket&);
    uint64_t dropped() const { return dropped_; }

private:
    bool open_ = false, broken_ = false, seqKnown_ = false;
    uint32_t timestamp_ = 0;
    uint16_t nextSeq_ = 0;
    std::vector<std::string> nals_;
    std::string fragment_;
    uint64_t dropped_ = 0;
};

bool hasKeyframe(const std::vector<std::string>& nals); // an IDR slice

class VideoChannel
{
public:
    // `threaded` false: decoding happens in pump() (tests).
    explicit VideoChannel(AuthorId self, bool threaded = true);
    ~VideoChannel();
    VideoChannel(const VideoChannel&) = delete;
    VideoChannel& operator=(const VideoChannel&) = delete;

    void setSelf(AuthorId);
    // The camera, or a moving test pattern (SONORA_COLLAB_VIDEO_TEST autotests: no camera, no permission). False: no camera.
    bool startCamera(bool testPattern = false);
    void stopCamera();
    bool cameraOn() const { return cameraOn_.load(); }
    void setSending(bool on) { sending_ = on; } // frames go out (else only the local preview updates)
    // A captured frame: shown as this person's tile, encoded and packetized when sending. Any thread.
    void sendFrame(platform::VideoImage);

    std::vector<std::string> takeOutgoing();
    void receive(AuthorId speaker, std::string_view rtp); // message thread
    void forget(AuthorId speaker);
    void pump(); // decode what arrived (worker, or tests)

    struct Tile
    {
        AuthorId author = 0;
        std::shared_ptr<const platform::VideoImage> image;
        uint64_t serial = 0; // grows with every new frame
        int64_t updatedMs = 0;
    };
    std::vector<Tile> tiles() const; // the latest frame of everyone, this person first
    struct Stats
    {
        uint64_t framesSent = 0, packetsSent = 0, packetsReceived = 0, framesDecoded = 0, framesDropped = 0;
    };
    Stats stats() const;

private:
    struct Remote;
    void run();
    void testPattern();
    void setTile(AuthorId, std::shared_ptr<const platform::VideoImage>);

    std::atomic<AuthorId> self_;
    std::atomic<bool> cameraOn_{false}, sending_{false}, stop_{false}, patternOn_{false};
    std::unique_ptr<platform::Camera> camera_;
    std::thread pattern_, worker_;
    mutable std::mutex lock_; // everything below
    std::condition_variable wake_;
    std::map<AuthorId, std::unique_ptr<Remote>> remotes_;
    std::deque<std::pair<AuthorId, std::vector<std::string>>> frames_; // complete access units to decode
    std::map<AuthorId, Tile> tiles_;
    std::vector<std::string> outgoing_;
    Stats stats_;
    uint64_t serial_ = 0;
    // The sending side (camera or pattern thread).
    std::mutex encodeLock_;
    std::unique_ptr<platform::H264Encoder> encoder_;
    int64_t framesEncoded_ = 0;
    uint16_t seq_ = 0;
    // The decoding side (worker only).
    std::map<AuthorId, std::unique_ptr<platform::H264Decoder>> decoders_;
};

} // namespace sonora::collab

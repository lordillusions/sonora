#include "collab/Video.h"

#include <juce_core/juce_core.h>

#include <chrono>
#include <cmath>

namespace sonora::collab
{
namespace
{
constexpr size_t maxQueuedFrames = 6;    // decode backlog; older frames go (and the next keyframe is awaited)
constexpr size_t maxOutgoingPackets = 400;
int64_t nowMs() { return juce::Time::currentTimeMillis(); }
} // namespace

std::optional<AuthorId> videoSpeaker(uint32_t ssrc)
{
    if ((ssrc & ~0xffu) != videoSsrcBase || (ssrc & 0xffu) > 63) return std::nullopt;
    return static_cast<AuthorId>(ssrc & 0xffu);
}

std::vector<std::string> packetizeH264(const std::vector<std::string>& nals, uint32_t ssrc, uint16_t& seq, uint32_t timestamp, size_t mtu)
{
    std::vector<std::string> out;
    auto emit = [&](std::string payload, bool marker) {
        RtpPacket p;
        p.payloadType = videoPayloadType;
        p.marker = marker;
        p.seq = seq++;
        p.timestamp = timestamp;
        p.ssrc = ssrc;
        p.payload = std::move(payload);
        out.push_back(encodeRtp(p));
    };
    for (size_t i = 0; i < nals.size(); ++i)
    {
        const auto& nal = nals[i];
        if (nal.empty()) continue;
        const bool last = i + 1 == nals.size();
        if (nal.size() <= mtu)
        {
            emit(nal, last);
            continue;
        }
        // FU-A: the NAL header's F/NRI bits in the indicator, its type in the FU header, S/E flags on the ends.
        const auto header = static_cast<uint8_t>(nal[0]);
        const size_t chunk = mtu - 2;
        for (size_t at = 1; at < nal.size(); at += chunk)
        {
            const bool start = at == 1, end = at + chunk >= nal.size();
            std::string payload;
            payload.push_back(static_cast<char>((header & 0xe0) | 28));
            payload.push_back(static_cast<char>((start ? 0x80 : 0) | (end ? 0x40 : 0) | (header & 0x1f)));
            payload.append(nal, at, std::min(chunk, nal.size() - at));
            emit(std::move(payload), last && end);
        }
    }
    return out;
}

std::optional<std::vector<std::string>> H264Assembler::push(const RtpPacket& p)
{
    if (! open_ || p.timestamp != timestamp_)
    {
        if (open_) ++dropped_; // the previous frame never saw its marker
        open_ = true;
        broken_ = false;
        timestamp_ = p.timestamp;
        nals_.clear();
        fragment_.clear();
    }
    if (seqKnown_ && p.seq != nextSeq_) broken_ = true; // a packet is missing (here or at the end of the last frame)
    seqKnown_ = true;
    nextSeq_ = static_cast<uint16_t>(p.seq + 1);
    const auto& b = p.payload;
    if (b.empty()) broken_ = true;
    else
    {
        const int type = static_cast<uint8_t>(b[0]) & 0x1f;
        if (type >= 1 && type <= 23) nals_.push_back(b);
        else if (type == 28 && b.size() > 2)
        {
            const auto fu = static_cast<uint8_t>(b[1]);
            if (fu & 0x80)
            {
                fragment_.assign(1, static_cast<char>((static_cast<uint8_t>(b[0]) & 0xe0) | (fu & 0x1f)));
                fragment_.append(b, 2, std::string::npos);
            }
            else if (fragment_.empty()) broken_ = true;
            else fragment_.append(b, 2, std::string::npos);
            if ((fu & 0x40) && ! fragment_.empty())
            {
                nals_.push_back(std::move(fragment_));
                fragment_.clear();
            }
        }
        else broken_ = true; // STAP-A and the rest: not sent by Sonora
    }
    if (! p.marker) return std::nullopt;
    open_ = false;
    if (broken_ || ! fragment_.empty() || nals_.empty())
    {
        ++dropped_;
        return std::nullopt;
    }
    return std::move(nals_);
}

bool hasKeyframe(const std::vector<std::string>& nals)
{
    return std::any_of(nals.begin(), nals.end(), [](const std::string& n) { return ! n.empty() && (static_cast<uint8_t>(n[0]) & 0x1f) == 5; });
}

// ------------------------------------------------------------------------------------------------ channel

struct VideoChannel::Remote
{
    H264Assembler assembler;
    bool needKeyframe = true;
};

VideoChannel::VideoChannel(AuthorId self, bool threaded) : self_(self)
{
    if (threaded) worker_ = std::thread([this] { run(); });
}

VideoChannel::~VideoChannel()
{
    stopCamera();
    {
        const std::lock_guard<std::mutex> l(lock_);
        stop_ = true;
    }
    wake_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void VideoChannel::setSelf(AuthorId self) { self_ = self; }

bool VideoChannel::startCamera(bool pattern)
{
    if (cameraOn_) return true;
    if (pattern)
    {
        patternOn_ = true;
        pattern_ = std::thread([this] { testPattern(); });
    }
    else
    {
        camera_ = platform::Camera::open(videoWidth, videoHeight, videoFps, [this](platform::VideoImage image) { sendFrame(std::move(image)); });
        if (! camera_) return false;
    }
    cameraOn_ = true;
    return true;
}

void VideoChannel::stopCamera()
{
    camera_.reset();
    patternOn_ = false;
    if (pattern_.joinable()) pattern_.join();
    cameraOn_ = false;
    const std::lock_guard<std::mutex> l(lock_);
    tiles_.erase(self_.load());
}

void VideoChannel::testPattern()
{
    int frame = 0;
    while (patternOn_)
    {
        platform::VideoImage image;
        image.width = videoWidth;
        image.height = videoHeight;
        image.bgra.resize((size_t) videoWidth * videoHeight);
        for (int y = 0; y < videoHeight; ++y)
            for (int x = 0; x < videoWidth; ++x)
            {
                const int bar = ((x + frame * 4) / 60) % 6;
                static constexpr uint32_t colours[] = {0xffeb5757u, 0xfff2994au, 0xff27ae60u, 0xff2f80edu, 0xff9b51e0u, 0xffe0e0e0u};
                image.bgra[(size_t) y * videoWidth + (size_t) x] = colours[bar] - (uint32_t) ((y * 40 / videoHeight) * 0x010101);
            }
        sendFrame(std::move(image));
        ++frame;
        std::this_thread::sleep_for(std::chrono::milliseconds(1000 / videoFps));
    }
}

void VideoChannel::setTile(AuthorId author, std::shared_ptr<const platform::VideoImage> image)
{
    const std::lock_guard<std::mutex> l(lock_);
    auto& tile = tiles_[author];
    tile.author = author;
    tile.image = std::move(image);
    tile.serial = ++serial_;
    tile.updatedMs = nowMs();
}

void VideoChannel::sendFrame(platform::VideoImage image)
{
    if (image.width <= 0 || image.height <= 0) return;
    auto shared = std::make_shared<platform::VideoImage>(std::move(image));
    setTile(self_.load(), shared); // the local preview
    if (! sending_) return;
    std::vector<std::string> packets;
    {
        const std::lock_guard<std::mutex> l(encodeLock_);
        if (! encoder_) encoder_ = platform::H264Encoder::create(shared->width, shared->height, videoFps, videoBitrate);
        if (! encoder_) return;
        std::vector<std::string> nals;
        const bool keyframe = framesEncoded_ % (videoFps * 2) == 0;
        if (! encoder_->encode(*shared, keyframe, nals))
        {
            encoder_.reset(); // a size change, or the encoder failed: start over with a keyframe
            framesEncoded_ = 0;
            return;
        }
        packets = packetizeH264(nals, videoSsrc(self_.load()), seq_, (uint32_t) (framesEncoded_ * 90000 / videoFps));
        ++framesEncoded_;
    }
    const std::lock_guard<std::mutex> l(lock_);
    if (outgoing_.size() + packets.size() > maxOutgoingPackets) return; // the session is not taking them: drop the frame
    stats_.packetsSent += packets.size();
    ++stats_.framesSent;
    for (auto& p : packets) outgoing_.push_back(std::move(p));
}

std::vector<std::string> VideoChannel::takeOutgoing()
{
    const std::lock_guard<std::mutex> l(lock_);
    return std::exchange(outgoing_, {});
}

void VideoChannel::receive(AuthorId speaker, std::string_view rtp)
{
    auto packet = decodeRtp(rtp);
    if (! packet || packet->payloadType != videoPayloadType || speaker == self_.load()) return;
    {
        const std::lock_guard<std::mutex> l(lock_);
        ++stats_.packetsReceived;
        auto& remote = remotes_[speaker];
        if (! remote) remote = std::make_unique<Remote>();
        const auto droppedBefore = remote->assembler.dropped();
        auto frame = remote->assembler.push(*packet);
        if (remote->assembler.dropped() != droppedBefore)
        {
            remote->needKeyframe = true; // what follows refers to the lost frame
            ++stats_.framesDropped;
        }
        if (! frame) return;
        if (remote->needKeyframe && ! hasKeyframe(*frame))
        {
            ++stats_.framesDropped;
            return;
        }
        remote->needKeyframe = false;
        frames_.push_back({speaker, std::move(*frame)});
        while (frames_.size() > maxQueuedFrames)
        {
            if (auto it = remotes_.find(frames_.front().first); it != remotes_.end()) it->second->needKeyframe = true;
            frames_.pop_front();
            ++stats_.framesDropped;
        }
    }
    wake_.notify_one();
}

void VideoChannel::forget(AuthorId speaker)
{
    const std::lock_guard<std::mutex> l(lock_);
    remotes_.erase(speaker);
    tiles_.erase(speaker);
}

void VideoChannel::pump()
{
    for (;;)
    {
        std::pair<AuthorId, std::vector<std::string>> next;
        {
            const std::lock_guard<std::mutex> l(lock_);
            if (frames_.empty()) break;
            next = std::move(frames_.front());
            frames_.pop_front();
            if (! remotes_.count(next.first)) continue;
        }
        auto& decoder = decoders_[next.first];
        if (! decoder) decoder = platform::H264Decoder::create();
        if (! decoder) continue;
        auto image = std::make_shared<platform::VideoImage>();
        if (decoder->decode(next.second, *image))
        {
            setTile(next.first, std::move(image));
            const std::lock_guard<std::mutex> l(lock_);
            ++stats_.framesDecoded;
        }
        else
        {
            const std::lock_guard<std::mutex> l(lock_);
            if (auto it = remotes_.find(next.first); it != remotes_.end()) it->second->needKeyframe = true;
            ++stats_.framesDropped;
        }
    }
}

void VideoChannel::run()
{
    while (! stop_)
    {
        {
            std::unique_lock<std::mutex> l(lock_);
            wake_.wait_for(l, std::chrono::milliseconds(50), [this] { return stop_.load() || ! frames_.empty(); });
        }
        pump();
    }
}

std::vector<VideoChannel::Tile> VideoChannel::tiles() const
{
    const std::lock_guard<std::mutex> l(lock_);
    std::vector<Tile> out;
    if (auto it = tiles_.find(self_.load()); it != tiles_.end()) out.push_back(it->second);
    for (const auto& [author, tile] : tiles_)
        if (author != self_.load()) out.push_back(tile);
    return out;
}

VideoChannel::Stats VideoChannel::stats() const
{
    const std::lock_guard<std::mutex> l(lock_);
    return stats_;
}

} // namespace sonora::collab

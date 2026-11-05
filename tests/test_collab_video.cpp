// COL-8: collab video: H.264 RTP packetization, VideoToolbox round trips, the video channel, and the host's forwarding
// (never over TURN).
#include "TestFramework.h"
#include "collab/BarePort.h"
#include "collab/Video.h"
#include "io/als/AlsReader.h"

#include <juce_core/juce_core.h>

#include <cmath>

using namespace sonora;

namespace
{
std::string nal(uint8_t header, size_t size, char fill)
{
    std::string n(size, fill);
    n[0] = static_cast<char>(header);
    return n;
}

platform::VideoImage gradient(int frame)
{
    platform::VideoImage image;
    image.width = collab::videoWidth;
    image.height = collab::videoHeight;
    image.bgra.resize((size_t) image.width * (size_t) image.height);
    for (int y = 0; y < image.height; ++y)
        for (int x = 0; x < image.width; ++x)
        {
            const auto r = (uint32_t) ((x + frame * 3) % 256), g = (uint32_t) (y * 255 / image.height), b = 128u;
            image.bgra[(size_t) y * (size_t) image.width + (size_t) x] = 0xff000000u | r << 16 | g << 8 | b;
        }
    return image;
}

// Mean of one channel (shift 16 red, 8 green, 0 blue) over the central quarter.
double centreMean(const platform::VideoImage& image, int shift)
{
    double sum = 0.0;
    int n = 0;
    for (int y = image.height / 4; y < image.height * 3 / 4; ++y)
        for (int x = image.width / 4; x < image.width * 3 / 4; ++x, ++n)
            sum += (double) ((image.bgra[(size_t) y * (size_t) image.width + (size_t) x] >> shift) & 0xffu);
    return n > 0 ? sum / n : 0.0;
}
} // namespace

TEST_CASE("collab video: H.264 access units become RTP packets (single NAL, FU-A) and come back; a gap drops the frame")
{
    const std::vector<std::string> unit = {nal(0x67, 20, 's'), nal(0x68, 5, 'p'), nal(0x65, 5000, 'i')};
    uint16_t seq = 65530; // across the wrap
    const auto packets = collab::packetizeH264(unit, collab::videoSsrc(2), seq, 9000);
    REQUIRE(packets.size() == 2 + 5); // SPS, PPS, then 5 fragments of the IDR
    for (size_t i = 0; i < packets.size(); ++i)
    {
        const auto p = collab::decodeRtp(packets[i]);
        REQUIRE(p.has_value());
        CHECK(p->payload.size() <= collab::videoMtu && p->payloadType == collab::videoPayloadType && p->timestamp == 9000);
        CHECK(p->marker == (i + 1 == packets.size()));
        CHECK(collab::videoSpeaker(p->ssrc) == std::optional<collab::AuthorId>(2));
    }
    CHECK(collab::hasKeyframe(unit) && ! collab::hasKeyframe({nal(0x41, 10, 'x')}));

    collab::H264Assembler assembler;
    std::optional<std::vector<std::string>> frame;
    for (const auto& p : packets) frame = assembler.push(*collab::decodeRtp(p));
    REQUIRE(frame.has_value());
    CHECK(*frame == unit);

    // The next frame loses a fragment: dropped; the one after arrives whole.
    const std::vector<std::string> second = {nal(0x41, 3000, 'q')};
    auto lossy = collab::packetizeH264(second, collab::videoSsrc(2), seq, 15000);
    REQUIRE(lossy.size() == 3);
    lossy.erase(lossy.begin() + 1);
    frame.reset();
    for (const auto& p : lossy) frame = assembler.push(*collab::decodeRtp(p));
    CHECK(! frame.has_value() && assembler.dropped() == 1);
    for (const auto& p : collab::packetizeH264(second, collab::videoSsrc(2), seq, 21000)) frame = assembler.push(*collab::decodeRtp(p));
    CHECK(frame.has_value() && *frame == second);
}

TEST_CASE("collab video: VideoToolbox encodes and decodes frames, and a channel shows the sender's camera")
{
    if (! platform::videoAvailable()) return;
    auto encoder = platform::H264Encoder::create(collab::videoWidth, collab::videoHeight, collab::videoFps, collab::videoBitrate);
    auto decoder = platform::H264Decoder::create();
    REQUIRE(encoder != nullptr && decoder != nullptr);
    int decoded = 0;
    platform::VideoImage last;
    for (int f = 0; f < 20; ++f)
    {
        std::vector<std::string> nals;
        REQUIRE(encoder->encode(gradient(f), f == 0, nals));
        if (f == 0) CHECK(collab::hasKeyframe(nals) && (static_cast<uint8_t>(nals[0][0]) & 0x1f) == 7); // SPS first
        platform::VideoImage out;
        if (decoder->decode(nals, out)) { ++decoded; last = std::move(out); }
    }
    CHECK(decoded == 20);
    CHECK(last.width == collab::videoWidth && last.height == collab::videoHeight);
    const auto original = gradient(19);
    CHECK(std::abs(centreMean(last, 8) - centreMean(original, 8)) < 12.0);
    CHECK(std::abs(centreMean(last, 0) - centreMean(original, 0)) < 12.0);

    // A decoder that starts in the middle waits for a keyframe.
    auto late = platform::H264Decoder::create();
    std::vector<std::string> pFrame;
    REQUIRE(encoder->encode(gradient(21), false, pFrame));
    platform::VideoImage ignored;
    CHECK(! late->decode(pFrame, ignored));

    // Two channels: A's frames reach B as packets; a lost packet costs frames until the next keyframe.
    collab::VideoChannel a(1, false), b(2, false);
    a.setSending(true);
    int lost = 0;
    for (int f = 0; f < 45; ++f)
    {
        a.sendFrame(gradient(f));
        auto packets = a.takeOutgoing();
        if (f == 5 && ! packets.empty()) { packets.erase(packets.begin()); ++lost; } // P-frames are often one packet: all of it
        for (const auto& p : packets) b.receive(1, p);
        b.pump();
    }
    const auto stats = b.stats();
    CHECK(lost == 1 && a.stats().framesSent == 45);
    CHECK(stats.framesDecoded == 5 + 15); // 0-4, then 5-29 are lost or wait for the keyframe at 30, then 30-44
    CHECK(stats.framesDropped >= 1);
    const auto tilesB = b.tiles();
    REQUIRE(tilesB.size() == 1);
    CHECK(tilesB[0].author == 1 && tilesB[0].image && tilesB[0].image->width == collab::videoWidth);
    const auto tilesA = a.tiles();
    REQUIRE(! tilesA.empty());
    CHECK(tilesA[0].author == 1); // the local preview comes first
}

TEST_CASE("collab video: the host forwards video only to peers with it and never over TURN")
{
    auto loaded = io::loadAls(juce::File(SONORA_TEST_FIXTURES).getChildFile("two_tracks.xml"));
    REQUIRE(loaded);
    auto hub = collab::MemoryHub::create();
    collab::BarePort hostPort, anaPort, benPort, oldPort;
    hostPort.document = std::make_unique<model::Song>(*loaded.song);
    collab::CollabSession::Options o;
    o.name = "Hana";
    o.maxParticipants = 5;
    auto host = collab::CollabSession::host(hostPort, hub->hostSide(), o);
    hostPort.session = host.get();
    auto join = [&](collab::BarePort& port, const std::string& name, uint32_t features) {
        collab::CollabSession::Options g;
        g.name = name;
        g.features = features;
        auto s = collab::CollabSession::guest(port, hub->guestSide(), g);
        port.session = s.get();
        return s;
    };
    auto ana = join(anaPort, "Ana", collab::supportedFeatures);
    auto ben = join(benPort, "Ben", collab::supportedFeatures);
    auto old = join(oldPort, "Old", collab::FeatureChat); // chat, no voice or video
    auto pump = [&] {
        for (int i = 0; i < 40; ++i)
        {
            host->poll();
            ana->poll();
            ben->poll();
            old->poll();
        }
    };
    pump();
    if (! (collab::supportedFeatures & collab::FeatureVideo))
    {
        CHECK(! ana->videoAvailable());
        return;
    }
    REQUIRE(ana->videoAvailable() && host->videoAvailable());
    CHECK(! old->videoAvailable());
    uint16_t seq = 0;
    for (const auto& p : collab::packetizeH264({nal(0x65, 3000, 'k')}, collab::videoSsrc(ana->self()), seq, 0)) ana->sendVideo(p);
    pump();
    CHECK(host->stats().videoIn == 3 && host->stats().videoForwarded == 3 && ben->stats().videoIn == 3);
    CHECK(old->stats().videoIn == 0 && old->state() == collab::CollabSession::State::Active);
    CHECK(benPort.voices.empty()); // video is not voice

    // Through TURN: guests can't send, the host forwards nothing.
    hub->setRelayed(true);
    CHECK(! ana->videoAvailable() && ana->videoUnavailableReason().find("direct connection") != std::string::npos);
    const auto forwarded = host->stats().videoForwarded;
    for (const auto& p : collab::packetizeH264({nal(0x41, 100, 'p')}, collab::videoSsrc(collab::hostAuthor), seq, 3000)) host->sendVideo(p);
    pump();
    CHECK(ben->stats().videoIn == 3 && host->stats().videoForwarded == forwarded);
    hub->setRelayed(false);

    // The host turns the camera off for the session.
    host->setPolicy({true, true, false});
    pump();
    CHECK(! ana->videoAvailable() && ana->videoUnavailableReason().find("camera off") != std::string::npos);
}

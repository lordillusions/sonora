// COL-7: collab voice: RTP framing, the jitter buffer, Opus through two voice taps, and the host forwarding speakers.
#include "TestFramework.h"
#include "collab/BarePort.h"
#include "collab/Voice.h"
#include "io/als/AlsReader.h"

#include <juce_core/juce_core.h>

#include <cmath>

using namespace sonora;

namespace
{
constexpr double pi = 3.14159265358979323846;

collab::RtpPacket packet(uint16_t seq, uint32_t ssrc, std::string payload = "opus")
{
    collab::RtpPacket p;
    p.seq = seq;
    p.timestamp = seq * 960u;
    p.ssrc = ssrc;
    p.payload = std::move(payload);
    return p;
}
} // namespace

TEST_CASE("collab voice: RTP packets round-trip; RTCP, other versions and broken headers are refused")
{
    auto p = packet(65535, collab::voiceSsrc(3), std::string("\x01\x02\x03", 3));
    p.marker = true;
    const auto bytes = collab::encodeRtp(p);
    REQUIRE(bytes.size() == 15);
    const auto back = collab::decodeRtp(bytes);
    REQUIRE(back.has_value());
    CHECK(back->seq == 65535 && back->timestamp == p.timestamp && back->ssrc == p.ssrc && back->marker && back->payload == p.payload);
    CHECK(collab::voiceSpeaker(back->ssrc) == std::optional<collab::AuthorId>(3));
    CHECK(! collab::voiceSpeaker(0x12345678).has_value());

    CHECK(! collab::decodeRtp(bytes.substr(0, 11)).has_value());
    auto v1 = bytes;
    v1[0] = 0x40;
    CHECK(! collab::decodeRtp(v1).has_value());
    auto rtcp = bytes;
    rtcp[1] = static_cast<char>(200); // sender report
    CHECK(! collab::decodeRtp(rtcp).has_value());
    // CSRC + header extension + padding are skipped.
    std::string full = bytes.substr(0, 12);
    full[0] = static_cast<char>(0x80 | 0x20 | 0x10 | 1);
    full += std::string(4, 'c');                       // one CSRC
    full += std::string("\xBE\xDE\x00\x01", 4) + "xxxx"; // one extension word
    full += "pay";
    full += std::string("\x00\x00\x03", 3);            // three bytes of padding
    const auto skipped = collab::decodeRtp(full);
    REQUIRE(skipped.has_value());
    CHECK(skipped->payload == "pay");
    full.back() = 60; // padding longer than the packet
    CHECK(! collab::decodeRtp(full).has_value());
}

TEST_CASE("collab voice: the jitter buffer reorders, conceals with the next frame, drops late frames and wraps")
{
    using Kind = collab::JitterBuffer::Next::Kind;
    collab::JitterBuffer j;
    j.push(65534, "a");
    CHECK(j.pop().kind == Kind::Idle); // waits for three frames
    j.push(0, "c");                     // across the wrap, out of order
    j.push(65535, "b");
    auto n = j.pop();
    CHECK(n.kind == Kind::Frame && n.payload == "a");
    CHECK(j.pop().payload == "b");
    CHECK(j.pop().payload == "c");
    j.push(2, "e");                     // 1 is lost
    j.push(3, "f");
    n = j.pop();
    CHECK(n.kind == Kind::Missing && n.payload == "e"); // the next frame carries 1's FEC
    j.push(1, "d");                                     // too late now
    CHECK(j.late() == 1);
    CHECK(j.pop().payload == "e");
    CHECK(j.pop().payload == "f");
    for (int i = 0; i < collab::JitterBuffer::idleAfter; ++i) CHECK(j.pop().kind == Kind::Missing); // concealed
    CHECK(j.pop().kind == Kind::Idle);
    CHECK(! j.playing());
    // A burst far ahead is cut back to a short queue.
    for (uint16_t s = 100; s < 140; ++s) j.push(s, "x");
    CHECK(j.depth() <= collab::JitterBuffer::startDepth + 1);
}

TEST_CASE("collab voice: a mic at 44.1 kHz reaches a 48 kHz output through Opus, after Main and ducking the music")
{
    if (! collab::voiceBuiltIn()) return;
    engine::VoiceTap tapA, tapB;
    collab::VoiceChannel a(tapA, 1, false), b(tapB, 2, false);
    a.setTransmit(true);
    CHECK(tapA.capturing.load() && tapB.active.load());
    std::vector<float> mic(441), outA0(441), outA1(441), outB0(480), outB1(480);
    double phase = 0.0;
    std::vector<float> heard;
    double musicSum = 0.0;
    int musicCount = 0;
    for (int step = 0; step < 200; ++step) // 2 s in 10 ms steps
    {
        const bool talking = step < 100;
        for (auto& v : mic)
        {
            v = talking ? 0.5f * (float) std::sin(phase) : 0.0f;
            phase += 2.0 * pi * 440.0 / 44100.0;
        }
        const float* in[1] = {mic.data()};
        float* outsA[2] = {outA0.data(), outA1.data()};
        tapA.process(in, 1, outsA, 2, 441, 44100.0);
        a.pump();
        for (const auto& rtp : a.takeOutgoing()) b.receive(1, rtp);
        b.pump();
        std::fill(outB0.begin(), outB0.end(), 0.0f);  // left: silence, so it is the voice alone
        std::fill(outB1.begin(), outB1.end(), 0.5f);  // right: "music" at 0.5
        float* outsB[2] = {outB0.data(), outB1.data()};
        tapB.process(nullptr, 0, outsB, 2, 480, 48000.0);
        if (step >= 40 && step < 90)
        {
            heard.insert(heard.end(), outB0.begin(), outB0.end());
            for (size_t i = 0; i < 480; ++i) musicSum += outB1[i] - outB0[i];
            musicCount += 480;
        }
        if (step == 199)
        {
            float tail = 0.0f;
            for (size_t i = 0; i < 480; ++i) tail = std::max(tail, std::abs(outB1[i] - 0.5f));
            CHECK(tail < 0.02f); // a second after the voice stopped (latency + 300 ms hold), the music is back at full level
        }
    }
    const auto stats = b.stats();
    CHECK(a.stats().sent >= 95 && stats.decoded >= 90 && stats.late == 0);
    // The tone came through: loud enough, and about 440 zero crossings... per second (880 sign changes).
    double sum = 0.0;
    int crossings = 0;
    for (size_t i = 0; i < heard.size(); ++i)
    {
        sum += (double) heard[i] * heard[i];
        if (i > 0 && (heard[i - 1] < 0.0f) != (heard[i] < 0.0f)) ++crossings;
    }
    const double rms = std::sqrt(sum / (double) heard.size());
    const double seconds = (double) heard.size() / 48000.0;
    CHECK(rms > 0.2 && rms < 0.5);
    CHECK(std::abs(crossings / seconds - 880.0) < 40.0);
    // While the voice plays, the music is ducked to half.
    CHECK(std::abs(musicSum / musicCount - 0.25) < 0.03);
    CHECK(tapB.underruns() == 0);
}

TEST_CASE("collab voice: the host forwards each speaker, never to an older Sonora; spoofed speakers are dropped")
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
    auto old = join(oldPort, "Old", 0);
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
    REQUIRE(ana->state() == collab::CollabSession::State::Active);
    REQUIRE(old->state() == collab::CollabSession::State::Active);
    if (! collab::voiceBuiltIn())
    {
        CHECK(! ana->voiceAvailable());
        return;
    }
    CHECK(ana->voiceAvailable() && host->voiceAvailable());
    CHECK(! old->voiceAvailable());

    ana->setMediaState({true, false});
    pump();
    CHECK(ben->mediaState(ana->self()).micOn && host->mediaState(ana->self()).micOn);
    ana->sendVoice(collab::encodeRtp(packet(7, collab::voiceSsrc(ana->self()))));
    host->sendVoice(collab::encodeRtp(packet(9, collab::voiceSsrc(collab::hostAuthor))));
    ana->sendVoice(collab::encodeRtp(packet(8, collab::voiceSsrc(ben->self())))); // pretends to be Ben
    pump();
    REQUIRE(hostPort.voices.size() == 1);
    CHECK(hostPort.voices[0].first == ana->self());
    REQUIRE(benPort.voices.size() == 2);
    CHECK(benPort.voices[0].first == ana->self() || benPort.voices[1].first == ana->self());
    CHECK(benPort.voices[0].first == collab::hostAuthor || benPort.voices[1].first == collab::hostAuthor);
    CHECK(anaPort.voices.size() == 1 && anaPort.voices[0].first == collab::hostAuthor); // never its own voice back
    CHECK(oldPort.voices.empty() && old->state() == collab::CollabSession::State::Active && ! oldPort.ended_);
    CHECK(host->stats().voiceDropped == 1);

    // Someone joining later learns who has the mic on.
    collab::BarePort cleoPort;
    auto cleo = join(cleoPort, "Cleo", collab::supportedFeatures);
    for (int i = 0; i < 40; ++i) { cleo->poll(); pump(); }
    CHECK(cleo->mediaState(ana->self()).micOn && ! cleo->mediaState(ben->self()).micOn);

    // The host turns voice off: nobody can send, and the host passes nothing on.
    host->setPolicy({true, false, true});
    pump();
    CHECK(! ana->voiceAvailable() && ana->voiceUnavailableReason().find("turned voice off") != std::string::npos);
    benPort.voices.clear();
    host->sendVoice(collab::encodeRtp(packet(10, collab::voiceSsrc(collab::hostAuthor))));
    pump();
    CHECK(benPort.voices.empty());
}

#include "TestFramework.h"

#include "core/DeviceCatalog.h"
#include "core/LiveColors.h"
#include "core/TimeUtils.h"

using namespace sonora::model;

TEST_CASE("time signature decode/encode")
{
    auto [n, d] = decodeTimeSignature(201);
    CHECK(n == 4);
    CHECK(d == 4);
    auto [n2, d2] = decodeTimeSignature(encodeTimeSignature(7, 8));
    CHECK(n2 == 7);
    CHECK(d2 == 8);
    CHECK(encodeTimeSignature(3, 4) == 200);
}

TEST_CASE("gain <-> dB uses Live's -70 dB floor")
{
    CHECK_NEAR(gainToDecibels(1.0), 0.0, 1e-9);
    CHECK_NEAR(gainToDecibels(0.0003162277571), -70.0, 1e-6);
    CHECK_NEAR(decibelsToGain(-6.0), 0.501187, 1e-5);
    CHECK(decibelsToGain(-70.0) == 0.0);
}

TEST_CASE("clip content position follows loop")
{
    // Clip placed at beat 4, loop 0..8, looping.
    CHECK_NEAR(clipContentPosition(4.0, 4.0, 0.0, 8.0, 0.0, true), 0.0, 1e-9);
    CHECK_NEAR(clipContentPosition(11.5, 4.0, 0.0, 8.0, 0.0, true), 7.5, 1e-9);
    CHECK_NEAR(clipContentPosition(13.0, 4.0, 0.0, 8.0, 0.0, true), 1.0, 1e-9);
    // Not looping: keeps advancing.
    CHECK_NEAR(clipContentPosition(13.0, 4.0, 0.0, 8.0, 0.0, false), 9.0, 1e-9);
    // Start offset inside the loop.
    CHECK_NEAR(clipContentPosition(4.0, 4.0, 2.0, 6.0, 1.0, true), 3.0, 1e-9);
}

TEST_CASE("device catalog")
{
    const auto* eq = findCatalogEntry("Eq8");
    REQUIRE(eq != nullptr);
    CHECK(eq->displayName == "EQ Eight");
    CHECK(eq->implemented);
    CHECK(findCatalogEntry("NotADevice") == nullptr);
    CHECK(isRackTag("DrumGroupDevice"));
    CHECK(isMaxForLiveTag("MxDeviceAudioEffect"));
}

TEST_CASE("palette")
{
    CHECK(livePaletteColour(13) == 0xFFFFFF);
    CHECK(livePaletteColour(-1) == 0x7B7B7B);
    CHECK(livePaletteColour(999) == 0x7B7B7B);
}

#include "core/InputRouting.h"

TEST_CASE("input routing: external mono stereo MIDI ports channels resampling and unsupported targets")
{
    using namespace sonora::model;
    Track track;
    track.kind = TrackKind::Audio;
    track.inputRouting = "AudioIn/External/M3";
    CHECK(parseInput(track).kind == InputKind::AudioMono);
    CHECK(parseInput(track).channel == 3);
    track.inputRouting = "AudioIn/External/S1";
    track.inputLowerDisplay = "3/4";
    CHECK(parseInput(track).kind == InputKind::AudioStereo);
    CHECK(parseInput(track).channel == 2);
    track.inputRouting = "AudioIn/Main";
    CHECK(parseInput(track).kind == InputKind::Resampling);
    track.inputRouting = "AudioIn/Track.12/PostFx";
    CHECK(parseInput(track).kind == InputKind::Unsupported); // preserved for the inter-track routing follow-up
    track.inputRouting = "AudioIn/External/M-1";
    CHECK(parseInput(track).kind == InputKind::Unsupported);
    track.kind = TrackKind::Midi;
    track.inputRouting = "MidiIn/External.All/-1";
    CHECK(parseInput(track).device.empty());
    CHECK(parseInput(track).channel == 0);
    setInputSelection(track, {InputKind::Midi, 5, "controller-port"});
    const auto packed = packInput(parseInput(track));
    CHECK(inputKind(packed) == InputKind::Midi);
    CHECK(inputChannel(packed) == 5);
    CHECK(inputMidiPort(packed) == midiPortToken("controller-port"));
    CHECK(inputMidiPort(packed) != midiPortToken("other-port"));
    track.inputRouting = "MidiIn/External.All/16";
    CHECK(parseInput(track).kind == InputKind::Unsupported);
    setInputSelection(track, {InputKind::None, 0, {}});
    CHECK(parseInput(track).kind == InputKind::None);
}

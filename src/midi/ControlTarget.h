#pragma once
// What a MIDI message can control and how its value is interpreted. No JUCE dependency, so the
// router and the persistence code are unit-testable (ROADMAP MIDI-2..4).

#include <string>

namespace sonora::midi
{

struct ControlTarget
{
    enum class Kind
    {
        TrackVolume,
        TrackPan,
        TrackSend,
        TrackActivator, // button: on/off
        TrackSolo,      // button
        TrackArm,       // button
        DeviceParameter,
        DeviceOn, // button: device enabled
        TransportPlay,
        TransportStop,
        TransportToggle,
        Tempo,
        SessionClip,
    };
    Kind kind = Kind::TrackVolume;
    int track = 0;  // ControlBank index (tracks..., returns..., main)
    int device = 0; // index in the track's top-level device chain
    int index = 0;  // send index
    std::string param; // DeviceParameter: ParameterInfo::id (the .als parameter path)
    int clipUid = -1; // SessionClip: runtime identity, never resolve a replacement in the same slot
    int trackId = -1; // saved track ID; index is the saved Session slot when rebinding a sidecar
    int boundTrackId = -1, boundReturnId = -1; // runtime-only identities across structure edits/history
    bool trackBound = false, returnBound = false;

    bool operator==(const ControlTarget& other) const
    {
        if (kind != other.kind) return false;
        if (kind == Kind::SessionClip && clipUid >= 0 && other.clipUid >= 0) return clipUid == other.clipUid;
        return track == other.track && device == other.device && index == other.index && param == other.param
               && clipUid == other.clipUid && trackId == other.trackId;
    }
    bool isButton() const
    {
        return kind != Kind::TrackVolume && kind != Kind::TrackPan && kind != Kind::TrackSend
               && kind != Kind::DeviceParameter && kind != Kind::Tempo;
    }
};

// How an encoder reports its value (Live's "ControllerMapMode").
enum class MapMode
{
    Absolute = 0,
    Relative2sComplement,
    RelativeBinaryOffset,
    RelativeSignedBit,
};

// Maps the numeric ControllerMapMode stored in .als files. Live's list (absolute, absolute
// 14 bit, signed bit, smooth signed bit, binary offset, smooth binary offset, two's complement,
// smooth two's complement) is recalled from the Live API documentation; the "smooth" variants
// only change the response curve. TODO(ALS-13): verify with a set that uses relative encoders.
inline MapMode mapModeFromLive(int v)
{
    switch (v)
    {
        case 2: case 3: return MapMode::RelativeSignedBit;
        case 4: case 5: return MapMode::RelativeBinaryOffset;
        case 6: case 7: return MapMode::Relative2sComplement;
        default: return MapMode::Absolute;
    }
}

// Signed tick count of a relative encoder CC value (0..127).
inline int decodeRelative(MapMode mode, int cc)
{
    switch (mode)
    {
        case MapMode::Relative2sComplement: return cc < 64 ? cc : cc - 128;
        case MapMode::RelativeBinaryOffset: return cc - 64;
        case MapMode::RelativeSignedBit: return (cc & 0x40) != 0 ? -(cc & 0x3f) : (cc & 0x3f);
        case MapMode::Absolute: break;
    }
    return 0;
}

struct Mapping
{
    int channel = 1; // 1..16
    bool isNote = false; // MIDI note press (and release for clips), instead of a CC
    int number = 0;  // CC or note number, 0..127
    MapMode mode = MapMode::Absolute;
    ControlTarget target;
    int keyCode = 0; // > 0: computer key, instead of a MIDI source; only letters/digits for clip launching

    bool sameSource(const Mapping& other) const
    {
        return keyCode > 0 || other.keyCode > 0 ? keyCode > 0 && keyCode == other.keyCode
                                              : channel == other.channel && isNote == other.isNote && number == other.number;
    }

    bool operator==(const Mapping&) const = default;
};

// A control moved: either an absolute 0..1 value or a relative step in "ticks" (±1 == one
// encoder click), or a button press (value 1).
struct ControlChange
{
    float value = 0.0f;
    bool relative = false;
    int ticks = 0;
    bool trigger = false; // note-on: buttons toggle instead of following `value`
    bool released = false; // SessionClip: note-off or computer-key release (legacy buttons ignore it)
};

inline int normaliseLaunchKey(int code)
{
    if (code >= 'a' && code <= 'z') code -= 'a' - 'A';
    return (code >= 'A' && code <= 'Z') || (code >= '0' && code <= '9') ? code : 0;
}

} // namespace sonora::midi

#pragma once
// COL-2: exact binary codec for the Song model (docs/COLLAB_DESIGN.md sections 3-5, ADR 0010).
//
// A collab session replicates the Song as a set of objects (song header, one header, device list and clip order
// per track, one object per clip). Each object has two encodings:
//  - Full: every field, doubles bit-exact, source XML fragments and native content by value. This is what travels
//    between peers and what undo and conflict recovery restore. decode*() reads only this form.
//  - Identity: a cheap local fingerprint for change detection. It leaves out live values (mixer levels, device
//    parameter values, bypass, opaque plugin state, the saved playhead), which the live-parameter channel syncs
//    instead, and it stands immutable shared fragments in by pointer. Identity bytes are only ever compared with
//    identity bytes made by the same process. hash*() feed the same values into a 64-bit hash without building the
//    bytes (COL-2b): equal identity forms give equal hashes, different ones differ except for 64-bit collisions.
//
// Decoding is safe on untrusted input (bounded counts, depth and enums; CodecError on anything malformed). Value
// ranges are not validated here; the collab sequencer does that.
//
// Every field of Model.h must be covered. tests/test_model_codec.cpp round-trips real sets and compares writer
// output, and ModelCodec.cpp pins the struct sizes on macOS arm64 so a new field cannot be forgotten silently.

#include "core/ByteCodec.h"
#include "core/Model.h"

#include <string>
#include <string_view>
#include <vector>

namespace sonora::model::codec
{
inline constexpr uint32_t formatVersion = 1;
inline constexpr int maxDeviceDepth = 12;

enum class Form
{
    Identity,
    Full
};

// Song-level fields that are edited during a session: tempo, signature, scenes, locators, loop/punch, quantisation,
// scale, return-send templates and the id allocators. Not tracks, not the document binding (see encodeDocument).
std::string encodeSongHeader(const Song&, Form);
void decodeSongHeader(std::string_view, Song&);

// Document binding, carried only by a join snapshot: source path, creator, schema, warnings, saved clip
// locations and the MIDI clip template.
std::string encodeDocument(const Song&);
void decodeDocument(std::string_view, Song&);

// Every Track field except devices, arrangement clips and Session clips. Session slot count is kept (the slots come
// back empty); take lanes and frozen clips are part of the header.
std::string encodeTrackHeader(const Track&, Form);
Track decodeTrackHeader(std::string_view);

std::string encodeDevices(const std::vector<Device>&, Form);
std::vector<Device> decodeDevices(std::string_view);

std::string encodeClip(const Clip&, Form);
Clip decodeClip(std::string_view);

// 64-bit hashes of the identity forms above (change detection; same process only).
uint64_t hashSongHeader(const Song&);
uint64_t hashTrackHeader(const Track&);
uint64_t hashDevices(const std::vector<Device>&);
uint64_t hashClip(const Clip&);

// False when a value is NaN or infinite (validation of objects received from peers).
bool finiteSongHeader(const Song&);
bool finiteTrackHeader(const Track&);
bool finiteDevices(const std::vector<Device>&);
bool finiteClip(const Clip&);

// The whole song in the full form (tests, model hashes and keep-a-copy).
std::string encodeSong(const Song&);
Song decodeSong(std::string_view);

} // namespace sonora::model::codec

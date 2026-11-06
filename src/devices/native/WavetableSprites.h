#pragma once
// Wavetables of the imported Wavetable instrument (DEV-28b). Load thread only (file I/O, allocation).
//
// A set names each oscillator's table by its display name (`SpriteName1` = "Harmonics 1") or stores a user sample
// (`UserSprite1` SampleRef). Factory tables are the reference's own assets and are never copied into Sonora: when the
// reference application is installed, they are read at run time from its Builtin/Samples/Vector/Sprites folder
// ("Vector_Sprite_<Category> <Name>.wav", 1024-sample frames: all 194 files are whole multiples of 1024, 60 are not
// of 2048, and "Basic Shapes" = 4 frames sine / triangle / saw / square, as the reference plays it, DEV-28b2).
// Otherwise a Sonora-generated stand-in plays and the load reports it (docs/WAVETABLE.md "Tables").

#include "core/Model.h"
#include "devices/native/WavetableData.h"

#include <memory>
#include <string>
#include <vector>

namespace sonora::devices
{
class WavetableInstrument;

struct WavetableSprite
{
    std::string category, name; // "Retro", "Harmonics 1"; category empty for user samples
    std::string file;           // absolute path, empty for the generated stand-in
};

// Factory tables found on this machine, sorted by category then name (cached after the first scan).
const std::vector<WavetableSprite>& factoryWavetables();
// The folder searched for factory tables, or empty. SONORA_WAVETABLE_SPRITES overrides the search.
std::string factoryWavetableFolder();
// Finds a factory table by display name ("Harmonics 1") or "Category Name"; nullptr when absent.
const WavetableSprite* findFactoryWavetable(const std::string& name);
// Samples per frame of the reference's tables (factory and user samples alike).
constexpr int kWavetableFrameLength = 1024;
// Loads a table (cached per file); nullptr with `error` set on failure. Lengths that are not whole frames are cut to
// whole 1024-sample frames (kWavetableFrameLength; a shorter file becomes one frame, resampled).
std::shared_ptr<const WavetableData> loadWavetableFile(const std::string& path, std::string& error);
// Sonora's own stand-in table: a 16-frame sine -> triangle -> saw -> square morph, generated here.
std::shared_ptr<const WavetableData> standInWavetable();

// What the set stores for oscillator `index` (0 / 1), read from the device's retained XML.
struct WavetableSpriteRef
{
    std::string spriteName;   // SpriteName1/2
    std::string userPath;     // resolved UserSprite SampleRef path, empty when none
    bool raw = false;         // UseRawUserWavetable
};
WavetableSpriteRef readWavetableSpriteRef(const model::Device& device, int index);
// Rewrites SpriteName<index+1> (and clears that oscillator's user sample) in the device's retained XML; marks the
// device so the writer saves the choice.
void setWavetableSpriteName(model::Device& device, int index, const std::string& name);

// Resolves and installs both oscillators' tables on `synth`; appends notes for stand-ins to `warnings`.
void prepareWavetableInstrument(WavetableInstrument& synth, const model::Device& device, std::vector<std::string>& warnings);
} // namespace sonora::devices

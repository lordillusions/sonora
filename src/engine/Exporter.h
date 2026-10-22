#pragma once
// Project export / bounce: renders the arrangement offline through the real engine (AudioEngine::renderOffline) into
// WAV / AIFF / FLAC files: the Main mix, one track, or every track as a stem. Rendering goes to 32-bit float
// scratch files first so Normalize, Mono, bit depth and dither are applied in one finishing pass, and the Main mix is
// measured with the BS.1770 meter on the way (loudness report in the result).
//
// Threads: run() blocks on a worker thread; beginOffline()/endOffline() on the engine belong to the caller (message thread).

#include "core/Model.h"
#include "engine/AudioEngine.h"

#include <juce_audio_formats/juce_audio_formats.h>

#include <atomic>
#include <string>
#include <vector>

namespace sonora::engine
{

enum class ExportTarget
{
    Main,        // the whole mix: tracks, returns and Main effects
    SingleTrack, // one track's output (post-fader, before sends and Main), `trackIndex`
    AllTracks    // every track and return as its own file (stems) into a folder
};
enum class ExportFormat { Wav, Aiff, Flac };

struct ExportOptions
{
    ExportTarget target = ExportTarget::Main;
    int trackIndex = -1;
    bool preMixer = false; // Freeze/Bounce: bake instruments/effects, keep fader/pan/sends live
    int sessionScene = -1; // >= 0: render this Session slot on the requested track
    double startBeat = 0.0, endBeat = 0.0; // render range in beats (end exclusive)
    double tailSeconds = 0.0;              // extra time after the range (reverb / delay tails)
    bool autoTail = false;                 // keep going until the Main output is silent (max 30 s), overrides tailSeconds
    ExportFormat format = ExportFormat::Wav;
    int bitDepth = 24;                     // 16, 24 or 32 (float; WAV only)
    bool dither = true;                    // triangular, only below 32 bit
    bool normalize = false;                // peak to 0 dBFS (per file)
    bool mono = false;                     // sum to one channel
    juce::File destination;                // Main / SingleTrack: the file; AllTracks: the folder
    std::string baseName = "Export";       // stems: file name prefix
};

struct ExportFile
{
    juce::File file;
    std::string name; // track name ("Main" for the mix)
};

struct ExportResult
{
    bool ok = false, cancelled = false;
    std::string error;
    std::vector<ExportFile> files;
    // Loudness of the first rendered stream (the mix for Main), measured on the written signal.
    float integratedLufs = -200.0f, loudnessRange = 0.0f, truePeakDb = -200.0f, samplePeakDb = -200.0f;
    double seconds = 0.0;      // length of each file
    std::vector<model::WarpMarker> warpMarkers; // source seconds -> elapsed arrangement beats, including tempo automation
    double renderSeconds = 0.0; // wall time
};

class Exporter
{
public:
    Exporter(AudioEngine& engine, const model::Song& song) : engine_(engine), song_(song) {}

    // Blocks. `progress` runs 0..1, `cancel` is polled between blocks. The engine must be in offline mode.
    ExportResult run(const ExportOptions& options, std::atomic<float>& progress, const std::atomic<bool>& cancel);

    struct Range { double startBeat = 0.0, endBeat = 0.0; };
    // mode 0: the whole arrangement, 1: the loop brace (the arrangement when the loop is off), 2: custom bar numbers (the end bar is excluded).
    static Range resolveRange(const model::Song& song, int mode, double startBar, double endBar);
    // End of the last arrangement clip, rounded up to a whole bar (a sensible default range end).
    static double arrangementEndBeat(const model::Song& song);
    static std::string extensionFor(ExportFormat format);
    static std::string safeFileName(const std::string& name);

private:
    AudioEngine& engine_;
    const model::Song& song_;
};

} // namespace sonora::engine

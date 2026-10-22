#pragma once
// Stem separation (vocals / drums / bass / other) by an external command-line tool. Sonora does not bundle a
// model: it looks for a separator the user installed and runs it as a child process, then collects the WAV
// files it wrote.
//
// Supported tools (first found wins unless the caller picks one):
//   audio-separator  (pip / uv tool: `uv tool install "audio-separator[cpu]"`) runs UVR's MDX-Net, VR and Demucs models.
//                    When Ultimate Vocal Remover.app is installed its model folder is reused (no download) for the
//                    models it contains.
//   demucs           (`pipx install demucs`) htdemucs: drums, bass, other, vocals.
//
// Everything here is JUCE-core only so it is unit tested with a fake tool script.

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <string>
#include <vector>

namespace sonora::engine
{

struct StemTool
{
    enum class Kind { AudioSeparator, Demucs } kind = Kind::Demucs;
    juce::File executable;
    juce::File modelFolder; // audio-separator: folder holding the models to reuse (UVR's MDX_Net_Models), may be empty
    std::string model;      // model file / name to ask for
    std::string label;      // "audio-separator (UVR MDX-Net Inst HQ 3)" for the UI
};

struct StemFile
{
    std::string name; // "Vocals", "Drums", "Bass", "Other", "Instrumental"...
    juce::File file;
};

struct StemResult
{
    bool ok = false, cancelled = false;
    std::string error;
    std::vector<StemFile> stems;
};

class StemSeparator
{
public:
    // Looks for installed tools: PATH, ~/.local/bin, Homebrew, pipx / uv tool directories. `extraDirectories` are searched first.
    static std::vector<StemTool> detectTools(const std::vector<juce::File>& extraDirectories = {});
    // Folder of UVR's bundled MDX-Net models when the app is installed ("" otherwise).
    static juce::File uvrModelFolder();
    // The shell-free argument list for a run (public for tests).
    static juce::StringArray commandLine(const StemTool& tool, const juce::File& input, const juce::File& outputDir);
    // Runs the tool (blocking, worker thread). `onLine` receives the tool's output lines (progress / errors).
    static StemResult separate(const StemTool& tool, const juce::File& input, const juce::File& outputDir,
                               const std::atomic<bool>& cancel, const std::function<void(const std::string&)>& onLine = {});
    // Display name of a stem from a file name written by either tool ("song_(Vocals)_model.wav" -> "Vocals", "drums.wav" -> "Drums").
    static std::string stemNameFromFile(const juce::File& file);
};

} // namespace sonora::engine

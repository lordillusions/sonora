#pragma once
// Sonora Seraph, Real voices (PLG-27b): a recording of a sung note analysed for pitch-synchronous resynthesis.
//
// Analysis (message thread, once per file, cached by path): the fundamental track (YIN on 1024-sample frames, hop 256),
// the voiced span, the longest stable span (pitch within 50 cents of its median, level within 6 dB) which becomes the
// sustain, the attack before it, and pitch marks one per period through both (each mark placed where the period around it
// best matches the period around the previous one, so the marks stay on the same point of the waveform). A file without
// such a sustain of at least 0.6 s is refused with the reason: a phrase, a chord, a wet or noisy sample has no stable
// pitch. The sung note is the sustain's median pitch; a key in the file name ("_A3", "_F#") only settles an octave doubt.
// A vowel in the file name ("_a", "_ah", "_o", "_oo"...) is kept for the mapping. docs/SERAPH.md "Real voices".

#include <memory>
#include <string>
#include <vector>

namespace sonora::devices::seraph
{
struct RealVoice
{
    std::string path, name;
    double sampleRate = 44100.0;
    std::vector<float> audio;    // mono, at the file's rate
    std::vector<double> marks;   // pitch marks (sample positions in `audio`), attack then sustain
    std::vector<float> periods;  // the local period (samples) at each mark
    // One grain per mark, two periods long (centre at index grainHalf[i] of the grain starting at grainStart[i]): the
    // recording's harmonics there (amplitude and phase, measured exactly) with the spectrum between them interpolated, so
    // the grains can be laid at another period without the notches plain PSOLA leaves between the recording's harmonics.
    std::vector<float> grainData;
    std::vector<int> grainStart, grainHalf;
    int sustainBegin = 0, sustainEnd = 0; // [begin, end) indices into marks
    float midi = 60;             // the note it was sung at
    float level = 0.1f;          // RMS over the sustain
    int vowel = -1;              // 0..4 from the file name (A E I O U), -1 unknown

    double attackStartSeconds() const { return marks.empty() ? 0.0 : marks.front() / sampleRate; }
    double sustainSeconds() const
    {
        return sustainEnd > sustainBegin ? (marks[(size_t) sustainEnd - 1] - marks[(size_t) sustainBegin]) / sampleRate : 0.0;
    }
};

// Analyses mono or stereo audio (channels averaged). Null and `reason` when it cannot be played.
std::shared_ptr<const RealVoice> analyseRealVoice(const float* const* channels, int numChannels, int numSamples, double sampleRate,
                                                  const std::string& name, std::string& reason);
// The vowel a file name names (-1: none) and the key it names (MIDI, -1: none; a pitch class without octave gives -2 - pc).
int vowelFromName(const std::string& name);
int keyFromName(const std::string& name);

// The fundamental track of `x` (tests and the face): one value per hop of 256 samples, 0 where unvoiced.
std::vector<float> pitchTrack(const float* x, int n, double sampleRate, std::vector<float>* aperiodicity = nullptr);
} // namespace sonora::devices::seraph

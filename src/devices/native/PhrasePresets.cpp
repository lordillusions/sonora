// Sonora Phrase factory presets (PLG-25, docs/PHRASE.md "Factory presets"). Code tables, like Sweep's types: each preset
// is a part, the generator style and seed for its slots, a form, and the performance values. The patterns come from the
// generators (core/PhraseEngine), so a preset is the same notes on every computer. Names are Sonora's own.

#include "devices/native/Phrase.h"

#include "core/PatternRandom.h"

namespace sonora::devices
{
namespace
{
using namespace model;
using P = Phrase;
using Values = std::vector<std::pair<int, float>>;

// Every performance value a preset sets (so applying one resets what the last one changed).
Values perform(Values overrides)
{
    Values v{{P::Pattern, 0}, {P::Density, 1}, {P::Swing, 0}, {P::Gate, 1}, {P::Velocity, 1}, {P::Accent, 20}, {P::Octave, 0},
             {P::Shift, 0}, {P::Humanize, 0.15f}, {P::KickGuard, 0}};
    for (const auto& [param, value] : overrides)
    {
        bool found = false;
        for (auto& existing : v)
            if (existing.first == param) { existing.second = value; found = true; }
        if (! found) v.push_back({param, value});
    }
    return v;
}

// The form: letters of slots ("A B A C"); slot A is generated, B..D are answers of A by `answers` in order.
void setForm(PhrasePartState& part, const std::string& form)
{
    part.formLength = 0;
    for (char c : form)
        if (c >= 'A' && c <= 'H' && part.formLength < kPhraseFormMax) part.form[(size_t) part.formLength++] = (uint8_t) (c - 'A');
    if (part.formLength == 0) { part.formLength = 1; part.form[0] = 0; }
}

Phrase::Preset arp(std::string name, std::string description, ArpStyle style, uint32_t seed, std::string form,
                   std::initializer_list<AnswerKind> kinds, Values values, int progression = -1, int octave = 4,
                   std::function<void(PhrasePattern&)> touch = {})
{
    return {"Arps", std::move(name), std::move(description), PhrasePart::Arp, perform(std::move(values)),
            [style, seed, form, kindList = std::vector<AnswerKind>(kinds), progression, octave, touch](PhraseState& s) {
                auto& part = s.part(PhrasePart::Arp);
                part.arp = style;
                for (auto& pattern : part.slots) pattern.octave = (int8_t) octave;
                part.formLength = 1;
                part.form[0] = 0;
                phraseGenerate(s, PhrasePart::Arp, 0, seed);
                if (touch) touch(part.slots[0]);
                int slot = 1;
                for (auto kind : kindList) phraseAnswer(s, PhrasePart::Arp, 0, slot++, kind);
                if (style.bars < 8 || ! form.empty()) setForm(part, form.empty() ? "A" : form);
                if (progression >= 0) s.progression = factoryProgression(progression);
            }};
}

Phrase::Preset stabs(std::string name, std::string description, StabStyle style, uint32_t seed, std::string form,
                     std::initializer_list<AnswerKind> kinds, Values values, std::function<void(PhrasePattern&)> voicing = {},
                     int progression = -1)
{
    return {"Stabs", std::move(name), std::move(description), PhrasePart::Stabs, perform(std::move(values)),
            [style, seed, form, kindList = std::vector<AnswerKind>(kinds), voicing, progression](PhraseState& s) {
                auto& part = s.part(PhrasePart::Stabs);
                part.stabs = style;
                for (auto& pattern : part.slots)
                {
                    pattern.octave = 4;
                    pattern.voices = 4;
                    pattern.voicing = VoicingStyle::Close;
                    pattern.strumMs = 0;
                    pattern.strumDown = false;
                    if (voicing) voicing(pattern);
                }
                phraseGenerate(s, PhrasePart::Stabs, 0, seed);
                int slot = 1;
                for (auto kind : kindList) phraseAnswer(s, PhrasePart::Stabs, 0, slot++, kind);
                setForm(part, form);
                if (progression >= 0) s.progression = factoryProgression(progression);
            }};
}

Phrase::Preset pad(std::string name, std::string description, std::function<void(PhrasePattern&)> setup, Values values, int progression = -1)
{
    return {"Pads", std::move(name), std::move(description), PhrasePart::Pad, perform(std::move(values)),
            [setup, progression](PhraseState& s) {
                auto& part = s.part(PhrasePart::Pad);
                PhrasePattern p;
                p.rate = (uint8_t) kRate1_1;
                p.length = 4;
                setup(p);
                for (auto& slot : part.slots) slot = p;
                setForm(part, "A");
                if (progression >= 0) s.progression = factoryProgression(progression);
            }};
}

Phrase::Preset bass(std::string name, std::string description, BassStyle style, uint32_t seed, std::string form, Values values,
                    std::function<void(PhraseState&)> touch = {})
{
    return {"Bass", std::move(name), std::move(description), PhrasePart::Bass, perform(std::move(values)),
            [style, seed, form, touch](PhraseState& s) {
                auto& part = s.part(PhrasePart::Bass);
                part.bass = style;
                for (auto& slot : part.slots) slot.octave = 1;
                phraseGenerate(s, PhrasePart::Bass, 0, seed);
                for (int slot = 1; slot < kPhraseSlots; ++slot) part.slots[(size_t) slot] = part.slots[0];
                setForm(part, form);
                if (touch) touch(s);
            }};
}

ArpStyle arpStyle(ArpShape shape, ArpRhythm rhythm, ArpAccents accents, uint8_t bars, uint8_t range = 2, uint8_t tension = 30,
                  uint8_t passing = 20, uint8_t variation = 40, bool glide = false, uint8_t euclidHits = 7)
{
    ArpStyle s;
    s.shape = shape;
    s.rhythm = rhythm;
    s.accents = accents;
    s.bars = bars;
    s.range = range;
    s.tension = tension;
    s.passing = passing;
    s.variation = variation;
    s.accentGlide = glide;
    s.euclidHits = euclidHits;
    return s;
}

StabStyle stabStyle(StabRhythm rhythm, uint8_t hits, StabContour contour, uint8_t bars)
{
    StabStyle s;
    s.rhythm = rhythm;
    s.hits = hits;
    s.contour = contour;
    s.bars = bars;
    return s;
}

BassStyle bassStyle(BassRhythm rhythm, uint8_t jumps, uint8_t fifths, uint8_t bars)
{
    BassStyle s;
    s.rhythm = rhythm;
    s.octaveJumps = jumps;
    s.fifths = fifths;
    s.bars = bars;
    return s;
}

// A second, quieter probability line: the beats always, other steps 40-80 %.
void probability(PhrasePattern& p)
{
    for (int i = 0; i < p.length; ++i)
    {
        auto& st = p.steps[(size_t) i];
        if (st.state != StepState::Note || i % 4 == 0) continue;
        st.chance = (uint8_t) (40 + PatternRandom::range(0, 40, 17, i, 0, 1));
        st.velocity = (uint8_t) std::min<int>(st.velocity, 80);
    }
}

std::vector<Phrase::Preset> build()
{
    using A = ArpShape;
    using R = ArpRhythm;
    using Ac = ArpAccents;
    using K = AnswerKind;
    std::vector<Phrase::Preset> list;
    // ---- Arps (22)
    list.push_back(arp("Pedal Rider", "Sixteenths on the root, a few steps moved to the fifth and the 2nd, accents off the beat",
                       arpStyle(A::PedalLow, R::Sixteenths, Ac::OffBeat, 2, 2, 40), 11, "A", {}, {{P::KickGuard, 0.35f}}));
    list.push_back(arp("Pedal Glide", "The pedal line with long accented notes that glide on a mono synth",
                       arpStyle(A::PedalLow, R::Sixteenths, Ac::OffBeat, 2, 2, 30, 10, 40, true), 12, "A", {}, {{P::KickGuard, 0.35f}}));
    list.push_back(arp("Phrygian Tension", "An accented 2nd on step 7 or 15 that falls back to the root",
                       arpStyle(A::PedalLow, R::Sixteenths, Ac::ThreeThreeTwo, 1, 1, 95, 0, 0), 13, "A", {}, {{P::KickGuard, 0.4f}}));
    list.push_back(arp("Offbeat Runner", "Up through the chord on the three sixteenths between the kicks",
                       arpStyle(A::Up, R::Offbeats, Ac::OffBeat, 2), 14, "A", {}, {}));
    list.push_back(arp("Kick Clear", "The pedal line with the kick's sixteenth left alone",
                       arpStyle(A::PedalLow, R::Sixteenths, Ac::OffBeat, 2, 2, 30), 15, "A", {}, {{P::KickGuard, 1}}));
    list.push_back(arp("Three Three Two", "Thirds in a 3-3-2 rhythm with 3-3-2 accents",
                       arpStyle(A::Thirds, R::ThreeThreeTwo, Ac::ThreeThreeTwo, 2), 16, "A", {}, {}));
    list.push_back(arp("Gallop Fifths", "Root, fifth and octave in a galloping rhythm",
                       arpStyle(A::RootFifthOctave, R::Gallop, Ac::OffBeat, 2, 2, 20, 0), 17, "A", {}, {{P::KickGuard, 0.3f}}));
    list.push_back(arp("Spiral Ascent", "Two up, one down, over three octaves in four bars",
                       arpStyle(A::Spiral, R::Sixteenths, Ac::PhraseEnd, 4, 3, 20, 10, 50), 18, "A", {}, {}));
    list.push_back(arp("Second Bar Turns", "Up and down; the second bar turns at its end",
                       arpStyle(A::UpDown, R::Sixteenths, Ac::OffBeat, 2, 2, 30, 20, 90), 19, "A", {}, {}));
    list.push_back(arp("Triplet Cascade", "Down through the chord in sixteenth triplets",
                       arpStyle(A::Down, R::Triplets, Ac::None, 1, 2, 0, 10, 30), 20, "A", {}, {{P::Velocity, 0.85f}}));
    list.push_back(arp("Triplet Climb", "Up in sixteenth triplets over two bars",
                       arpStyle(A::Up, R::Triplets, Ac::PhraseEnd, 2, 3, 0, 0, 40), 21, "A", {}, {}));
    list.push_back(arp("Eight Bar Question", "Four bars ask, four bars answer and come home to the root",
                       arpStyle(A::UpDown, R::Sixteenths, Ac::OffBeat, 8, 2, 30, 20, 50), 22, "", {}, {{P::KickGuard, 0.3f}}));
    list.push_back(arp("Call And Answer", "A high pedal line in two bars, answered by two that close",
                       arpStyle(A::PedalHigh, R::Sixteenths, Ac::OffBeat, 2, 2, 30, 10), 23, "A B", {K::Answer}, {}));
    list.push_back(arp("Three Asks", "One bar asked three times, then the answer", arpStyle(A::Thirds, R::Sixteenths, Ac::OffBeat, 1), 24,
                       "A A A B", {K::Answer}, {}));
    list.push_back(arp("Sparkle High", "Thin high notes over three octaves", arpStyle(A::Up, R::Sixteenths, Ac::None, 2, 3, 0, 30, 60), 25,
                       "A", {}, {{P::Density, 0.45f}, {P::Octave, 1}, {P::Velocity, 0.7f}, {P::Humanize, 0.4f}}));
    list.push_back(arp("Probability Sparkle", "A quieter second line an octave up: beats always, the rest now and then",
                       arpStyle(A::Walk, R::Sixteenths, Ac::None, 2, 2, 0, 20, 40), 26, "A", {},
                       {{P::Octave, 1}, {P::Velocity, 0.75f}, {P::Humanize, 0.3f}}, -1, 4, probability));
    list.push_back(arp("Euclid Seven", "Seven hits spread over sixteen, spiralling", arpStyle(A::Spiral, R::Euclid, Ac::OffBeat, 2, 2, 20, 10, 40, false, 7),
                       27, "A", {}, {}));
    list.push_back(arp("Euclid Five Walk", "Five hits in sixteen on a random walk through the chord",
                       arpStyle(A::Walk, R::Euclid, Ac::None, 2, 2, 0, 20, 40, false, 5), 28, "A", {}, {{P::Swing, 0.2f}}));
    list.push_back(arp("Low Root Fifth", "Root, fifth, octave, fifth in the low register", arpStyle(A::RootFifthOctave, R::Sixteenths, Ac::OffBeat, 1, 1, 0, 0, 0),
                       29, "A", {}, {{P::KickGuard, 0.5f}}, -1, 3));
    list.push_back(arp("Swung Thirds", "Rolling thirds with a swing on the off sixteenths", arpStyle(A::Thirds, R::Sixteenths, Ac::OffBeat, 2), 30,
                       "A", {}, {{P::Swing, 0.35f}}));
    list.push_back(arp("Dark Walk", "A narrow walk an octave down, off the beat", arpStyle(A::Walk, R::Offbeats, Ac::None, 2, 1, 20, 30, 40), 31,
                       "A", {}, {{P::Octave, -1}}));
    list.push_back(arp("Build Up", "The pedal line at a third of its notes: raise Density over 16 bars to build it",
                       arpStyle(A::PedalLow, R::Sixteenths, Ac::OffBeat, 4, 2, 40, 20, 50), 32, "A", {}, {{P::Density, 0.3f}, {P::KickGuard, 0.35f}}));
    // ---- Stabs (15)
    using SR = StabRhythm;
    using SC = StabContour;
    list.push_back(stabs("Offbeat Q A", "Two bars ask on the off-beats, two answer and close", stabStyle(SR::Offbeats, 3, SC::Rise, 2), 41, "A B", {K::Answer}, {}));
    list.push_back(stabs("Q Q Q A", "A one-bar 3-3-2 call three times, then the answer", stabStyle(SR::ThreeThreeTwo, 4, SC::Rise, 1), 42, "A A A B", {K::Answer},
                         {{P::KickGuard, 0.5f}}));
    list.push_back(stabs("Q A Q A'", "Question, answer, question, a sparser answer", stabStyle(SR::Offbeats, 3, SC::Arch, 2), 43, "A B A C",
                         {K::Answer, K::Sparse}, {}));
    list.push_back(stabs("Dotted Push", "Dotted-eighth stabs falling, answered", stabStyle(SR::Dotted, 5, SC::Fall, 2), 44, "A B", {K::Answer}, {}));
    list.push_back(stabs("Anticipation", "Stabs a sixteenth before the beat, rising", stabStyle(SR::Push, 4, SC::Rise, 2), 45, "A B", {K::Answer}, {}));
    list.push_back(stabs("Sparse Call", "Two stabs a bar, then a long answer", stabStyle(SR::Sparse, 2, SC::Flat, 2), 46, "A B", {K::Answer},
                         {{P::Gate, 1.6f}}));
    list.push_back(stabs("Hard Fifths", "Root and fifth stacks, short and hard", stabStyle(SR::Offbeats, 4, SC::Rise, 2), 47, "A B", {K::Answer},
                         {{P::Gate, 0.7f}, {P::Velocity, 1.15f}}, [](PhrasePattern& p) { p.voicing = VoicingStyle::Fifths; }));
    list.push_back(stabs("Wide Soft", "Wide voicings, strummed a little, softer", stabStyle(SR::Offbeats, 3, SC::Arch, 2), 48, "A B", {K::Answer},
                         {{P::Velocity, 0.8f}, {P::Gate, 1.3f}}, [](PhrasePattern& p) { p.voicing = VoicingStyle::Wide; p.voices = 5; p.strumMs = 18; }));
    list.push_back(stabs("Drop Two Chords", "3-3-2 stabs in drop-2 voicings", stabStyle(SR::ThreeThreeTwo, 5, SC::Fall, 2), 49, "A B", {K::Answer},
                         {{P::KickGuard, 0.5f}}, [](PhrasePattern& p) { p.voicing = VoicingStyle::Drop2; }));
    list.push_back(stabs("Strum Up", "Off-beat chords strummed upwards", stabStyle(SR::Offbeats, 4, SC::Rise, 2), 50, "A B", {K::Answer}, {},
                         [](PhrasePattern& p) { p.strumMs = 35; }));
    list.push_back(stabs("Strum Down", "Dotted chords strummed downwards", stabStyle(SR::Dotted, 4, SC::Fall, 2), 51, "A B", {K::Answer}, {},
                         [](PhrasePattern& p) { p.strumMs = 30; p.strumDown = true; }));
    list.push_back(stabs("Six Voice Wall", "Six-voice open chords on the off-beats", stabStyle(SR::Offbeats, 3, SC::Flat, 2), 52, "A B", {K::Answer},
                         {{P::Velocity, 0.9f}}, [](PhrasePattern& p) { p.voices = 6; p.voicing = VoicingStyle::Open; }));
    list.push_back(stabs("Counter Line", "The call, then a counter-line that speaks in its gaps", stabStyle(SR::Offbeats, 4, SC::Rise, 2), 53, "A B",
                         {K::Counter}, {}));
    list.push_back(stabs("Mirror Answer", "A 3-3-2 arch answered upside down", stabStyle(SR::ThreeThreeTwo, 4, SC::Arch, 2), 54, "A B", {K::Mirror},
                         {{P::KickGuard, 0.5f}}));
    list.push_back(stabs("Rising Eight", "Four bars of pushes rising, then their answer", stabStyle(SR::Push, 6, SC::Rise, 4), 55, "A B", {K::Answer}, {}));
    // ---- Pads (12)
    auto padBase = [](VoicingStyle voicing, int voices, PadMotion motion, PadBass padBass, int colour, int shimmer = 0, int bars = 2,
                      int strum = 0, int overlap = 30) {
        return [=](PhrasePattern& p) {
            p.padVoicing = voicing;
            p.padVoices = (uint8_t) voices;
            p.motion = motion;
            p.padBass = padBass;
            p.colour = (uint8_t) colour;
            p.shimmer = (uint8_t) shimmer;
            p.motionBars = (uint8_t) bars;
            p.padStrumMs = (uint8_t) strum;
            p.overlap = (uint8_t) overlap;
            p.padCentre = 62;
            p.padVelocity = 78;
        };
    };
    using V = VoicingStyle;
    using M = PadMotion;
    using B = PadBass;
    list.push_back(pad("Wide Bed", "Root and fifth low, colour above, common tones held", padBase(V::Wide, 5, M::Hold, B::Root, 35), {}));
    list.push_back(pad("Open Bed", "Open voicings held from chord to chord", padBase(V::Open, 4, M::Hold, B::Root, 25), {}));
    list.push_back(pad("Close Bed", "Close voicings without a bass, for a busy low end", padBase(V::Close, 4, M::Hold, B::Off, 30), {}));
    list.push_back(pad("Breathing Wide", "One inner voice moves every bar", padBase(V::Wide, 5, M::Breathe, B::Root, 30, 0, 1), {}));
    list.push_back(pad("Breathing Open", "Open voicings, a voice moving every two bars", padBase(V::Open, 5, M::Breathe, B::Off, 40, 0, 2), {}));
    list.push_back(pad("Slow Breath", "A wide bed with a slow inner movement", padBase(V::Wide, 6, M::Breathe, B::RootFifth, 20, 0, 4, 0, 60), {}));
    list.push_back(pad("Pulse Every Bar", "The chord struck again every bar", padBase(V::Close, 4, M::Pulse, B::Root, 25, 0, 1, 0, 0),
                       {{P::Gate, 0.9f}}));
    list.push_back(pad("Pulse Two Bars", "Wide chords struck every two bars", padBase(V::Wide, 5, M::Pulse, B::Root, 30, 0, 2, 0, 10), {}));
    list.push_back(pad("Shimmer Bed", "A held bed with quiet octave-up chord tones on the off eighths", padBase(V::Wide, 5, M::Hold, B::Root, 40, 35), {}));
    list.push_back(pad("Shimmer Breath", "Breathing open voicings with a little sparkle", padBase(V::Open, 5, M::Breathe, B::Off, 30, 20, 2), {}));
    list.push_back(pad("Strummed Pad", "Every chord strummed slowly upwards", padBase(V::Wide, 6, M::Hold, B::Root, 30, 0, 2, 160), {}));
    list.push_back(pad("Fifth Drone", "Root and fifth under a plain voicing, no colour", padBase(V::Close, 3, M::Hold, B::RootFifth, 0), {}));
    // ---- Bass (12)
    using BR = BassRhythm;
    list.push_back(bass("KBBB Roller", "The three sixteenths between the kicks, one root, one velocity", bassStyle(BR::KBBB, 15, 25, 4), 61, "A", {}));
    list.push_back(bass("KBBB Fifth Ends", "The roller with the fifth on every phrase end", bassStyle(BR::KBBB, 15, 100, 4), 62, "A", {}));
    list.push_back(bass("K-BB", "Two bass notes after each kick", bassStyle(BR::KxBB, 20, 30, 4), 63, "A", {}));
    list.push_back(bass("KB-B", "Bass on the second and fourth sixteenths", bassStyle(BR::KBxB, 20, 30, 4), 64, "A", {}));
    list.push_back(bass("Gallop", "A sixteenth and two thirty-seconds between the kicks", bassStyle(BR::Gallop, 10, 20, 2), 65, "A", {}));
    list.push_back(bass("Offbeat Octaves", "Low on the off-beat, the octave after it", bassStyle(BR::OffbeatOctaves, 0, 20, 4), 66, "A", {}));
    list.push_back(bass("Triplet Roll", "Two triplet eighths after each kick", bassStyle(BR::Triplet, 15, 30, 4), 67, "A", {}));
    list.push_back(bass("Jumping Roller", "KBBB with an octave jump in most bars", bassStyle(BR::KBBB, 65, 20, 4), 68, "A", {}));
    list.push_back(bass("Plain Roller", "KBBB, no jumps, no fifths: the pure floor", bassStyle(BR::KBBB, 0, 0, 4), 69, "A", {}));
    list.push_back(bass("K-BB Fifths", "K-BB that turns to the fifth at phrase ends", bassStyle(BR::KxBB, 0, 100, 2), 70, "A", {}));
    list.push_back(bass("Gallop Jumps", "The gallop with octave jumps", bassStyle(BR::Gallop, 60, 30, 2), 71, "A", {}));
    list.push_back(bass("Eight Then Fifth", "The root for eight bars, then two bars on the fifth", bassStyle(BR::KBBB, 0, 0, 4), 72, "A A B",
                        {}, [](PhraseState& s) {
                            auto& b = s.part(PhrasePart::Bass).slots[1];
                            for (int i = 0; i < b.length / 2; ++i)
                                if (b.steps[(size_t) i].state == StepState::Note) b.steps[(size_t) i].tone = 1;
                        }));
    return list;
}
} // namespace

const std::vector<Phrase::Preset>& Phrase::presets()
{
    static const std::vector<Preset> list = build();
    return list;
}
} // namespace sonora::devices

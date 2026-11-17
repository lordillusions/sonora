// Sonora Seraph (PLG-27): factory presets. Names and settings are Sonora's own.
#include "devices/native/Seraph.h"

#include <algorithm>
#include <cmath>

namespace sonora::devices
{
const std::vector<std::string>& Seraph::presetCategories()
{
    static const std::vector<std::string> list = {"Angels", "Monks", "Cathedral", "Boys", "Solo", "Chants", "Gated", "Beds", "Real", "Real Choir"};
    return list;
}

const std::vector<Seraph::Preset>& Seraph::presets()
{
    // Every preset lists only what differs from the defaults (Mixed, 5 singers, A, Tension 50 %, Breath 20 %, Vibrato 12
    // cents, Attack 120 ms, Release 600 ms, Space 35 % Church at Distance 30 %...). Key, Seed and Output stay the player's.
    constexpr float A = 0, O = 3, U = 4; // vowels (E = 1, I = 2)
    constexpr int Room = 0, Chapel = 1, Cathedral = 3, Endless = 4; // Space Size (Church = 2 is the default)
    using dsp::TranceGate;
    static const std::vector<Preset> list = {
        // ---- Angels: women, high airy beds
        {"Seraphim Aah", "Angels", "Women on a bright, airy aah in the cathedral: the classic angel bed.",
         {{Section, Women}, {Singers, 6}, {Vowel, A}, {Breath, 0.35f}, {Tension, 0.4f}, {Size, -0.04f}, {Bright, 2}, {Air, 4},
          {Attack, 400}, {Release, 1500}, {VowelMotion, 0.25f}, {Vibrato, 14}, {Space, 0.45f}, {SpaceSize, Cathedral}, {Width, 0.85f}}, {}},
        {"Halo Ooh", "Angels", "A soft women's ooh that floats behind everything.",
         {{Section, Women}, {Singers, 6}, {Vowel, U}, {Breath, 0.3f}, {Tension, 0.35f}, {Air, 3}, {Attack, 600}, {Release, 2000},
          {Space, 0.5f}, {SpaceSize, Cathedral}, {Width, 0.8f}}, {}},
        {"Breath of Heaven", "Angels", "Breathy sopranos between oh and ooh, the vowel drifting slowly.",
         {{Section, Women}, {Singers, 6}, {Vowel, 3.2f}, {Breath, 0.6f}, {Tension, 0.25f}, {VowelMotion, 0.4f}, {MotionRate, 0.08f},
          {Attack, 900}, {Release, 2000}, {Space, 0.45f}, {SpaceSize, Cathedral}, {Width, 0.85f}}, {}},
        {"Bright Seraphs", "Angels", "Bright women between ah and eh, pressed a little, lots of air.",
         {{Section, Women}, {Singers, 6}, {Vowel, 1.2f}, {Bright, 5}, {Tension, 0.6f}, {Air, 6}, {Attack, 300}, {Release, 1200},
          {Space, 0.4f}, {SpaceSize, Cathedral}}, {}},
        {"Dark Angels", "Angels", "Women on a dark oh-ooh, larger throats, further away.",
         {{Section, Women}, {Singers, 6}, {Vowel, 3.5f}, {Size, 0.08f}, {Bright, -4}, {Attack, 500}, {Release, 1800}, {Space, 0.5f},
          {SpaceSize, Cathedral}, {Distance, 0.5f}}, {}},
        {"Far Choir of Light", "Angels", "Women at the far end of the nave: little direct sound, all hall.",
         {{Section, Women}, {Singers, 6}, {Vowel, 0.5f}, {Breath, 0.3f}, {Attack, 500}, {Release, 2000}, {Space, 0.6f},
          {SpaceSize, Cathedral}, {Distance, 0.85f}}, {}},
        {"Near Whisper Angels", "Angels", "Four breathy women close to the microphone in a small chapel.",
         {{Section, Women}, {Singers, 4}, {Vowel, 0.4f}, {Breath, 0.7f}, {Tension, 0.2f}, {Attack, 250}, {Release, 900},
          {Space, 0.2f}, {SpaceSize, Chapel}, {Distance, 0.05f}}, {}},
        {"Heaven's Gate Swell", "Angels", "Women that swell in level and brightness over two seconds of a held chord.",
         {{Section, Women}, {Singers, 6}, {Vowel, A}, {Swell, 0.8f}, {Attack, 1500}, {Release, 3000}, {Breath, 0.3f}, {Space, 0.5f},
          {SpaceSize, Cathedral}}, {}},
        {"Angel Steps", "Angels", "The vowel moves from ah to oh from one note to the next.",
         {{Section, Women}, {Singers, 6}, {Vowel, A}, {VowelSteps, 1}, {Breath, 0.3f}, {Attack, 300}, {Release, 1500}, {Space, 0.45f},
          {SpaceSize, Cathedral}}, {}},
        {"Soprano Glow", "Angels", "Eight sopranos and altos with a warmer vibrato.",
         {{Section, Women}, {Singers, 8}, {Vowel, 0.3f}, {Vibrato, 18}, {VibratoRate, 5.8f}, {Bright, 2}, {Air, 5}, {Attack, 350},
          {Release, 1500}, {Space, 0.45f}, {SpaceSize, Cathedral}}, {}},
        {"Lullaby Ooh", "Angels", "A half-hummed ooh, breathy and close.",
         {{Section, Women}, {Singers, 5}, {Vowel, U}, {Hum, 0.3f}, {Breath, 0.4f}, {Attack, 800}, {Release, 2000}, {Space, 0.4f},
          {SpaceSize, Chapel}}, {}},
        {"Silver Aah", "Angels", "Small bright throats on aah, wide, with a shining top.",
         {{Section, Women}, {Singers, 6}, {Vowel, 0.2f}, {Tension, 0.55f}, {Size, -0.05f}, {Bright, 3}, {Air, 7}, {Width, 1},
          {Attack, 300}, {Release, 1500}, {Space, 0.45f}, {SpaceSize, Cathedral}}, {}},

        // ---- Monks: men, low and dark
        {"Monks Oo", "Monks", "Men on a dark ooh in a church, steady, little vibrato.",
         {{Section, Men}, {Vowel, U}, {Bright, -3}, {Tension, 0.45f}, {Vibrato, 6}, {Drift, 6}, {Attack, 250}, {Release, 1200},
          {Space, 0.4f}}, {}},
        {"Low Om", "Monks", "Six men opening from a hum into oh, larger throats.",
         {{Section, Men}, {Singers, 6}, {Vowel, 3.4f}, {Hum, 0.5f}, {Size, 0.06f}, {Onset, M}, {Vibrato, 5}, {Attack, 300},
          {Release, 1500}, {Space, 0.45f}}, {}},
        {"Hummed Basses", "Monks", "Closed-mouth basses: a warm hum under everything.",
         {{Section, Men}, {Singers, 6}, {Vowel, U}, {Hum, 0.85f}, {Warmth, 3}, {Vibrato, 4}, {Attack, 400}, {Release, 1500}}, {}},
        {"Tenor Unison", "Monks", "Men on a ringing oh, pressed a little: the line carries.",
         {{Section, Men}, {Vowel, O}, {Bright, 2}, {Tension, 0.6f}, {Vibrato, 10}, {Attack, 200}, {Release, 1000}}, {}},
        {"Cloister Oh", "Monks", "Men on oh, far down the cloister.",
         {{Section, Men}, {Vowel, O}, {Vibrato, 6}, {Attack, 300}, {Release, 1500}, {Space, 0.5f}, {SpaceSize, Cathedral},
          {Distance, 0.6f}}, {}},
        {"Dark Vigil", "Monks", "Very dark men's ooh, large throats, a long cathedral.",
         {{Section, Men}, {Singers, 6}, {Vowel, U}, {Bright, -6}, {Size, 0.1f}, {Breath, 0.15f}, {Vibrato, 4}, {Attack, 500},
          {Release, 2000}, {Space, 0.55f}, {SpaceSize, Cathedral}}, {}},
        {"Close Monks", "Monks", "Four men close to the microphone in a small room.",
         {{Section, Men}, {Singers, 4}, {Vowel, 3.5f}, {Vibrato, 6}, {Attack, 150}, {Release, 800}, {Space, 0.15f}, {SpaceSize, Room},
          {Distance, 0.05f}}, {}},
        {"Crypt Drone", "Monks", "A half-hummed ooh, slow to come and slow to go.",
         {{Section, Men}, {Singers, 6}, {Vowel, U}, {Hum, 0.6f}, {Size, 0.12f}, {Vibrato, 3}, {Attack, 1200}, {Release, 2500},
          {Space, 0.5f}, {SpaceSize, Cathedral}}, {}},
        {"Brothers Ah", "Monks", "Men on an open ah.",
         {{Section, Men}, {Vowel, 0.3f}, {Tension, 0.55f}, {Bright, 1}, {Vibrato, 8}, {Attack, 200}, {Release, 1000}}, {}},
        {"Night Office", "Monks", "Each note begins with an n and steps between oh and ooh.",
         {{Section, Men}, {Vowel, O}, {VowelSteps, 2}, {Onset, N}, {Vibrato, 6}, {Attack, 150}, {Release, 1200}, {Space, 0.45f}}, {}},
        {"Abbey Wall", "Monks", "Eight men spread wide on oh-ooh in the cathedral.",
         {{Section, Men}, {Singers, 8}, {Vowel, 3.5f}, {Width, 1}, {Vibrato, 6}, {Attack, 300}, {Release, 1500}, {Space, 0.5f},
          {SpaceSize, Cathedral}}, {}},
        {"Deep Throat Bass", "Monks", "Men with the largest throats: dark, warm, chesty.",
         {{Section, Men}, {Vowel, U}, {Size, 0.15f}, {Bright, -4}, {Warmth, 4}, {Vibrato, 5}, {Attack, 250}, {Release, 1200}}, {}},

        // ---- Cathedral: mixed choirs
        {"Choir", "Cathedral", "A mixed choir of sopranos, altos, tenors and basses on aah: the starting point.", {}, {}},
        {"Full Choir Wall", "Cathedral", "Women on the notes, men an octave below, six each, wide in the cathedral.",
         {{MenOctave, 1}, {Singers, 6}, {Vowel, 0.2f}, {Width, 1}, {Attack, 300}, {Release, 1500}, {Space, 0.45f}, {SpaceSize, Cathedral}}, {}},
        {"Soft Swell", "Cathedral", "A gentle oh-ooh that swells as it is held.",
         {{MenOctave, 1}, {Vowel, 3.5f}, {Swell, 0.8f}, {Attack, 900}, {Release, 2000}, {Space, 0.45f}, {SpaceSize, Cathedral}}, {}},
        {"Chamber Choir", "Cathedral", "Two singers a part, close, in a chapel.",
         {{Singers, 2}, {Vibrato, 10}, {Attack, 150}, {Release, 900}, {Space, 0.25f}, {SpaceSize, Chapel}, {Distance, 0.15f}}, {}},
        {"Big Bright Choir", "Cathedral", "Eight a part, men an octave down, pressed and bright.",
         {{MenOctave, 1}, {Singers, 8}, {Bright, 4}, {Air, 6}, {Tension, 0.65f}, {Vibrato, 14}, {Attack, 250}, {Release, 1500},
          {Space, 0.4f}, {SpaceSize, Cathedral}}, {}},
        {"Requiem", "Cathedral", "A dark, distant oh-ooh in a long cathedral.",
         {{MenOctave, 1}, {Vowel, 3.6f}, {Bright, -3}, {Attack, 500}, {Release, 2500}, {Space, 0.55f}, {SpaceSize, Cathedral},
          {Distance, 0.5f}}, {}},
        {"Gloria Aah", "Cathedral", "Full voices on aah with a festive vibrato.",
         {{MenOctave, 1}, {Vowel, A}, {Tension, 0.6f}, {Vibrato, 16}, {Attack, 200}, {Release, 1500}, {Space, 0.45f}, {SpaceSize, Cathedral}}, {}},
        {"Nave Echo", "Cathedral", "The choir at the far end of the nave.",
         {{MenOctave, 1}, {Vowel, 0.6f}, {Attack, 400}, {Release, 2000}, {Space, 0.6f}, {SpaceSize, Cathedral}, {Distance, 0.9f}}, {}},
        {"Evensong", "Cathedral", "The vowel moves between ah and oh from one chord to the next.",
         {{Vowel, A}, {VowelSteps, 1}, {Attack, 250}, {Release, 1500}, {Space, 0.45f}, {SpaceSize, Cathedral}}, {}},
        {"Hymn Hum", "Cathedral", "The whole choir humming, men an octave below.",
         {{MenOctave, 1}, {Vowel, U}, {Hum, 0.7f}, {Vibrato, 8}, {Attack, 400}, {Release, 1500}}, {}},

        // ---- Boys: clear, little vibrato, large space
        {"Boy Trebles", "Boys", "Boys' voices on aah: clear, almost no vibrato, a big church around them.",
         {{Section, Boys}, {Singers, 6}, {Vibrato, 4}, {Drift, 5}, {Breath, 0.25f}, {Attack, 250}, {Release, 1500}, {Space, 0.5f},
          {SpaceSize, Cathedral}}, {}},
        {"Choirboys Ooh", "Boys", "Boys on ooh from the back of the cathedral.",
         {{Section, Boys}, {Singers, 6}, {Vowel, U}, {Vibrato, 3}, {Drift, 5}, {Attack, 300}, {Release, 1800}, {Space, 0.55f},
          {SpaceSize, Cathedral}, {Distance, 0.5f}}, {}},
        {"Treble Aah", "Boys", "A brighter treble aah, a little closer.",
         {{Section, Boys}, {Singers, 6}, {Vowel, 0.4f}, {Vibrato, 5}, {Bright, 2}, {Attack, 200}, {Release, 1200}, {Space, 0.45f},
          {SpaceSize, Cathedral}}, {}},
        {"Boys Far Away", "Boys", "Boys on oh, very far away.",
         {{Section, Boys}, {Singers, 6}, {Vowel, O}, {Vibrato, 3}, {Attack, 400}, {Release, 2000}, {Space, 0.6f}, {SpaceSize, Cathedral},
          {Distance, 0.85f}}, {}},

        // ---- Solo: one singer, legato, soloist vibrato and scoop
        {"Solo Soprano", "Solo", "One soprano, legato, with a soloist's vibrato and a small scoop into each note.",
         {{Section, SoloSoprano}, {Mode, Legato}, {Vibrato, 45}, {VibratoRate, 5.6f}, {VibratoDelay, 250}, {Scoop, 30}, {Glide, 150},
          {Attack, 80}, {Release, 500}, {Space, 0.3f}, {SpaceSize, Chapel}}, {}},
        {"Solo Soprano Hall", "Solo", "The soprano in the cathedral.",
         {{Section, SoloSoprano}, {Mode, Legato}, {Vibrato, 45}, {VibratoRate, 5.6f}, {VibratoDelay, 250}, {Scoop, 30}, {Glide, 150},
          {Attack, 80}, {Release, 600}, {Space, 0.45f}, {SpaceSize, Cathedral}}, {}},
        {"Solo Alto", "Solo", "One alto, legato, warm vibrato.",
         {{Section, SoloAlto}, {Mode, Legato}, {Vowel, 0.6f}, {Vibrato, 40}, {VibratoRate, 5.3f}, {VibratoDelay, 300}, {Scoop, 25},
          {Glide, 160}, {Attack, 90}, {Release, 500}, {Space, 0.3f}, {SpaceSize, Chapel}}, {}},
        {"Solo Alto Dry", "Solo", "The alto in a small room, close.",
         {{Section, SoloAlto}, {Mode, Legato}, {Vowel, 0.6f}, {Vibrato, 40}, {VibratoRate, 5.3f}, {VibratoDelay, 300}, {Scoop, 25},
          {Glide, 160}, {Attack, 90}, {Release, 400}, {Space, 0.08f}, {SpaceSize, Room}, {Distance, 0}}, {}},
        {"Solo Tenor", "Solo", "One tenor, legato, ringing.",
         {{Section, SoloTenor}, {Mode, Legato}, {Vowel, 0.3f}, {Tension, 0.6f}, {Vibrato, 42}, {VibratoRate, 5.5f}, {VibratoDelay, 250},
          {Scoop, 35}, {Glide, 140}, {Attack, 80}, {Release, 500}, {Space, 0.3f}, {SpaceSize, Chapel}}, {}},
        {"Solo Tenor Hall", "Solo", "The tenor in the cathedral.",
         {{Section, SoloTenor}, {Mode, Legato}, {Vowel, 0.3f}, {Tension, 0.6f}, {Vibrato, 42}, {VibratoRate, 5.5f}, {VibratoDelay, 250},
          {Scoop, 35}, {Glide, 140}, {Attack, 80}, {Release, 600}, {Space, 0.45f}, {SpaceSize, Cathedral}}, {}},
        {"Solo Bass", "Solo", "One bass on oh, legato.",
         {{Section, SoloBass}, {Mode, Legato}, {Vowel, O}, {Vibrato, 35}, {VibratoRate, 5.0f}, {VibratoDelay, 300}, {Scoop, 25},
          {Glide, 180}, {Attack, 100}, {Release, 500}, {Space, 0.3f}, {SpaceSize, Chapel}}, {}},
        {"Solo Bass Dry", "Solo", "The bass close, in a small room.",
         {{Section, SoloBass}, {Mode, Legato}, {Vowel, O}, {Vibrato, 35}, {VibratoRate, 5.0f}, {VibratoDelay, 300}, {Scoop, 25},
          {Glide, 180}, {Attack, 100}, {Release, 400}, {Space, 0.08f}, {SpaceSize, Room}, {Distance, 0}}, {}},

        // ---- Chants: three-note phrases around each held note
        {"Phrygian Monks", "Chants", "Men sing 1 - b2 - 1 around the held note, a fifth above them, a drone on the root.",
         {{Section, Men}, {Chant, 1}, {Scale, 5}, {PhraseParam, 0}, {ChantRate, 1}, {Organum, Fifth}, {Ison, IsonRoot}, {Vowel, U},
          {Glide, 180}, {Vibrato, 5}, {Attack, 250}, {Release, 1500}, {Space, 0.45f}, {SpaceSize, Cathedral}}, {}},
        {"Minor Sigh", "Chants", "Men sigh down a step and back (1 - b7 - 1) in minor, with the fifth.",
         {{Section, Men}, {Chant, 1}, {Scale, 1}, {PhraseParam, 1}, {ChantRate, 1}, {Organum, Fifth}, {Vowel, O}, {Glide, 200},
          {Vibrato, 5}, {Attack, 250}, {Release, 1500}, {Space, 0.45f}, {SpaceSize, Cathedral}}, {}},
        {"Old Psalm", "Chants", "A reciting tone that falls a step at the end, quarter-bar notes, over a drone.",
         {{Section, Men}, {Chant, 1}, {Scale, 1}, {PhraseParam, 8}, {ChantRate, 0}, {Ison, IsonRoot}, {Vowel, O}, {Onset, Cycle},
          {Glide, 90}, {Vibrato, 4}, {Attack, 120}, {Release, 1200}, {Space, 0.45f}}, {}},
        {"Kyrie Rise", "Chants", "The full choir rises three steps in minor, doubled at the octave.",
         {{MenOctave, 1}, {Chant, 1}, {Scale, 1}, {PhraseParam, 2}, {ChantRate, 2}, {Organum, Octave}, {Vowel, 0.4f}, {Glide, 220},
          {Attack, 300}, {Release, 1800}, {Space, 0.5f}, {SpaceSize, Cathedral}}, {}},
        {"Lament of Nuns", "Chants", "Women fall and return in Phrygian, a fourth above, over a drone.",
         {{Section, Women}, {Chant, 1}, {Scale, 5}, {PhraseParam, 15}, {ChantRate, 1}, {Organum, Fourth}, {Ison, IsonRoot},
          {Vowel, 3.2f}, {Breath, 0.3f}, {Glide, 200}, {Attack, 300}, {Release, 1800}, {Space, 0.5f}, {SpaceSize, Cathedral}}, {}},
        {"Slow Vigil", "Chants", "Up a third and back, two bars a note, a drone of root and fifth.",
         {{Section, Men}, {Chant, 1}, {Scale, 5}, {PhraseParam, 5}, {ChantRate, 3}, {Ison, IsonRootFifth}, {Hum, 0.3f}, {Vowel, U},
          {Glide, 300}, {Vibrato, 4}, {Attack, 600}, {Release, 2500}, {Space, 0.55f}, {SpaceSize, Cathedral}}, {}},
        {"Amen Fifths", "Chants", "An amen (step above, then home) in fifths and octaves.",
         {{Chant, 1}, {Scale, 1}, {PhraseParam, 13}, {ChantRate, 2}, {Organum, FifthOctave}, {Vowel, A}, {Glide, 250}, {Attack, 400},
          {Release, 2000}, {Space, 0.5f}, {SpaceSize, Cathedral}}, {}},
        {"Third Chant", "Chants", "1 - b3 - 2 - 1 in minor with the fifth, each note a new syllable.",
         {{Section, Men}, {Chant, 1}, {Scale, 1}, {PhraseParam, 10}, {ChantRate, 1}, {Organum, Fifth}, {Onset, Cycle}, {Vowel, O},
          {Glide, 160}, {Vibrato, 5}, {Attack, 200}, {Release, 1500}, {Space, 0.45f}}, {}},
        {"Eastern Call", "Chants", "Women call up and answer down in Phrygian dominant over a drone.",
         {{Section, Women}, {Chant, 1}, {Scale, 16}, {PhraseParam, 11}, {ChantRate, 1}, {Ison, IsonRoot}, {Vowel, 0.3f}, {Glide, 180},
          {Vibrato, 14}, {Attack, 250}, {Release, 1500}, {Space, 0.45f}, {SpaceSize, Cathedral}}, {}},
        {"Drone Chapel", "Chants", "A slow low turn with the octave below and a root-and-fifth drone.",
         {{Section, Men}, {Chant, 1}, {Scale, 5}, {PhraseParam, 18}, {ChantRate, 3}, {Organum, OctaveBelow}, {Ison, IsonRootFifth},
          {Vowel, U}, {Hum, 0.2f}, {Glide, 300}, {Vibrato, 3}, {Attack, 600}, {Release, 2500}, {Space, 0.5f}, {SpaceSize, Chapel}}, {}},

        // ---- Gated: trance gate on
        {"Gated Angels", "Gated", "Women chopped in sixteenths before the church: its tail softens the gaps.",
         {{Section, Women}, {Singers, 6}, {Gate, 1}, {GatePattern, 0}, {GateLength, 0.6f}, {GateSmooth, 4}, {Attack, 200},
          {Release, 1200}, {Space, 0.3f}}, {}},
        {"Rolling Choir", "Gated", "The choir on the three sixteenths between the kicks, like the bass.",
         {{Gate, 1}, {GatePattern, 2}, {GateLength, 0.7f}, {GateSmooth, 3}, {Attack, 150}, {Release, 1000}, {Space, 0.28f}}, {}},
        {"Kick Space Pad", "Gated", "Women gated after the hall, the kick's sixteenth left empty.",
         {{Section, Women}, {Singers, 6}, {Gate, 1}, {GatePattern, 3}, {GatePosition, AfterSpace}, {GateLength, 0.75f}, {GateSmooth, 3},
          {Attack, 150}, {Release, 1000}, {Space, 0.35f}}, {}},
        {"Hard Cut Monks", "Gated", "Men on ooh in hard eighths after the space.",
         {{Section, Men}, {Vowel, U}, {Gate, 1}, {GatePattern, 1}, {GatePosition, AfterSpace}, {GateLength, 0.5f}, {GateSmooth, 2},
          {Attack, 100}, {Release, 800}, {Space, 0.4f}}, {}},
        {"3-3-2 Voices", "Gated", "The full choir in a tresillo, before the hall.",
         {{MenOctave, 1}, {Gate, 1}, {GatePattern, 5}, {GateLength, 0.8f}, {GateSmooth, 4}, {Attack, 150}, {Release, 1000},
          {Space, 0.3f}}, {}},
        {"Gallop Angels", "Gated", "Women galloping after the space.",
         {{Section, Women}, {Singers, 6}, {Gate, 1}, {GatePattern, 7}, {GatePosition, AfterSpace}, {GateLength, 0.65f}, {GateSmooth, 3},
          {Attack, 150}, {Release, 1000}, {Space, 0.4f}, {SpaceSize, Cathedral}}, {}},
        {"Triplet Heaven", "Gated", "Women in sixteenth triplets, the first of each beat closed.",
         {{Section, Women}, {Singers, 6}, {Gate, 1}, {GatePattern, 16}, {GateRate, TranceGate::SixteenthTriplet}, {GateLength, 0.6f},
          {GateSmooth, 3}, {Attack, 200}, {Release, 1200}, {Space, 0.3f}}, {}},
        {"Turnaround Choir", "Gated", "Two bars: offbeat sixteenths, then the last beat opens up.",
         {{Gate, 1}, {GatePattern, 19}, {GateLength, 0.7f}, {GateSmooth, 3}, {Attack, 150}, {Release, 1000}, {Space, 0.28f}}, {}},
        {"Duck Pump Choir", "Gated", "Every beat dips and swells back, like a sidechained pad.",
         {{MenOctave, 1}, {Gate, 1}, {GatePattern, 14}, {GateLength, 1.0f}, {GateSmooth, 12}, {Attack, 200}, {Release, 1200},
          {Space, 0.25f}, {SpaceSize, Chapel}}, {}},
        {"Custom Pulse", "Gated", "Women in a pattern of their own (edit the steps on the face).",
         {{Section, Women}, {Singers, 6}, {Gate, 1}, {GatePattern, (float) TranceGate::kFactoryPatterns}, {GatePosition, AfterSpace},
          {GateLength, 0.7f}, {GateSmooth, 3}, {Attack, 150}, {Release, 1000}, {Space, 0.4f}, {SpaceSize, Cathedral}},
         {0, 1, 0.6f, 1, 0, 1, 1, 0.6f, 0, 1, 0.6f, 1, 0, 0.6f, 1, 1}},

        // ---- Beds: very long, moving, endless space
        {"Endless Aah", "Beds", "A choir that takes three seconds to arrive and six to leave, the vowel always moving.",
         {{Attack, 3000}, {Release, 6000}, {VowelMotion, 0.7f}, {MotionRate, 0.05f}, {Space, 0.6f}, {SpaceSize, Endless}}, {}},
        {"Cloud Choir", "Beds", "Breathy women far away in an endless space.",
         {{Section, Women}, {Singers, 6}, {Breath, 0.5f}, {Tension, 0.3f}, {Attack, 2500}, {Release, 5000}, {VowelMotion, 0.8f},
          {MotionRate, 0.06f}, {Space, 0.65f}, {SpaceSize, Endless}, {Distance, 0.7f}}, {}},
        {"Abyss Hum", "Beds", "Men humming with the vowel drifting under the hum.",
         {{Section, Men}, {Singers, 6}, {Vowel, U}, {Hum, 0.6f}, {Vibrato, 4}, {Attack, 3000}, {Release, 6000}, {VowelMotion, 0.6f},
          {MotionRate, 0.05f}, {Space, 0.6f}, {SpaceSize, Endless}}, {}},
        {"Vowel Tide", "Beds", "The full range of vowels washing slowly through the choir.",
         {{VowelMotion, 1.0f}, {MotionRate, 0.1f}, {Attack, 2000}, {Release, 5000}, {Space, 0.55f}, {SpaceSize, Endless}}, {}},
        {"Distant Shore", "Beds", "The whole choir as far away as it gets.",
         {{MenOctave, 1}, {Attack, 3000}, {Release, 6000}, {VowelMotion, 0.6f}, {MotionRate, 0.04f}, {Space, 0.7f}, {SpaceSize, Endless},
          {Distance, 1.0f}}, {}},
        {"Starlight Bed", "Beds", "Breathy boys with a bright top, slowly moving.",
         {{Section, Boys}, {Singers, 6}, {Breath, 0.4f}, {Vibrato, 3}, {Air, 6}, {VowelMotion, 0.7f}, {MotionRate, 0.07f},
          {Attack, 2500}, {Release, 6000}, {Space, 0.65f}, {SpaceSize, Endless}}, {}},

        // ---- Real (PLG-27b): your own recordings sing (drop a dry, held sung note on the face); until then, the synthesis
        {"Real Section", "Real", "Your recordings as a mixed section: each singer reads them its own way. Needs a sung note.",
         {{Source, RealSource}, {Detune, 10}, {Drift, 8}, {Timing, 30}, {Attack, 200}, {Release, 1200}, {Space, 0.4f}}, {}},
        {"Real Angels", "Real", "Your recordings as a high, bright section far in the cathedral. Needs a sung note.",
         {{Source, RealSource}, {Section, Women}, {Singers, 6}, {Size, -0.04f}, {Air, 5}, {Width, 0.9f}, {Attack, 500}, {Release, 2000},
          {Space, 0.5f}, {SpaceSize, Cathedral}, {Distance, 0.5f}}, {}},
        {"Real Monks", "Real", "Your recordings low and dark, larger throats. Needs a sung note.",
         {{Source, RealSource}, {Section, Men}, {Singers, 6}, {Size, 0.08f}, {Vibrato, 5}, {Attack, 300}, {Release, 1500}, {Space, 0.45f},
          {SpaceSize, Cathedral}}, {}},
        {"Real Soloist", "Real", "One of your recordings alone, legato, with a soloist's vibrato. Needs a sung note.",
         {{Source, RealSource}, {Section, SoloTenor}, {Mode, Legato}, {Vibrato, 35}, {VibratoDelay, 250}, {Scoop, 25}, {Glide, 150},
          {Attack, 60}, {Release, 500}, {Space, 0.3f}, {SpaceSize, Chapel}}, {}},
        {"Real Chant", "Real", "Your recordings chant 1 - b2 - 1 around the held note with a fifth above and a drone. Needs a sung note.",
         {{Source, RealSource}, {Section, Men}, {Chant, 1}, {Scale, 5}, {PhraseParam, 0}, {ChantRate, 1}, {Organum, Fifth}, {Ison, IsonRoot},
          {Glide, 180}, {Vibrato, 5}, {Attack, 250}, {Release, 1500}, {Space, 0.45f}, {SpaceSize, Cathedral}}, {}},
        {"Real Gated", "Real", "Your recordings chopped in sixteenths after the space. Needs a sung note.",
         {{Source, RealSource}, {Section, Women}, {Singers, 6}, {Gate, 1}, {GatePattern, 0}, {GatePosition, AfterSpace}, {GateLength, 0.6f},
          {GateSmooth, 3}, {Attack, 150}, {Release, 1000}, {Space, 0.35f}}, {}},
        {"Real Bed", "Real", "Your recordings as an endless, slowly moving bed. Needs a sung note.",
         {{Source, RealSource}, {Singers, 6}, {Attack, 2500}, {Release, 6000}, {Detune, 12}, {Drift, 12}, {Space, 0.6f}, {SpaceSize, Endless}}, {}},

        // ---- Real Choir (PLG-27h): the Seraph Voices pack's recorded singers (VocalSet, CC BY 4.0: music made with them
        // needs the credit line). Without the pack the face offers the download and the synthesis sings meanwhile.
        {"Recorded Choir", "Real Choir", "The pack's women and men as a mixed choir in a church. Needs the Seraph Voices pack.",
         {{Source, RealSource}, {Singers, 6}, {Detune, 8}, {Timing, 30}, {Attack, 250}, {Release, 1500}, {Space, 0.45f}}, {}, "All"},
        {"Recorded Women", "Real Choir", "The pack's women, high and wide in the cathedral. Needs the Seraph Voices pack.",
         {{Source, RealSource}, {Section, Women}, {Singers, 6}, {Width, 0.9f}, {Air, 4}, {Attack, 400}, {Release, 2000}, {Space, 0.5f},
          {SpaceSize, Cathedral}}, {}, "Women"},
        {"Recorded Men", "Real Choir", "The pack's men, low and close in a church. Needs the Seraph Voices pack.",
         {{Source, RealSource}, {Section, Men}, {Singers, 6}, {Vibrato, 6}, {Attack, 250}, {Release, 1500}, {Space, 0.4f}}, {}, "Men"},
        {"Recorded Chant", "Real Choir", "The pack's men chant 1 - b2 - 1 with a fifth above and a drone. Needs the Seraph Voices pack.",
         {{Source, RealSource}, {Section, Men}, {Chant, 1}, {Scale, 5}, {PhraseParam, 0}, {ChantRate, 1}, {Organum, Fifth}, {Ison, IsonRoot},
          {Glide, 220}, {Vibrato, 5}, {Human, 0.3f}, {IsonLevel, 0.4f}, {Attack, 250}, {Release, 1500}, {Space, 0.45f}, {SpaceSize, Cathedral}},
         {}, "Men"},
        {"Recorded Gated", "Real Choir", "The pack's women in sixteenths after the space. Needs the Seraph Voices pack.",
         {{Source, RealSource}, {Section, Women}, {Singers, 6}, {Gate, 1}, {GatePattern, 0}, {GatePosition, AfterSpace}, {GateLength, 0.55f},
          {GateSmooth, 3}, {Human, 0.3f}, {Attack, 150}, {Release, 1000}, {Space, 0.35f}}, {}, "Women"},
        {"Recorded Bed", "Real Choir", "The whole pack as an endless bed. Needs the Seraph Voices pack.",
         {{Source, RealSource}, {Singers, 6}, {Attack, 2500}, {Release, 6000}, {Detune, 10}, {Drift, 10}, {Human, 0.3f}, {Space, 0.6f},
          {SpaceSize, Endless}}, {}, "All"},
    };
    return list;
}

int Seraph::matchingPreset() const
{
    const auto& list = presets();
    const auto& infos = parameters();
    for (size_t p = 0; p < list.size(); ++p)
    {
        bool same = true;
        for (int i = 0; i < NumParams && same; ++i)
        {
            if (i == Output || i == Key || i == Seed) continue; // the player's own: not part of a preset
            float expected = infos[(size_t) i].defaultValue;
            for (const auto& [param, v] : list[p].values)
                if (param == i) expected = v;
            const float tolerance = std::max(1.0e-3f, std::abs(expected) * 1.0e-3f);
            same = std::abs(value(i) - expected) <= tolerance;
        }
        if (same && ! list[p].gate.empty())
            for (size_t s = 0; s < list[p].gate.size() && same; ++s)
                same = customGateSteps() == (int) list[p].gate.size() && std::abs(customGateLevel((int) s) - list[p].gate[s]) < 1.0e-3f;
        if (same) return (int) p;
    }
    return -1;
}

void Seraph::applyPreset(int index)
{
    const auto& list = presets();
    if (index < 0 || (size_t) index >= list.size()) return;
    const auto& preset = list[(size_t) index];
    const auto& infos = parameters();
    for (int i = 0; i < NumParams; ++i)
    {
        if (i == Output || i == Key || i == Seed) continue;
        float v = infos[(size_t) i].defaultValue;
        for (const auto& [param, value] : preset.values)
            if (param == i) v = value;
        setParameter(i, v);
    }
    if (! preset.gate.empty()) setCustomGate(preset.gate.data(), (int) preset.gate.size());
    presetName_ = preset.name;
    // PLG-27h: a Real Choir preset sings the Seraph Voices pack's singers (the men, the women or all of them)
    packWanted_.clear();
    if (! preset.pack.empty())
    {
        const auto pack = findPackVoices(packsRoot);
        if (pack.folder.empty()) packWanted_ = preset.name; // the face offers the download; the synthesis sings meanwhile
        else
        {
            clearRealVoices();
            std::vector<std::string> files;
            for (const auto& singer : pack.singers)
            {
                const bool man = singer.name.rfind("Male", 0) == 0;
                if (preset.pack == "All" || (preset.pack == "Men") == man) files.insert(files.end(), singer.files.begin(), singer.files.end());
            }
            addRealFiles(files);
        }
    }
}
} // namespace sonora::devices

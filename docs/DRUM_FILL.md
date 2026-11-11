# Drum fills (PLG-20)

Right-click a MIDI clip > **Generate Drum Fill** > Last Beat / Last 2 Beats / Last Bar / Last 2 Bars. The notes in that
part of the clip (its loop end backwards) are replaced by a fill; everything before it stays. Each click is a new fill
(one undo step each): click again until one fits. Requested 2026-11-11 ("many, random, a drum fill that fits the tempo
and format of the track"). Native only: hosts like the reference do not load VST3 MIDI effects.

`src/core/DrumFill.cpp` (model layer, no audio):

1. **Kit** (`drumKitFor`): the track's first Drum Rack, pad names -> roles (kick/bd, snare/sd/rim, clap/cp, closed/open
   hat (hh, oh, ch), hi/mid/low/floor tom, crash/cymbal/splash/china, ride); pads only called "Tom" are low/mid/high by
   pitch. Pitches the clip uses that no name explains: General MIDI. No Drum Rack names at all: General MIDI defaults.
   A missing piece falls back (clap <-> snare, a tom -> another tom -> snare, open hat -> closed hat -> ride).
2. **Groove** (`analyseGroove`, over the clip before the fill): swing (how late the off 16ths are, applied to the fill's
   off 16ths), triplet feel, median kick / snare / hat velocities, density.
3. **Style**, weighted by the tempo and the kit: snare roll (8ths then 16ths, 32nds at the end sometimes, rising
   velocity), tom run (high -> mid -> low with kicks on the beats), kick and snare (syncopated, ghosts), hat build (16ths
   -> 32nds, open hats, snare pickup; 4/4 kick from 115 BPM), triplets (snare/toms, mostly when the clip is in triplets),
   break (space, then flammed snare + kick hits at the end). Ghosts, flams, humanized velocity (+-5), an accent at the end.

Tests: `sonora_tests "drum fill"` (kit from pad names and GM, groove swing/velocities, every style x 3 seeds inside the
region with kit pitches and a hit in the last beat, notes before untouched, seeds differ, tom run walks down, snare roll
speeds up, clip command + undo, audio clips refused) and `clip menu: Generate Drum Fill...` (the menu with real edits).

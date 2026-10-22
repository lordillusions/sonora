# Sonora — features

Sonora is in **Alpha**: the features below work today, but expect rough edges and keep backups of your sets.

Sonora is an open-source (AGPL-3.0) digital audio workstation for macOS. This page lists only what it does today.

## Projects & file compatibility

- Opens and saves Ableton Live 12 sets (`.als`); Live 11 sets open too.
- Saving keeps everything Sonora does not edit as it was, so a set you open and save unchanged comes out identical.
  A backup copy is kept when you overwrite a set.
- If a set uses a device or plug-in Sonora cannot run, the set still opens. Sonora lists what it skipped, and
  missing instruments get a placeholder so the MIDI stays audible.
- New Set, Save, Save As, Save a Copy and Collect All and Save, which copies every sample into the project folder.
  Recent sets, and a prompt to save unsaved changes before closing, quitting or opening another set.
- Untitled sets work right away; the first Save creates a project folder and moves the samples into it.
- Drag audio files or a set from Finder straight into the app.
- Reads WAV, AIFF, FLAC, Ogg Vorbis, MP3 and the macOS audio formats (M4A/AAC/ALAC, CAF and more).
- Loads Live theme files (`.ask`) and has a UI zoom from 80 to 200 %.

## Arrangement & Session

- Arrangement and Session views with a familiar layout: control bar, browser, detail view, mixer and status bar.
- **Session:** launch clips and scenes, with launch quantization set globally or per clip. Launch modes are Trigger,
  Gate, Toggle and Repeat, with legato and launch velocity. Scenes can carry a tempo. Stop buttons, Stop All Clips and
  Back to Arrangement.
- Launch clips from MIDI notes or controllers, or from computer-keyboard keys.
- **Arrangement:** select, move, resize, trim, split, duplicate and delete clips, also across tracks. Select time
  ranges and drag them across several tracks at once. Consolidate a range into one clip.
- Loop brace, locators, an adaptive or fixed grid with triplets, zoom around the pointer, adjustable track heights,
  and an editable tempo and time signature.
- Tracks: MIDI, audio, group (nested groups too) and return tracks. You can insert, rename, recolour, reorder,
  duplicate, delete, group, fold, hide and deactivate them. Deactivated tracks use no CPU.
- Linked MIDI clips: an edit to one clip of a linked set applies to all of them.
- Take lanes with comping: audition takes and comp the best parts into the main lane.
- Full undo and redo.
- Rebindable keyboard shortcuts for menu commands, plus context menus throughout.

## Editing: MIDI, audio, warping, automation

- **Piano roll:** draw, move, resize and delete notes. Edit velocity and note probability, transpose, and duplicate
  loops. Note names show at high zoom, and the playhead is visible while the clip plays.
- 35 scales for the set and per clip, with scale highlighting, Fold and Fold to Scale.
- MIDI transform and generate tools, and per-note expression editing (pitch, slide, pressure, release velocity).
- **Clip envelopes:** pitch bend, channel pressure and all 128 CCs inside MIDI clips.
- **Audio clips:** gain, pitch, reverse, crop, clip start/end and loop, and fades with adjustable curves.
- **Warping:** Beats, Tones, Texture, Complex and Complex Pro keep the pitch when the tempo changes; Re-Pitch follows
  the tempo. You can add and move warp markers. When you add a sample, Sonora detects its tempo, so a loop starts in sync.
- Strip Silence, and jumping between transients.
- **Automation:** lanes for mixer, native-device and plug-in parameters, with curved segments, drawing and
  point editing. Record automation, and re-enable it after you move an automated control by hand.

## Mixing & routing

- Volume, pan, sends, mute and solo (exclusive or additive), with smooth parameter changes.
- Groups and return tracks (up to 12 returns).
- Sidechain inputs for Compressor, Glue Compressor and Gate.
- Track delay and automatic plug-in delay compensation.
- **Monitor tab:** LUFS (momentary, short-term, integrated, range), sample and true peak, spectrum, oscilloscope,
  correlation meter, goniometer, balance and width for the main output.
- Metronome with count-in.

## Recording

- MIDI and audio recording into the Arrangement or Session, per-track inputs (mono or stereo), and monitoring set to
  In, Auto or Off.
- Count-in, punch in/out, and loop recording into take lanes.
- Session recording and MIDI overdub, with tracks starting and stopping independently.
- Records MIDI controllers (CC, pitch bend, channel pressure, sustain) along with notes.
- Resample the main output to a track.
- **Capture MIDI** keeps what you just played, even without pressing Record. **Capture Audio** keeps the last
  60 seconds of your enabled inputs.
- Computer MIDI keyboard.

## Native instruments

- **Sonora Prism:** Sonora's own synth engine, inspired by modern wavetable synths. It has three oscillators
  (wavetable, sample, multisample, SFZ, granular and spectral sources), a sub and a noise oscillator, two filters,
  four envelopes, ten LFOs (including chaos types), a 64-slot modulation matrix and eight macros.
  - Drag a modulation source onto any knob to route it.
  - The editor includes a built-in effects rack, an on-screen keyboard with an arpeggiator, and live modulation
    animation.
  - Saves and opens its own `.prism` presets (Save Preset… / Open Preset…), with a preset browser organised in banks
    and groups, and loads wavetables from WAV files.
  - It opens in its own window or docks in the device view.
- **Sonora Resynth:** turns a dry, one-note-at-a-time bass or melody into an editable MIDI clip plus an instrument
  that plays it back. The instrument has a harmonic editor, filter, envelopes, LFO and Mono, Legato or Poly modes.
  It can also run without the original recording.
- **Sonora FM:** four-operator FM synth with eight algorithms.
- **Sonora Vector:** compact two-oscillator wavetable synth.
- **Sonora Impulse:** eight-slot drum sampler.
- **Sonora Sampler:** multisample instrument with key and velocity zones and loops.
- Plays these Live devices found in sets with Sonora's own DSP: Simpler (Classic, One-Shot, Slice), Sampler, and
  Instrument and Drum Racks (macros, chains, key/velocity zones, choke groups).

## Native audio & MIDI effects

- Plays these Live devices found in sets with Sonora's own DSP: Utility, EQ Eight, EQ Three, Compressor,
  Glue Compressor, Limiter, Multiband Dynamics, Reverb, Delay, Auto Filter, Redux, Gate, Auto Pan, Tuner and Audio
  Effect Racks.
- Sonora's own effects:
  - **Sonora Echo:** stereo, ping-pong and mid/side delay.
  - **Sonora LFO:** volume and pan shaper with a drawable shape.
  - **Sonora Curve LFO:** curve-based modulator with twelve editable graphs and a filter.
  - A loudness meter, which also stands in for the Swiss Army Meter patch some sets use.
- MIDI effects: Arpeggiator, Note Length, Pitch, Chord, Scale, Random, Velocity and Sonora Velocity.
- You can add every native device from the browser and edit it on its own panel. EQ Eight, EQ Three, Compressor
  and Limiter have Live 12-style faces.

## Plug-in hosting (VST3/AU)

- Hosts VST3 plug-ins and restores their saved state from the set.
- Audio Unit plug-ins are scanned and loaded on macOS.
- If a set refers to a VST2 plug-in, Sonora loads its VST3 version and restores the stored parameter values where
  their names match.
- A separate scanner process means a crashing or hanging plug-in cannot take the app down. Failed plug-ins are listed
  and can be retried.
- Plug-in parameter automation, tempo and transport for plug-ins, an X/Y pad on the plug-in device, and plug-in windows.
- In the browser, plug-ins are grouped by vendor and can be tagged and filtered.

## MIDI controllers

- Set each MIDI input as Track and/or Remote.
- MIDI Map mode (Cmd+M): click a control, then move a knob. Relative encoders work too.
- Imports the MIDI mappings stored in a set. Mappings are saved per set, and transport mappings globally.
- Incoming MIDI is placed with sample accuracy.
- MIDI clock output (clock, start/continue/stop, song position).

## Real-time internet collaboration

- Share a session over the internet through Sonora's open-source, self-hostable relay server, or on the local network
  with no server at all. Nearby sessions show up automatically.
- Guests join with an invite link or by asking to join. The host approves each request and can set people as Editor
  or Listener.
- Everyone compares four safety words to confirm the connection is genuine.
- Every edit syncs to everyone, and each person has their own undo.
- You can see where other people are working: their cursors, selected clips and time selections, shown in the
  Arrangement, Session and piano roll.
- Shared transport. Mixer and device knob moves are mirrored live, and so are plug-in editor changes.
- Samples are transferred to guests automatically, and samples that guests add are shared. Guests can save their own copy.

## Built-in AI assistant & agent control (MCP)

- **Agent Control** panel (Cmd+Shift+P): a log of agent activity with readable results and an undo for agent edits.
- **MCP bridge:** lets an external AI agent inspect and edit the open project. You allow this per project and turn
  it on explicitly.
- Optional built-in chat, using your own API key for OpenAI, Anthropic, Google Gemini, DeepSeek, Z.ai GLM, OpenRouter
  or any OpenAI-compatible endpoint. Keys are stored in the macOS Keychain.
- Agents can read the project and edit MIDI notes. They can also change native-device and VST3 parameters, and add
  or remove devices. Every change can be undone.

## Stem separation & audio-to-MIDI

- **Stems:** split an audio clip into stem tracks, using a separation tool you install yourself (`audio-separator`
  or `demucs`).
- **Convert Melody, Harmony or Drums to New MIDI Track**, and **Slice to New MIDI Track.** The analysis runs locally,
  the result is editable, and the source audio stays as it was.
- **Resynthesize to New MIDI Track** (see Sonora Resynth).

## Export & rendering

- **Export Audio:** WAV, AIFF or FLAC. Render the main output, a single track or all tracks as stems, for a chosen
  range and with tails. Options: normalize, mono, dither, and a loudness report.
- Freeze and Unfreeze, Bounce Track in Place, and Bounce to New Track (including a time selection).
- Export a MIDI clip as a MIDI file.

## Updates

- Release builds update themselves on macOS. Sonora downloads small delta updates when it can and falls back to the
  full download when a delta is missing or damaged.
- Choose the Stable or Beta channel, and turn automatic checks on or off.
- An update never restarts the app while you are playing, recording or rendering, and it asks you to save first.
- Nightly builds are a separate app with their own settings, so they never touch your stable install.

## Platforms & installers

- **macOS 13 or later on Apple silicon (arm64):** the main platform, installed from a drag-to-Applications disk image.
- **Linux x64:** an early build is available as an AppImage and a `.deb`. Its interface is drawn on the CPU.
- **Windows x64:** the port is in progress and not available yet.

## Sonora Prism as a VST3 plug-in

- Sonora Prism also comes as a standalone VST3 instrument for macOS (arm64), with the same sound engine and editor.
- 778 parameters can be automated from the host, and the host saves its full state, including imported tables and
  samples.
- Installed with a macOS installer package, for all users or just you.

## For developers & integrators

- Build from source with CMake and Ninja (`scripts/bootstrap.sh` fetches the dependencies). There is also a headless
  build without the UI.
- The `SonoraPrism_VST3` build target produces the plug-in.
- Control API and MCP server (`packages/mcp-server`, Node.js 22 or later). External agents use the same typed
  operations as the built-in assistant.
- The collaboration relay server (`packages/collab-relay`) for self-hosting.
- Command-line tools: `als_dump` shows what Sonora reads from a set, and `plugin_probe` checks plug-in loading and
  state restore.

---

Ableton and Live are trademarks of Ableton AG. Sonora is not affiliated with Ableton.

#!/usr/bin/env python3
"""Writes "Sonora Demo.xml", a made-up Live 12 style set used only for website screenshots (COM-3).

Everything in it is generated here: MIDI parts from simple patterns and drum/FX samples synthesized as WAV files,
so no real project, sample pack or third-party sound appears on the site. The XML uses only the elements
Sonora reads (see tests/fixtures/two_tracks.xml and docs/ALS_FORMAT.md); it is not meant to be opened in Live.

Usage: python3 scripts/site/make_demo_set.py OUT_DIR
Then:  build/tools/ui_snapshot out.png "OUT_DIR/Sonora Demo.xml" --scale 2 --size 1440x900 ...
"""
import math
import pathlib
import random
import struct
import sys
import wave

TEMPO = 124.0
BARS = 72
SR = 44100
BEAT_SEC = 60.0 / TEMPO
rng = random.Random(7)


# ---------------------------------------------------------------- samples

def write_wav(path, samples):
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(SR)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s)) * 30000)) for s in samples))
    return len(samples)


def bar_buffer(beats=4):
    return [0.0] * int(round(beats * BEAT_SEC * SR))


def add(buf, start_sec, voice):
    i0 = int(start_sec * SR)
    for i, v in enumerate(voice):
        if i0 + i < len(buf):
            buf[i0 + i] += v


def kick():
    out, phase = [], 0.0
    for i in range(int(0.35 * SR)):
        t = i / SR
        freq = 45 + 110 * math.exp(-t * 28)
        phase += 2 * math.pi * freq / SR
        out.append(math.sin(phase) * math.exp(-t * 7.5) * 0.95)
    return out


def noise_hit(length, decay, gain, tone=0.0):
    out, prev = [], 0.0
    for i in range(int(length * SR)):
        n = rng.uniform(-1, 1)
        hp = n - prev * tone  # crude high-pass so hats sound bright
        prev = n
        out.append(hp * math.exp(-i / SR * decay) * gain)
    return out


def clap():
    out = [0.0] * int(0.3 * SR)
    for k, off in enumerate((0.0, 0.011, 0.022)):
        add(out, off, noise_hit(0.25 if k == 2 else 0.02, 18 if k == 2 else 120, 0.55, 0.6))
    return out


def make_samples(folder):
    folder.mkdir(parents=True, exist_ok=True)
    files = {}
    buf = bar_buffer()
    for b in range(4):
        add(buf, b * BEAT_SEC, kick())
    files["Kick Loop"] = ("Kick Loop.wav", write_wav(folder / "Kick Loop.wav", buf), 4)

    buf = bar_buffer()
    for b in (1, 3):
        add(buf, b * BEAT_SEC, clap())
    files["Clap Loop"] = ("Clap Loop.wav", write_wav(folder / "Clap Loop.wav", buf), 4)

    buf = bar_buffer()
    for s in range(16):
        add(buf, s * BEAT_SEC / 4, noise_hit(0.08 if s % 4 == 2 else 0.03, 60, 0.45 if s % 4 == 2 else 0.25, 0.95))
    files["Hat Loop"] = ("Hat Loop.wav", write_wav(folder / "Hat Loop.wav", buf), 4)

    riser, n = [], int(32 * BEAT_SEC * SR)
    for i in range(n):
        x = i / n
        riser.append(rng.uniform(-1, 1) * (x ** 2) * 0.7 + math.sin(2 * math.pi * (200 + 1800 * x * x) * i / SR) * x * 0.2)
    files["Riser"] = ("Riser.wav", write_wav(folder / "Riser.wav", riser), 32)

    impact, phase = [], 0.0
    for i in range(int(8 * BEAT_SEC * SR)):
        t = i / SR
        phase += 2 * math.pi * (38 + 60 * math.exp(-t * 6)) / SR
        impact.append((math.sin(phase) * 0.8 + rng.uniform(-1, 1) * 0.3 * math.exp(-t * 3)) * math.exp(-t * 0.9))
    files["Impact"] = ("Impact.wav", write_wav(folder / "Impact.wav", impact), 8)
    return files


# ---------------------------------------------------------------- XML

class Ids:
    def __init__(self):
        self.n = 1000

    def next(self):
        self.n += 1
        return self.n


ids = Ids()


def notes_xml(notes):
    by_key = {}
    for t, d, k, v in notes:
        by_key.setdefault(k, []).append((t, d, v))
    out = []
    for i, (k, evs) in enumerate(sorted(by_key.items())):
        ev = "".join(f'<MidiNoteEvent Time="{t:g}" Duration="{d:g}" Velocity="{v}" OffVelocity="64" Probability="1" IsEnabled="true" />'
                     for t, d, v in evs)
        out.append(f'<KeyTrack Id="{i}"><Notes>{ev}</Notes><MidiKey Value="{k}" /></KeyTrack>')
    return "<Notes><KeyTracks>" + "".join(out) + "</KeyTracks></Notes>"


def midi_clip(start, length, loop, name, color, notes):
    return (f'<MidiClip Id="{ids.next()}" Time="{start:g}"><CurrentStart Value="{start:g}" /><CurrentEnd Value="{start + length:g}" />'
            f'<Loop><LoopStart Value="0" /><LoopEnd Value="{loop:g}" /><StartRelative Value="0" /><LoopOn Value="true" /></Loop>'
            f'<Name Value="{name}" /><Color Value="{color}" />{notes_xml(notes)}</MidiClip>')


def audio_clip(start, length, name, color, sample, folder):
    file, frames, beats = sample
    path = folder / file
    secs = frames / SR
    return (f'<AudioClip Id="{ids.next()}" Time="{start:g}"><CurrentStart Value="{start:g}" /><CurrentEnd Value="{start + length:g}" />'
            f'<Loop><LoopStart Value="0" /><LoopEnd Value="{beats:g}" /><StartRelative Value="0" /><LoopOn Value="true" /></Loop>'
            f'<Name Value="{name}" /><Color Value="{color}" /><IsWarped Value="true" /><WarpMode Value="0" />'
            f'<WarpMarkers><WarpMarker Id="0" SecTime="0" BeatTime="0" /><WarpMarker Id="1" SecTime="{secs:.6f}" BeatTime="{beats:g}" /></WarpMarkers>'
            f'<SampleVolume Value="1" /><PitchCoarse Value="0" /><PitchFine Value="0" />'
            f'<SampleRef><FileRef><RelativePathType Value="3" /><RelativePath Value="Samples/{file}" /><Path Value="{path}" /></FileRef>'
            f'<DefaultDuration Value="{frames}" /><DefaultSampleRate Value="{SR}" /></SampleRef></AudioClip>')


def mixer(volume=0.5011872, pan=0.0, sends=(0.0, 0.0)):
    s = "".join(f'<TrackSendHolder Id="{i}"><Send><Manual Value="{v}" /><AutomationTarget Id="{ids.next()}" /></Send></TrackSendHolder>'
                for i, v in enumerate(sends))
    vol_id = ids.next()
    return (f'<Mixer><Sends>{s}</Sends><Pan><Manual Value="{pan}" /><AutomationTarget Id="{ids.next()}" /></Pan>'
            f'<Volume><Manual Value="{volume}" /><AutomationTarget Id="{vol_id}" /></Volume>'
            f'<Speaker><Manual Value="true" /><AutomationTarget Id="{ids.next()}" /></Speaker></Mixer>'), vol_id


def eq8():
    bands = ""
    for i, (mode, freq, gain, q) in enumerate(((0, 40, 0, 0.71), (3, 120, 2.5, 0.8), (3, 450, -3, 1.2), (3, 2400, 1.5, 0.9), (5, 9000, 2, 0.71))):
        bands += (f'<Bands.{i}><ParameterA><IsOn><Manual Value="true" /></IsOn><Mode><Manual Value="{mode}" /></Mode>'
                  f'<Freq><Manual Value="{freq}" /></Freq><Gain><Manual Value="{gain}" /></Gain><Q><Manual Value="{q}" /></Q></ParameterA></Bands.{i}>')
    return f'<Eq8 Id="{ids.next()}"><On><Manual Value="true" /></On><UserName Value="" /><GlobalGain><Manual Value="0" /></GlobalGain>{bands}</Eq8>'


def simple_device(tag):
    return f'<{tag} Id="{ids.next()}"><On><Manual Value="true" /></On><UserName Value="" /></{tag}>'


def track(kind, tid, name, color, group=-1, arrangement="", session=(), devices="", volume=0.5011872, pan=0.0,
          sends=(0.0, 0.0), envelopes=""):
    mix, vol_id = mixer(volume, pan, sends)
    slots = "".join(f'<ClipSlot Id="{i}"><ClipSlot><Value>{c}</Value></ClipSlot></ClipSlot>' for i, c in enumerate(session))
    if kind == "MidiTrack":
        seq = (f'<MainSequencer><ClipSlotList>{slots}</ClipSlotList>'
               f'<ClipTimeable><ArrangerAutomation><Events>{arrangement}</Events></ArrangerAutomation></ClipTimeable></MainSequencer>')
    elif kind == "AudioTrack":
        seq = (f'<MainSequencer><ClipSlotList>{slots}</ClipSlotList>'
               f'<Sample><ArrangerAutomation><Events>{arrangement}</Events></ArrangerAutomation></Sample></MainSequencer>')
    else:
        seq = f'<MainSequencer><ClipSlotList>{slots}</ClipSlotList></MainSequencer>' if kind == "GroupTrack" else ""
    env = envelopes.replace("VOLUME_ID", str(vol_id)) if envelopes else ""
    group_xml = f'<TrackGroupId Value="{group}" />' if kind != "ReturnTrack" else ""
    return (f'<{kind} Id="{tid}"><Name><EffectiveName Value="{name}" /><UserName Value="{name}" /></Name><Color Value="{color}" />'
            f'{group_xml}<DeviceChain>{mix}{seq}<DeviceChain><Devices>{devices}</Devices></DeviceChain></DeviceChain>'
            f'<AutomationEnvelopes><Envelopes>{env}</Envelopes></AutomationEnvelopes></{kind}>')


# ---------------------------------------------------------------- music

ROOT = 45  # A2
PROGRESSION = [0, -4, 3, -2]  # Am - F - C - G (in semitones from A)


def bass_pattern():
    notes = []
    for bar, deg in enumerate(PROGRESSION):
        for step in (0, 0.75, 1.5, 2, 2.75, 3.5):
            notes.append((bar * 4 + step, 0.4, ROOT + deg + (12 if step in (1.5, 3.5) else 0), 100 if step == 0 else 84))
    return notes


def chord_pattern():
    shapes = {0: (0, 3, 7, 12), -4: (-4, 0, 3, 8), 3: (3, 7, 10, 15), -2: (-2, 2, 5, 10)}
    notes = []
    for bar, deg in enumerate(PROGRESSION):
        for iv in shapes[deg]:
            notes.append((bar * 4, 3.75, ROOT + 12 + iv, 76))
    return notes


def arp_pattern():
    notes = []
    for bar, deg in enumerate(PROGRESSION):
        tones = [0, 3, 7, 12, 15, 12, 7, 3]
        for s in range(16):
            notes.append((bar * 4 + s * 0.25, 0.2, ROOT + 24 + deg + tones[s % 8], 70 + (s % 4 == 0) * 25))
    return notes


def lead_pattern():
    phrase = [(0, 1.5, 12), (1.5, 0.5, 10), (2, 1, 7), (3, 1, 10), (4, 2, 12), (6, 1, 15), (7, 1, 14),
              (8, 1.5, 12), (9.5, 0.5, 10), (10, 2, 8), (12, 1, 7), (13, 1, 5), (14, 2, 3)]
    return [(t, d, ROOT + 24 + k, 96) for t, d, k in phrase]


def perc_pattern():
    hits = [(0.5, 60), (1.75, 62), (2.5, 60), (3.25, 64), (3.75, 62)]
    return [(bar * 4 + t, 0.2, k, 90) for bar in range(2) for t, k in hits]


def build(out, stage=None, name="Sonora Demo.xml", smp=None):
    """Writes the set. `stage` (scripts/site/make_timelapse.py) builds it part way: {"tracks": first N tracks,
    "bars": {track id: arrangement clips up to this bar}, "devices": {track id: first N devices}, "returns": bool}."""
    out = pathlib.Path(out)
    samples_dir = out / "Samples"
    smp = smp or make_samples(samples_dir)
    B = 4  # beats per bar
    stage = stage or {}

    def clip(start, length, make, tid=None):
        end = stage.get("bars", {}).get(tid, 1e9) * B
        return make(start, min(length, end - start)) if start < end else ""

    def chain(tid, *devices):
        return "".join(devices[:stage.get("devices", {}).get(tid, len(devices))])

    def bars(a, b):
        return (a * B, b * B)

    def a_clips(name, color, key, ranges, length=32, tid=None):
        return "".join(clip(s, min(length, e - s), lambda t, n: audio_clip(t, n, name, color, smp[key], samples_dir), tid)
                       for a, b in ranges for s, e in [bars(a, b)] for s in range(s, e, length))

    def m_clips(name, color, pattern, loop, ranges, length=32, tid=None):
        return "".join(clip(s, min(length, e - s), lambda t, n: midi_clip(t, n, loop, name, color, pattern), tid)
                       for a, b in ranges for s, e in [bars(a, b)] for s in range(s, e, length))

    # Song form (bars): intro 0-8, build 8-16, drop 16-32, break 32-40, build 40-48, drop 48-64, outro 64-72
    drums = [(0, 32), (40, 64), (64, 72)]
    tracks = []
    G_DRUMS, G_MUSIC, G_FX = 10, 20, 30
    tracks.append(track("GroupTrack", G_DRUMS, "Drums", 14))
    tracks.append(track("AudioTrack", 11, "Kick", 14, G_DRUMS, a_clips("Kick Loop", 14, "Kick Loop", drums, tid=11),
                        session=[audio_clip(0, 4, "Kick Loop", 14, smp["Kick Loop"], samples_dir)] * 6, devices=chain(11, eq8()), volume=0.7))
    tracks.append(track("AudioTrack", 12, "Clap", 15, G_DRUMS, a_clips("Clap Loop", 15, "Clap Loop", [(8, 32), (48, 64)], tid=12),
                        session=["", audio_clip(0, 4, "Clap Loop", 15, smp["Clap Loop"], samples_dir)] * 3, sends=(0.25, 0.0)))
    tracks.append(track("AudioTrack", 13, "Hats", 16, G_DRUMS, a_clips("Hat Loop", 16, "Hat Loop", [(16, 32), (40, 64)], tid=13),
                        session=["", "", audio_clip(0, 4, "Hat Loop", 16, smp["Hat Loop"], samples_dir)] * 2, pan=0.15))
    tracks.append(track("MidiTrack", 14, "Perc", 17, G_DRUMS, m_clips("Perc", 17, perc_pattern(), 8, [(16, 32), (48, 64)], tid=14),
                        session=[midi_clip(0, 8, 8, "Perc", 17, perc_pattern())] * 4, pan=-0.2, sends=(0.2, 0.15)))

    tracks.append(track("GroupTrack", G_MUSIC, "Music", 2))
    tracks.append(track("MidiTrack", 21, "Bass", 2, G_MUSIC, m_clips("Bass", 2, bass_pattern(), 16, [(8, 32), (40, 72)], tid=21),
                        session=[midi_clip(0, 16, 16, "Bass", 2, bass_pattern())] * 6,
                        devices=chain(21, eq8(), simple_device("Compressor2"), simple_device("StereoGain")), volume=0.6))
    volume_ride = ('<AutomationEnvelope Id="0"><EnvelopeTarget><PointeeId Value="VOLUME_ID" /></EnvelopeTarget><Automation><Events>'
                   '<FloatEvent Id="0" Time="-63072000" Value="0.3" /><FloatEvent Id="1" Time="0" Value="0.3" />'
                   '<FloatEvent Id="2" Time="32" Value="0.6" /><FloatEvent Id="3" Time="128" Value="0.6" />'
                   '<FloatEvent Id="4" Time="160" Value="0.25" /><FloatEvent Id="5" Time="192" Value="0.6" /></Events></Automation></AutomationEnvelope>')
    tracks.append(track("MidiTrack", 22, "Chords", 26, G_MUSIC, m_clips("Chords", 26, chord_pattern(), 16, [(0, 72)], tid=22),
                        session=[midi_clip(0, 16, 16, "Chords", 26, chord_pattern())] * 6, sends=(0.45, 0.1), envelopes=volume_ride))
    tracks.append(track("MidiTrack", 23, "Arp", 6, G_MUSIC, m_clips("Arp", 6, arp_pattern(), 16, [(16, 32), (40, 64)], tid=23),
                        session=["", "", midi_clip(0, 16, 16, "Arp", 6, arp_pattern())] * 2, devices=chain(23, simple_device("SonoraWavetable")),
                        sends=(0.2, 0.4), pan=0.25))
    tracks.append(track("MidiTrack", 24, "Lead", 9, G_MUSIC, m_clips("Lead", 9, lead_pattern(), 16, [(16, 32), (48, 64)], tid=24),
                        session=["", "", midi_clip(0, 16, 16, "Lead", 9, lead_pattern()), "", midi_clip(0, 16, 16, "Lead", 9, lead_pattern())],
                        devices=chain(24, simple_device("SonoraEcho"), simple_device("Delay"), simple_device("Reverb")), sends=(0.3, 0.25)))
    tracks.append(track("MidiTrack", 25, "Pad", 24, G_MUSIC, m_clips("Pad", 24, chord_pattern(), 16, [(32, 48)], tid=25),
                        session=["", "", "", midi_clip(0, 16, 16, "Pad", 24, chord_pattern())], devices=chain(25, simple_device("SonoraCurveLfo")),
                        sends=(0.6, 0.0)))

    tracks.append(track("GroupTrack", G_FX, "FX", 40))
    tracks.append(track("AudioTrack", 31, "Riser", 40, G_FX,
                        clip(8 * B, 8 * B, lambda t, n: audio_clip(t, n, "Riser", 40, smp["Riser"], samples_dir), 31)
                        + clip(40 * B, 8 * B, lambda t, n: audio_clip(t, n, "Riser", 40, smp["Riser"], samples_dir), 31), sends=(0.3, 0.2)))
    tracks.append(track("AudioTrack", 32, "Impact", 41, G_FX,
                        clip(16 * B, 2 * B, lambda t, n: audio_clip(t, n, "Impact", 41, smp["Impact"], samples_dir), 32)
                        + clip(48 * B, 2 * B, lambda t, n: audio_clip(t, n, "Impact", 41, smp["Impact"], samples_dir), 32), sends=(0.4, 0.0)))

    tracks = tracks[:stage.get("tracks", len(tracks))]
    returns = "" if not stage.get("returns", True) else (track("ReturnTrack", 90, "A-Reverb", 1, devices=simple_device("Reverb"))
               + track("ReturnTrack", 91, "B-Delay", 3, devices=simple_device("Delay")))
    scenes = "".join(f'<Scene Id="{i}"><Name Value="{n}" /></Scene>' for i, n in enumerate(("Intro", "Groove", "Drop", "Break", "Drop 2", "Outro")))
    xml = (f'<?xml version="1.0" encoding="UTF-8"?>\n<!-- Generated by scripts/site/make_demo_set.py for website screenshots. -->\n'
           f'<Ableton MajorVersion="5" MinorVersion="12.0_12120" Creator="Ableton Live 12.1.5"><LiveSet><Tracks>{"".join(tracks)}{returns}</Tracks>'
           f'<MainTrack><DeviceChain><Mixer><Volume><Manual Value="1" /></Volume><Tempo><Manual Value="{TEMPO:g}" /></Tempo>'
           f'<TimeSignature><Manual Value="201" /></TimeSignature></Mixer><DeviceChain><Devices>{simple_device("Limiter")}</Devices></DeviceChain></DeviceChain></MainTrack>'
           f'<Scenes>{scenes}</Scenes><Locators><Locators>'
           + "".join(f'<Locator Id="{i}"><Time Value="{b * B}" /><Name Value="{n}" /></Locator>'
                     for i, (b, n) in enumerate(((0, "Intro"), (16, "Drop"), (32, "Break"), (48, "Drop 2"), (64, "Outro"))))
           + '</Locators></Locators><Transport><LoopOn Value="false" /><LoopStart Value="64" /><LoopLength Value="64" /><CurrentTime Value="72" /></Transport>'
           '</LiveSet></Ableton>\n')
    (out / name).write_text(xml, encoding="utf-8")
    print(out / name)
    return smp


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    build(sys.argv[1])

#!/usr/bin/env python3
"""LIB-1: renders Sonora's own sample packs (CC0) into pack source folders for tools/pack_tool.

Every sound is synthesised here from oscillators, noise and filters: no recording, sample, preset or other third-party
material is read. Output is deterministic (fixed seeds), so the same commit renders the same files.

    python3 scripts/packs/render_sonora_packs.py <out-folder> [--only <pack-id>] [--quick]

<out-folder>/<pack name>/ gets the WAVs, pack-source.json (for pack_tool build), LICENSE.txt (CC0 1.0) and README.txt.
--quick renders a few sounds per pack (tests and dry runs). See docs/SAMPLE_PACKS.md.
Standard library only (Python 3.8+).
"""

import argparse
import json
import math
import os
import random
import struct
import subprocess
import sys
import wave

RATE = 44100
HERE = os.path.dirname(os.path.abspath(__file__))
TAU = 2.0 * math.pi


# ---------------------------------------------------------------- DSP building blocks (lists of floats, mono unless noted)

def silence(seconds):
    return [0.0] * int(seconds * RATE)


def env_exp(n, decay, attack=0.0005):
    """Exponential decay with a short linear attack; `decay` = seconds to fall by 1/e."""
    a = max(1, int(attack * RATE))
    k = math.exp(-1.0 / (decay * RATE))
    out, level = [], 1.0
    for i in range(n):
        if i < a:
            out.append(i / a)
        else:
            out.append(level)
            level *= k
    return out


def sweep_sine(seconds, f_start, f_end, sweep, phase=0.0):
    """Sine whose frequency falls exponentially from f_start to f_end (time constant `sweep` s)."""
    n = int(seconds * RATE)
    out = []
    for i in range(n):
        t = i / RATE
        f = f_end + (f_start - f_end) * math.exp(-t / sweep)
        phase += TAU * f / RATE
        out.append(math.sin(phase))
    return out


def noise(n, rng):
    return [rng.uniform(-1.0, 1.0) for _ in range(n)]


class Biquad:
    """RBJ cookbook biquad (lowpass, highpass, bandpass)."""

    def __init__(self, kind, freq, q=0.707):
        self.kind, self.q = kind, q
        self.x1 = self.x2 = self.y1 = self.y2 = 0.0
        self.set(freq)

    def set(self, freq):
        freq = min(max(freq, 10.0), RATE * 0.45)
        w = TAU * freq / RATE
        cw, alpha = math.cos(w), math.sin(w) / (2.0 * self.q)
        if self.kind == "lp":
            b0, b1, b2 = (1 - cw) / 2, 1 - cw, (1 - cw) / 2
        elif self.kind == "hp":
            b0, b1, b2 = (1 + cw) / 2, -(1 + cw), (1 + cw) / 2
        else:  # constant 0 dB peak band-pass
            b0, b1, b2 = alpha, 0.0, -alpha
        a0 = 1 + alpha
        self.b0, self.b1, self.b2 = b0 / a0, b1 / a0, b2 / a0
        self.a1, self.a2 = -2 * cw / a0, (1 - alpha) / a0

    def tick(self, x):
        y = self.b0 * x + self.b1 * self.x1 + self.b2 * self.x2 - self.a1 * self.y1 - self.a2 * self.y2
        self.x2, self.x1, self.y2, self.y1 = self.x1, x, self.y1, y
        return y


def filt(signal, kind, freq, q=0.707):
    f = Biquad(kind, freq, q)
    return [f.tick(x) for x in signal]


def filt_sweep(signal, kind, f_start, f_end, q=0.707, curve=1.0):
    f = Biquad(kind, f_start, q)
    n = max(1, len(signal) - 1)
    out = []
    for i, x in enumerate(signal):
        if i % 32 == 0:
            p = (i / n) ** curve
            f.set(f_start * (f_end / f_start) ** p)
        out.append(f.tick(x))
    return out


def mul(a, b):
    return [x * y for x, y in zip(a, b)]


def mix(*parts):
    n = max(len(p[0]) for p in parts)
    out = [0.0] * n
    for signal, gain in parts:
        for i, x in enumerate(signal):
            out[i] += x * gain
    return out


def saturate(signal, drive):
    norm = math.tanh(drive)
    return [math.tanh(x * drive) / norm for x in signal]


def squares(n, freqs, rng):
    """Sum of square waves at metallic ratios (cymbals, hats, cowbells)."""
    phases = [rng.random() for _ in freqs]
    out = []
    for i in range(n):
        s = 0.0
        for k, f in enumerate(freqs):
            phases[k] = (phases[k] + f / RATE) % 1.0
            s += 1.0 if phases[k] < 0.5 else -1.0
        out.append(s / len(freqs))
    return out


SAW_TABLES = {}


def saw_table(max_harmonic):
    """One band-limited saw cycle (2048 points) with harmonics up to `max_harmonic`."""
    if max_harmonic not in SAW_TABLES:
        size = 2048
        SAW_TABLES[max_harmonic] = [sum(math.sin(TAU * h * i / size) / h for h in range(1, max_harmonic + 1)) * 0.55
                                    for i in range(size)]
    return SAW_TABLES[max_harmonic]


def saw(seconds, freq, rng, detune_cents=0.0, vibrato=0.0):
    n = int(seconds * RATE)
    f = freq * 2 ** (detune_cents / 1200.0)
    table = saw_table(max(1, min(40, int(RATE * 0.45 / f))))
    size = len(table)
    phase = rng.random() * size
    out = []
    for i in range(n):
        step = f * (1.0 + vibrato * math.sin(TAU * 0.21 * i / RATE)) * size / RATE
        j = int(phase)
        frac = phase - j
        out.append(table[j % size] + (table[(j + 1) % size] - table[j % size]) * frac)
        phase = (phase + step) % size
    return out


def reverb(signal, mix_level=0.3, size=1.0, tail=1.5):
    """Schroeder reverb (4 combs, 2 allpasses) returning a stereo pair; the tail is appended."""
    padded = signal + [0.0] * int(tail * RATE)
    def comb_set(delays):
        out = [0.0] * len(padded)
        for d in delays:
            d = int(d * size)
            buf = [0.0] * d
            feedback, k = 0.80, 0
            for i, x in enumerate(padded):
                y = buf[k]
                buf[k] = x + y * feedback
                k = (k + 1) % d
                out[i] += y * 0.25
        for d, g in ((556, 0.5), (441, 0.5)):
            buf, k = [0.0] * d, 0
            for i, x in enumerate(out):
                b = buf[k]
                y = -x * g + b
                buf[k] = x + b * g
                k = (k + 1) % d
                out[i] = y
        return out
    left = comb_set([1557, 1617, 1491, 1422])
    right = comb_set([1580, 1640, 1514, 1445])
    dry = 1.0 - mix_level
    return ([x * dry + w * mix_level for x, w in zip(padded, left)],
            [x * dry + w * mix_level for x, w in zip(padded, right)])


def trim_fade(signal, fade=0.005):
    n = len(signal)
    f = min(n, int(fade * RATE))
    for i in range(f):
        signal[n - 1 - i] *= i / f
    return signal


def normalize(channels, peak_db=-1.0):
    peak = max(max(abs(x) for x in ch) for ch in channels) or 1.0
    gain = 10 ** (peak_db / 20.0) / peak
    return [[x * gain for x in ch] for ch in channels]


def remove_dc(signal):
    return filt(signal, "hp", 12.0)


def write_wav(path, channels):
    """24-bit PCM. `channels` is a list of 1 or 2 equal-length lists."""
    channels = normalize([trim_fade(remove_dc(ch)) for ch in channels])
    n = min(len(ch) for ch in channels)
    frames = bytearray()
    for i in range(n):
        for ch in channels:
            v = max(-8388608, min(8388607, int(round(ch[i] * 8388607))))
            frames += struct.pack("<i", v)[:3]
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with wave.open(path, "wb") as w:
        w.setnchannels(len(channels))
        w.setsampwidth(3)
        w.setframerate(RATE)
        w.writeframes(bytes(frames))


# ---------------------------------------------------------------- drum voices

def kick(rng, tune=50.0, punch=180.0, decay=0.35, click=0.3, drive=1.5, length=0.7):
    body = sweep_sine(length, punch, tune, 0.03)
    n = len(body)
    tone = mul(body, env_exp(n, decay))
    tick = mul(filt(noise(n, rng), "bp", 3500, 0.8), env_exp(n, 0.004))
    return saturate(mix((tone, 1.0), (tick, click)), drive)


def eight_o_eight(rng, freq, decay=0.9, length=2.2, drive=2.2):
    body = sweep_sine(length, freq * 2.2, freq, 0.025)
    n = len(body)
    tone = mul(body, env_exp(n, decay))
    tick = mul(filt(noise(n, rng), "hp", 2000), env_exp(n, 0.003))
    return saturate(mix((tone, 1.0), (tick, 0.15)), drive)


def snare(rng, tone_hz=185.0, snap=0.12, noise_decay=0.16, length=0.5, tone_mix=0.55):
    n = int(length * RATE)
    tone = mix((sweep_sine(length, tone_hz * 1.6, tone_hz, 0.02), 0.7), (sweep_sine(length, tone_hz * 2.9, tone_hz * 1.8, 0.02), 0.3))
    tone = mul(tone, env_exp(n, snap))
    hiss = filt(filt(noise(n, rng), "hp", 1800), "lp", 9000)
    hiss = mul(hiss, env_exp(n, noise_decay))
    return saturate(mix((tone, tone_mix), (hiss, 1.0 - tone_mix + 0.2)), 1.3)


def clap(rng, freq=1200.0, bursts=4, spread=0.011, tail=0.18, length=0.6):
    n = int(length * RATE)
    source = filt(noise(n, rng), "bp", freq, 1.2)
    envelope = [0.0] * n
    for b in range(bursts):
        start = int(b * spread * RATE * rng.uniform(0.8, 1.2))
        decay = tail if b == bursts - 1 else 0.006
        k = math.exp(-1.0 / (decay * RATE))
        level = 1.0
        for i in range(start, n):
            envelope[i] = max(envelope[i], level)
            level *= k
    return mul(source, envelope)


HAT_FREQS = [205.3, 304.4, 369.6, 522.7, 540.0, 800.0]


def hat(rng, decay=0.05, length=0.4, color=1.0, cutoff=7500):
    n = int(length * RATE)
    metal = squares(n, [f * color * 2.0 for f in HAT_FREQS], rng)
    body = filt(filt(metal, "bp", 10000, 0.9), "hp", cutoff)
    air = filt(noise(n, rng), "hp", 9000)
    return mul(mix((body, 0.8), (air, 0.35)), env_exp(n, decay))


def ride(rng, decay=0.9, length=2.5, color=1.0):
    n = int(length * RATE)
    metal = squares(n, [f * color * 1.4 for f in HAT_FREQS] + [1170.0 * color, 1420.0 * color], rng)
    body = filt(filt(metal, "bp", 6500, 0.7), "hp", 3500)
    ping = mul(sweep_sine(length, 3100 * color, 2900 * color, 0.5), env_exp(n, 0.25))
    return mul(mix((body, 0.8), (ping, 0.15)), env_exp(n, decay))


def tom(rng, freq, decay=0.25, length=0.8):
    body = sweep_sine(length, freq * 1.7, freq, 0.05)
    n = len(body)
    hit = mul(filt(noise(n, rng), "bp", freq * 4, 1.0), env_exp(n, 0.01))
    return mix((mul(body, env_exp(n, decay)), 1.0), (hit, 0.3))


def rim(rng, freq=1700.0, length=0.15):
    n = int(length * RATE)
    click = filt(mix((noise(n, rng), 0.6), (sweep_sine(length, freq, freq * 0.9, 0.01), 1.0)), "bp", freq, 2.5)
    return mul(click, env_exp(n, 0.012))


def cowbell(rng, base=540.0, length=0.5):
    n = int(length * RATE)
    tone = filt(squares(n, [base, base * 1.48], rng), "bp", base * 2.2, 1.6)
    return mul(tone, env_exp(n, 0.09))


def shaker(rng, length=0.22, bright=6000.0):
    n = int(length * RATE)
    attack = int(0.03 * RATE)
    envelope = [min(1.0, i / attack) * math.exp(-max(0, i - attack) / (0.05 * RATE)) for i in range(n)]
    return mul(filt(noise(n, rng), "hp", bright), envelope)


def conga(rng, freq, length=0.6):
    body = sweep_sine(length, freq * 1.25, freq, 0.012)
    n = len(body)
    slap = mul(filt(noise(n, rng), "bp", freq * 6, 1.5), env_exp(n, 0.006))
    return mix((mul(body, env_exp(n, 0.14)), 1.0), (slap, 0.25))


# ---------------------------------------------------------------- loops

def sequence(steps, bpm, bars, hits, swing=0.0, stereo_pan=None):
    """hits: list of (voice samples, pattern string of 16 chars per bar: 'x' full, 'o' soft, '.' rest, gain, pan -1..1)."""
    step = 60.0 / bpm / 4.0
    n = int(step * steps * bars * RATE)
    left, right = [0.0] * n, [0.0] * n
    for voice, pattern, gain, pan in hits:
        lg, rg = math.cos((pan + 1) * math.pi / 4), math.sin((pan + 1) * math.pi / 4)
        for bar in range(bars):
            for s, mark in enumerate(pattern):
                if mark == ".":
                    continue
                velocity = 1.0 if mark == "x" else 0.5
                t = (bar * steps + s) * step + (swing * step if s % 2 == 1 else 0.0)
                start = int(t * RATE)
                for i, x in enumerate(voice):
                    j = start + i
                    if j >= n:
                        break
                    left[j] += x * gain * velocity * lg
                    right[j] += x * gain * velocity * rg
    return [left, right]


# ---------------------------------------------------------------- tonal sounds

NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]


def midi_hz(note):
    return 440.0 * 2 ** ((note - 69) / 12.0)


def note_name(note):
    return NOTE_NAMES[note % 12] + str(note // 12 - 1)


def pad(rng, notes, seconds=8.0, cutoff=1400.0, detune=9.0):
    n = int(seconds * RATE)
    voices = []
    for note in notes:
        for d in (-detune, 0.0, detune):
            voices.append((saw(seconds, midi_hz(note), rng, d, vibrato=0.0015), 1.0 / (len(notes) * 3)))
    body = mix(*voices)
    body = filt_sweep(body, "lp", cutoff * 0.5, cutoff * 1.6, 0.9, curve=0.6)
    attack, release = int(1.8 * RATE), int(2.5 * RATE)
    envelope = [min(1.0, i / attack) * min(1.0, (n - i) / release) for i in range(n)]
    return reverb(mul(body, envelope), 0.35, 1.2, 2.0)


def drone(rng, root, seconds=12.0):
    n = int(seconds * RATE)
    parts = [(saw(seconds, midi_hz(root), rng, -4, 0.001), 0.5), (saw(seconds, midi_hz(root + 7), rng, 5, 0.001), 0.3),
             (saw(seconds, midi_hz(root - 12), rng, 0, 0.0), 0.4)]
    body = mix(*parts)
    lfo_filter = Biquad("lp", 400, 1.4)
    out = []
    for i, x in enumerate(body):
        if i % 64 == 0:
            lfo_filter.set(500 + 380 * math.sin(TAU * 0.07 * i / RATE))
        out.append(lfo_filter.tick(x))
    fade = int(3.0 * RATE)
    out = [x * min(1.0, i / fade) * min(1.0, (n - i) / fade) for i, x in enumerate(out)]
    return reverb(out, 0.4, 1.4, 2.5)


def texture(rng, seconds=6.0, centre=900.0, q=6.0, rate=0.3):
    n = int(seconds * RATE)
    source = noise(n, rng)
    f = Biquad("bp", centre, q)
    out = []
    for i, x in enumerate(source):
        if i % 32 == 0:
            f.set(centre * (1.0 + 0.6 * math.sin(TAU * rate * i / RATE) + 0.3 * math.sin(TAU * rate * 2.7 * i / RATE)))
        out.append(f.tick(x))
    fade = int(1.0 * RATE)
    out = [x * min(1.0, i / fade) * min(1.0, (n - i) / fade) for i, x in enumerate(out)]
    return reverb(out, 0.45, 1.3, 2.0)


def riser(rng, seconds=6.0, start=200.0, end=9000.0):
    n = int(seconds * RATE)
    swept = filt_sweep(noise(n, rng), "bp", start, end, 2.0, curve=1.8)
    phase, tone = 0.0, []
    for i in range(n):
        p = i / n
        phase += TAU * (start * 0.5 * (end / start) ** (p ** 1.5)) / RATE
        tone.append(math.sin(phase))
    envelope = [(i / n) ** 2.2 for i in range(n)]
    return reverb(mul(mix((swept, 1.0), (tone, 0.25)), envelope), 0.3, 1.0, 1.0)


def downlifter(rng, seconds=3.0):
    n = int(seconds * RATE)
    swept = filt_sweep(noise(n, rng), "bp", 8000, 150, 2.0, curve=0.5)
    envelope = [(1 - i / n) ** 1.5 for i in range(n)]
    tone = mul(sweep_sine(seconds, 400, 40, 0.8), envelope)
    return reverb(mix((mul(swept, envelope), 1.0), (tone, 0.4)), 0.3, 1.0, 1.0)


def impact(rng, seconds=4.0, low=48.0):
    n = int(seconds * RATE)
    boom = mul(sweep_sine(seconds, low * 3, low, 0.06), env_exp(n, 0.9))
    crack = mul(filt(noise(n, rng), "lp", 5000), env_exp(n, 0.05))
    metal = mul(filt(squares(n, [173.0, 241.0, 389.0, 467.0], rng), "bp", 1800, 1.0), env_exp(n, 0.4))
    return reverb(saturate(mix((boom, 1.0), (crack, 0.5), (metal, 0.15)), 1.8), 0.4, 1.5, 2.5)


def whoosh(rng, seconds=1.6):
    n = int(seconds * RATE)
    shaped = []
    f = Biquad("bp", 300, 1.8)
    source = noise(n, rng)
    for i, x in enumerate(source):
        p = i / n
        if i % 32 == 0:
            f.set(300 + 5000 * math.sin(math.pi * p) ** 2)
        shaped.append(f.tick(x) * math.sin(math.pi * p) ** 1.5)
    left = [x * (1 - i / n) for i, x in enumerate(shaped)]
    right = [x * (i / n) for i, x in enumerate(shaped)]
    return [left, right]


def sub_bass(rng, note, seconds=2.0):
    f = midi_hz(note)
    n = int(seconds * RATE)
    body = mix((sweep_sine(seconds, f * 1.02, f, 0.05), 1.0), (sweep_sine(seconds, f * 2.04, f * 2, 0.05), 0.18))
    envelope = [min(1.0, i / (0.004 * RATE)) * (0.85 + 0.15 * math.exp(-i / (0.2 * RATE))) for i in range(n)]
    tail = int(0.3 * RATE)
    envelope = [e * min(1.0, (n - i) / tail) for i, e in enumerate(envelope)]
    return saturate(mul(body, envelope), 1.6)


def pluck(rng, note, seconds=1.6):
    """Karplus-Strong string (a noise burst through a damped delay loop)."""
    period = max(2, int(round(RATE / midi_hz(note))))
    buf = [rng.uniform(-1, 1) for _ in range(period)]
    out, k = [], 0
    for _ in range(int(seconds * RATE)):
        a, b = buf[k], buf[(k + 1) % period]
        buf[k] = 0.4985 * (a + b)
        out.append(a)
        k = (k + 1) % period
    return filt(out, "lp", 6000)


# ---------------------------------------------------------------- packs

class Pack:
    def __init__(self, ident, name, styles, types, tags, description):
        self.id, self.name, self.styles, self.types, self.tags, self.description = ident, name, styles, types, tags, description
        self.files, self.folder_tags, self.file_tags = [], {}, {}

    def add(self, folder, name, channels, tags=None):
        self.files.append((folder, name, channels if isinstance(channels[0], list) else [channels]))
        if tags:
            self.file_tags[folder + "/" + name] = tags


def drum_kit(ident, name, style, bpm, quick, seed, kicks, extras, loops, description, swing=0.0):
    rng = random.Random(seed)
    pack = Pack(ident, name, [style], ["Drums", "One-shots", "Loops"], [style + " drums"], description)
    count = 2 if quick else None
    voices = {}
    for folder, sound, makers in [("Kicks", "Kick", kicks)] + extras:
        pack.folder_tags[folder] = {"Sound": sound, "Kind": "One-shot"}
        for i, make in enumerate(makers[:count] if count else makers, start=1):
            samples = make(random.Random(seed * 100 + len(pack.files)))
            pack.add(folder, "%s %s %02d.wav" % (style, sound, i), samples)
            voices.setdefault(sound, []).append(samples)
    pack.folder_tags["Loops"] = {"Sound": "Drum loop", "Kind": "Loop", "BPM": str(bpm)}
    for i, pattern in enumerate(loops[:1] if quick else loops, start=1):
        hits = [(voices[sound][min(variant, len(voices[sound]) - 1)], steps, gain, pan)
                for sound, variant, steps, gain, pan in pattern if sound in voices]
        pack.add("Loops", "%s Loop %02d %d BPM.wav" % (style, i, bpm), sequence(16, bpm, 2, hits, swing))
    return pack


def house(quick):
    kicks = [lambda r, t=t, d=d: kick(r, t, 170, d, 0.35, 1.6) for t, d in [(52, .32), (55, .28), (48, .38), (58, .25), (50, .3), (54, .36)]]
    extras = [
        ("Claps", "Clap", [lambda r, f=f: clap(r, f) for f in (1100, 1300, 950, 1500)]),
        ("Snares", "Snare", [lambda r, t=t: snare(r, t, 0.1, 0.13) for t in (190, 220, 170)]),
        ("Hats/Closed", "Closed hat", [lambda r, c=c: hat(r, 0.035, 0.25, c) for c in (1.0, 1.08, 0.94, 1.15)]),
        ("Hats/Open", "Open hat", [lambda r, c=c: hat(r, 0.22, 0.8, c, 6500) for c in (1.0, 1.1, 0.9)]),
        ("Percussion", "Percussion", [lambda r: conga(r, 220), lambda r: conga(r, 330), lambda r: rim(r), lambda r: shaker(r), lambda r: cowbell(r)]),
    ]
    loops = [
        [("Kick", 0, "x...x...x...x...", 1.0, 0), ("Clap", 0, "....x.......x...", 0.7, 0), ("Closed hat", 0, "..x...x...x...x.", 0.45, 0.3)],
        [("Kick", 1, "x...x...x...x...", 1.0, 0), ("Clap", 1, "....x.......x...", 0.7, -0.1), ("Open hat", 0, "..x...x...x...x.", 0.4, 0.3),
         ("Closed hat", 1, "x.oxx.oxx.oxx.ox", 0.3, -0.3)],
        [("Kick", 2, "x...x...x...x..o", 1.0, 0), ("Snare", 0, "....x.......x...", 0.6, 0), ("Closed hat", 2, "xoxoxoxoxoxoxoxo", 0.3, 0.35),
         ("Percussion", 0, "...x..x....x.x..", 0.45, -0.4)],
        [("Kick", 3, "x...x...x...x...", 1.0, 0), ("Clap", 2, "....x.......x...", 0.65, 0), ("Open hat", 1, "..x...x...x...x.", 0.35, 0.25),
         ("Percussion", 3, "xoxoxoxoxoxoxoxo", 0.3, -0.3), ("Percussion", 4, ".......x.......x", 0.25, 0.4)],
    ]
    return drum_kit("sonora-house-kit", "Sonora House Kit", "House", 124, quick, 124, kicks, extras, loops,
                    "Synthesised four-on-the-floor house drums: kicks, claps, snares, hats, percussion and 124 BPM loops.")


def techno(quick):
    kicks = [lambda r, t=t, d=d, dr=dr: kick(r, t, 150, d, 0.25, dr, 0.9) for t, d, dr in [(45, .45, 2.4), (48, .4, 3.0), (42, .55, 2.0), (50, .35, 3.5), (46, .5, 2.8)]]
    extras = [
        ("Claps", "Clap", [lambda r, f=f: clap(r, f, 5, 0.009, 0.25) for f in (1000, 1400, 850)]),
        ("Hats/Closed", "Closed hat", [lambda r, c=c: hat(r, 0.028, 0.2, c, 8500) for c in (1.2, 1.0, 1.3)]),
        ("Hats/Open", "Open hat", [lambda r, c=c: hat(r, 0.3, 1.0, c, 7000) for c in (1.0, 1.2)]),
        ("Rides", "Ride", [lambda r, c=c: ride(r, 0.8, 2.2, c) for c in (1.0, 0.9, 1.12)]),
        ("Percussion", "Percussion", [lambda r: rim(r, 1500), lambda r: tom(r, 110, 0.3), lambda r: tom(r, 75, 0.35), lambda r: shaker(r, 0.15, 7500)]),
    ]
    loops = [
        [("Kick", 0, "x...x...x...x...", 1.0, 0), ("Closed hat", 0, "..x...x...x...x.", 0.45, 0.2), ("Ride", 0, "x.x.x.x.x.x.x.x.", 0.2, -0.3)],
        [("Kick", 1, "x...x...x...x...", 1.0, 0), ("Clap", 0, "....x.......x...", 0.55, 0), ("Open hat", 0, "..x...x...x...x.", 0.35, 0.3),
         ("Percussion", 0, "...x.....x....x.", 0.4, -0.35)],
        [("Kick", 2, "x...x...x...x...", 1.0, 0), ("Closed hat", 1, "xxxxxxxxxxxxxxxx", 0.22, 0.25), ("Percussion", 1, "......x.......x.", 0.5, -0.2),
         ("Percussion", 2, "..x.......x.....", 0.45, 0.2)],
        [("Kick", 3, "x...x...x...x...", 1.0, 0), ("Ride", 1, "..x...x...x...x.", 0.3, 0.2), ("Clap", 1, "....x.......x..o", 0.5, -0.1),
         ("Percussion", 3, "xoxoxoxoxoxoxoxo", 0.25, -0.3)],
    ]
    return drum_kit("sonora-techno-kit", "Sonora Techno Kit", "Techno", 130, quick, 130, kicks, extras, loops,
                    "Synthesised techno drums: driven kicks, claps, hats, rides, toms and 130 BPM loops.")


def hiphop(quick):
    kicks = [lambda r, t=t, p=p, d=d: kick(r, t, p, d, 0.45, 1.3, 0.8) for t, p, d in [(60, 160, .22), (55, 140, .28), (65, 190, .18), (52, 130, .3)]]
    extras = [
        ("Snares", "Snare", [lambda r, t=t, n=n: snare(r, t, 0.08, n, 0.6, 0.45) for t, n in [(200, .2), (240, .16), (175, .24), (210, .18)]]),
        ("Claps", "Clap", [lambda r, f=f: clap(r, f, 3, 0.014, 0.14) for f in (1200, 900)]),
        ("Hats/Closed", "Closed hat", [lambda r, c=c: hat(r, 0.03, 0.2, c, 7000) for c in (0.95, 1.05, 1.15)]),
        ("Hats/Open", "Open hat", [lambda r: hat(r, 0.18, 0.6, 1.0, 6500)]),
        ("Percussion", "Percussion", [lambda r: rim(r, 1900), lambda r: cowbell(r, 560), lambda r: shaker(r, 0.18, 5500)]),
    ]
    loops = [
        [("Kick", 0, "x.....x...x.....", 1.0, 0), ("Snare", 0, "....x.......x...", 0.75, 0), ("Closed hat", 0, "x.x.x.x.x.x.x.x.", 0.4, 0.3)],
        [("Kick", 1, "x......x..x..x..", 1.0, 0), ("Snare", 1, "....x.......x...", 0.75, 0), ("Closed hat", 1, "xoxoxoxoxoxoxoxo", 0.35, 0.3),
         ("Open hat", 0, "..............x.", 0.3, -0.2)],
        [("Kick", 2, "x..x......x.....", 1.0, 0), ("Snare", 2, "....x.......x...", 0.7, 0), ("Clap", 0, "....x.......x...", 0.3, 0.1),
         ("Closed hat", 2, "x.xxx.x.x.xxx.x.", 0.3, 0.35), ("Percussion", 0, ".......x........", 0.35, -0.3)],
    ]
    pack = drum_kit("sonora-hip-hop-kit", "Sonora Hip-Hop Kit", "Hip-Hop", 90, quick, 90, kicks, extras, loops,
                    "Synthesised hip-hop drums: punchy kicks, snares, hats, swung 90 BPM loops and tuned 808 basses.", swing=0.18)
    pack.folder_tags["808s"] = {"Sound": "808", "Kind": "One-shot"}
    for note in ([36] if quick else [33, 34, 36, 38, 39, 41, 43, 45]):
        pack.add("808s", "Hip-Hop 808 %s.wav" % note_name(note), eight_o_eight(random.Random(note), midi_hz(note)), {"Key": NOTE_NAMES[note % 12]})
    pack.types = ["Drums", "One-shots", "Loops", "Bass"]
    return pack


def ambient(quick):
    rng = random.Random(7)
    pack = Pack("sonora-ambient-textures", "Sonora Ambient Textures", ["Ambient"], ["Pads", "Textures", "Drones"], ["Atmosphere"],
                "Synthesised ambient pads (chords), evolving drones and filtered-noise textures, all with a reverb tail.")
    chords = [("C", [48, 55, 60, 64]), ("Am", [45, 52, 57, 60]), ("F", [41, 48, 53, 57]), ("G", [43, 50, 55, 59]),
              ("Dm7", [50, 57, 60, 65]), ("Em", [40, 47, 52, 55])]
    pack.folder_tags["Pads"] = {"Sound": "Pad", "Kind": "Chord"}
    for name, notes in chords[:1] if quick else chords:
        pack.add("Pads", "Ambient Pad %s.wav" % name, list(pad(rng, notes, 3.0 if quick else 8.0, rng.uniform(1100, 1800))), {"Key": name})
    pack.folder_tags["Drones"] = {"Sound": "Drone", "Kind": "Loop"}
    for root in [36] if quick else [36, 38, 41, 43]:
        pack.add("Drones", "Ambient Drone %s.wav" % note_name(root), list(drone(rng, root, 4.0 if quick else 12.0)), {"Key": NOTE_NAMES[root % 12]})
    pack.folder_tags["Textures"] = {"Sound": "Texture", "Kind": "One-shot"}
    for i, (centre, q, rate) in enumerate([(700, 5, 0.2), (1500, 8, 0.35), (400, 4, 0.12), (2600, 10, 0.5)][:1 if quick else None], start=1):
        pack.add("Textures", "Ambient Texture %02d.wav" % i, list(texture(rng, 2.0 if quick else 6.0, centre, q, rate)))
    return pack


def cinematic(quick):
    rng = random.Random(11)
    pack = Pack("sonora-cinematic-fx", "Sonora Cinematic FX", ["Cinematic"], ["FX"], ["Trailer", "Transitions"],
                "Synthesised impacts, booms, risers, downlifters and whooshes for trailers and transitions.")
    makers = [("Impacts", "Impact", [lambda l=l: impact(rng, 4.0, l) for l in (45, 55, 38)]),
              ("Risers", "Riser", [lambda s=s, a=a, b=b: riser(rng, s, a, b) for s, a, b in [(6, 200, 9000), (4, 300, 7000), (8, 120, 10000)]]),
              ("Downlifters", "Downlifter", [lambda s=s: downlifter(rng, s) for s in (3, 2)]),
              ("Whooshes", "Whoosh", [lambda s=s: whoosh(rng, s) for s in (1.6, 1.1, 2.2)])]
    for folder, sound, made in makers:
        pack.folder_tags[folder] = {"Sound": sound, "Kind": "FX"}
        for i, make in enumerate(made[:1] if quick else made, start=1):
            pack.add(folder, "Cinematic %s %02d.wav" % (sound, i), list(make()))
    return pack


def instruments(quick):
    rng = random.Random(3)
    pack = Pack("sonora-synth-multisamples", "Sonora Synth Multisamples", ["Electronic"], ["Instruments", "Multisamples", "Bass"],
                ["Sampler"], "Synthesised multisampled instruments: a sub bass (C1-C4) and a plucked string (C2-C5), one note every minor third.")
    pack.folder_tags["Sub Bass"] = {"Sound": "Sub bass", "Kind": "Multisample"}
    for note in [36] if quick else range(24, 61, 3):
        pack.add("Sub Bass", "Sub Bass %s.wav" % note_name(note), sub_bass(rng, note), {"Key": NOTE_NAMES[note % 12]})
    pack.folder_tags["Pluck"] = {"Sound": "Pluck", "Kind": "Multisample"}
    for note in [60] if quick else range(36, 73, 3):
        pack.add("Pluck", "Pluck %s.wav" % note_name(note), pluck(rng, note), {"Key": NOTE_NAMES[note % 12]})
    return pack


PACKS = [("sonora-house-kit", house), ("sonora-techno-kit", techno), ("sonora-hip-hop-kit", hiphop),
         ("sonora-ambient-textures", ambient), ("sonora-cinematic-fx", cinematic), ("sonora-synth-multisamples", instruments)]


def git_commit():
    try:
        return subprocess.check_output(["git", "-C", HERE, "rev-parse", "--short", "HEAD"], text=True).strip()
    except Exception:
        return "unknown"


def write_pack(pack, out, version):
    folder = os.path.join(out, pack.name)
    commit = git_commit()
    for sub, name, channels in pack.files:
        write_wav(os.path.join(folder, sub, name), channels)
    with open(os.path.join(HERE, "licences", "CC0-1.0.txt"), encoding="utf-8") as f:
        cc0 = f.read()
    with open(os.path.join(folder, "LICENSE.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("%s\n\nThe sounds in this pack were synthesised by Sonora's scripts/packs/render_sonora_packs.py (commit %s).\n"
                "The Sonora contributors dedicate them to the public domain under CC0 1.0 Universal (below):\n"
                "use them in anything, commercial or not, with no attribution needed.\n\n%s" % (pack.name, commit, cc0))
    with open(os.path.join(folder, "README.txt"), "w", encoding="utf-8", newline="\n") as f:
        f.write("%s %s\n\n%s\n\nStyles: %s\nTypes: %s\nFiles: %d (24-bit WAV, 44.1 kHz)\nLicence: CC0 1.0 (LICENSE.txt)\n"
                % (pack.name, version, pack.description, ", ".join(pack.styles), ", ".join(pack.types), len(pack.files)))
    source = {
        "id": pack.id, "name": pack.name, "version": version, "description": pack.description, "author": "Sonora contributors",
        "licence": "CC0-1.0", "styles": pack.styles, "types": pack.types, "tags": pack.tags,
        "source": "Synthesised by scripts/packs/render_sonora_packs.py at commit %s; no recorded or third-party material." % commit,
        "folderTags": pack.folder_tags, "fileTags": pack.file_tags,
    }
    with open(os.path.join(folder, "pack-source.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(source, f, indent=2)
        f.write("\n")
    print("%-28s %3d files  %s" % (pack.id, len(pack.files), folder))


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("out")
    parser.add_argument("--only", action="append", default=[])
    parser.add_argument("--quick", action="store_true")
    parser.add_argument("--version", default="1.0")
    args = parser.parse_args()
    for ident, make in PACKS:
        if args.only and ident not in args.only:
            continue
        pack = make(args.quick)
        assert pack.id == ident
        write_pack(pack, args.out, args.version)
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Seraph Voices pack (PLG-27h): a small selection of VocalSet long tones for Seraph's Real voices.

    python3 scripts/packs/make_seraph_voices.py --vocalset ~/Music/Sonora/Datasets/VocalSet/FULL \
        --out <sources>/"Seraph Voices" [--tool build/tools/seraph_demo] [--version 1.0]

Reads a local copy of "VocalSet: A Singing Voice Dataset" (Wilkins, Seetharaman, Wahl, Pardo, 2018,
https://doi.org/10.5281/zenodo.1193957, CC BY 4.0), never part of the repository. For three men and three women it finds
the held notes of their straight long tones on A, O and U (seraph_demo --pack-notes), keeps three or four per vowel spread
over the singer's range, cuts each to its attack and about three seconds of sustain, levels it and writes 44.1 kHz FLAC
(seraph_demo --pack-cut), then keeps only the notes Seraph's own analysis accepts (seraph_demo --pack-check). It writes the
pack source folder pack_tool builds: the audio, pack-source.json (with the attribution block and every file's VocalSet
path in "source"), LICENSE.txt, ATTRIBUTION.txt (its first line is the credit Seraph shows) and SOURCES.txt.
Standard library only. docs/SAMPLE_PACKS.md "The Seraph Voices pack".
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

TITLE = "VocalSet: A Singing Voice Dataset"
AUTHORS = "Julia Wilkins, Prem Seetharaman, Alison Wahl and Bryan Pardo"
SOURCE_URL = "https://doi.org/10.5281/zenodo.1193957"
LICENCE_URL = "https://creativecommons.org/licenses/by/4.0/"
CHANGES = "A selection of long tones, trimmed and levelled, in Sonora's Seraph Voices pack."
# The same line as devices::Seraph::kPackCredit (the face shows it, Copy credit copies it).
CREDIT = '"%s" by %s (%s), licensed under CC BY 4.0 (%s). %s' % (TITLE, AUTHORS, SOURCE_URL, LICENCE_URL, CHANGES)

VOWELS = {"a": "ah", "o": "oh", "u": "oo"}
NOTE_NAMES = ["C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"]
# Default singers: three women and three men of VocalSet (its own anonymous ids) whose straight long tones on A, O and U
# each give three clean held notes (a survey of all 20 with --pack-notes, 2026-11-17).
DEFAULT_SINGERS = ["female1", "female2", "female8", "male1", "male3", "male5"]


def run(tool, *args):
    out = subprocess.run([tool, *args], capture_output=True, text=True)
    return out.returncode, out.stdout


def note_name(midi):
    n = int(round(midi))
    return "%s%d" % (NOTE_NAMES[n % 12], n // 12 - 1)


def singer_label(folder):
    m = re.match(r"(female|male)(\d+)$", folder)
    return ("Female " if m.group(1) == "female" else "Male ") + m.group(2) if m else folder


def long_tone_files(root, singer, vowel):
    """The singer's straight long tone on `vowel` (falling back to other plain techniques), as paths."""
    found = []
    for technique in ("straight", "vibrato", "forte"):
        for dirpath, _, files in os.walk(os.path.join(root, singer)):
            if "long" not in dirpath.lower() or technique not in dirpath.lower():
                continue
            for name in sorted(files):
                stem = os.path.splitext(name)[0].lower()
                if name.lower().endswith(".wav") and re.search(r"[_-]%s$" % vowel, stem):
                    found.append(os.path.join(dirpath, name))
        if found:
            return found
    return found


def spread(notes, count):
    """Up to `count` notes spread over the range, at least 4 semitones apart."""
    notes = sorted(notes, key=lambda n: n[2])
    chosen = []
    for target in [i / max(1, count - 1) for i in range(count)]:
        if not notes:
            break
        lo, hi = notes[0][2], notes[-1][2]
        want = lo + target * (hi - lo)
        best = min(notes, key=lambda n: abs(n[2] - want))
        if all(abs(best[2] - c[2]) >= 4 for c in chosen):
            chosen.append(best)
    return chosen


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--vocalset", required=True, help="VocalSet's FULL folder (one folder per singer)")
    parser.add_argument("--out", required=True, help="the pack's source folder to write (replaced)")
    parser.add_argument("--tool", default="build/tools/seraph_demo")
    parser.add_argument("--singers", default=",".join(DEFAULT_SINGERS))
    parser.add_argument("--per-vowel", type=int, default=3)
    parser.add_argument("--seconds", type=float, default=3.4, help="sustain kept after the attack")
    parser.add_argument("--version", default="1.0")
    args = parser.parse_args()

    root = os.path.expanduser(args.vocalset)
    out = os.path.expanduser(args.out)
    if os.path.exists(out):
        shutil.rmtree(out)
    os.makedirs(out)
    sources, file_tags, written = [], {}, []
    for singer in args.singers.split(","):
        label = singer_label(singer)
        for vowel, sound in VOWELS.items():
            notes = []
            for path in long_tone_files(root, singer, vowel):
                code, text = run(args.tool, "--pack-notes", path)
                for line in text.split("\n"):
                    parts = line.split()
                    if len(parts) == 3:
                        notes.append((path, float(parts[0]), float(parts[1]), float(parts[2])))
            # spread() looks at the third field: (path, start, midi, end)
            for path, start, midi, end in spread([(p, a, m, b) for p, a, b, m in notes], args.per_vowel):
                name = "%s %s %s.flac" % (label, sound, note_name(midi))
                target = os.path.join(out, label, name)
                if os.path.exists(target):
                    continue
                cut_from, cut_to = max(0.0, start - 0.25), min(end, start + args.seconds)
                code, _ = run(args.tool, "--pack-cut", path, "%.3f" % cut_from, "%.3f" % cut_to, target)
                if code != 0:
                    print("could not cut", path, file=sys.stderr)
                    continue
                rel = os.path.relpath(path, os.path.dirname(root.rstrip("/")))
                sources.append("%s/%s <- VocalSet/%s, %.2f-%.2f s" % (label, name, rel, cut_from, cut_to))
                file_tags["%s/%s" % (label, name)] = {"Key": NOTE_NAMES[int(round(midi)) % 12], "Vowel": sound.capitalize()}
                written.append(target)
            print("%s %s: %d notes" % (label, sound, sum(1 for w in written if ("/%s/" % label) in w and (" %s " % sound) in w)))
    # Seraph's own analysis decides: a note it would refuse does not ship
    code, text = run(args.tool, "--pack-check", *written)
    for line in text.split("\n"):
        if line.startswith("refused "):
            path = line[len("refused "):].split(": ")[0]
            print("dropped (Seraph refuses it):", line, file=sys.stderr)
            if os.path.exists(path):
                os.remove(path)
            sources[:] = [s for s in sources if os.path.basename(path) not in s]
            file_tags.pop(os.path.relpath(path, out), None)
    kept = [w for w in written if os.path.exists(w)]

    with open(os.path.join(out, "LICENSE.txt"), "w") as f:
        f.write("The recordings in this pack are from \"%s\" by %s (%s).\n\n" % (TITLE, AUTHORS, SOURCE_URL))
        f.write("They are licensed under the Creative Commons Attribution 4.0 International licence (CC BY 4.0).\n")
        f.write("The full licence text: %slegalcode\n\n" % LICENCE_URL)
        f.write("You may share and adapt them, also commercially, if you give appropriate credit, link to the licence and say\n")
        f.write("that changes were made (see ATTRIBUTION.txt). Music made with these recordings must carry that credit where\n")
        f.write("credits go (release notes, a description).\n")
    with open(os.path.join(out, "ATTRIBUTION.txt"), "w") as f:
        f.write(CREDIT + "\n\n")
        f.write("Title: %s\nAuthors: %s\nSource: %s\nLicence: CC BY 4.0, %s\nChanges: %s\n\n" % (TITLE, AUTHORS, SOURCE_URL, LICENCE_URL, CHANGES))
        f.write("Music made with these voices: put the first line of this file where your credits go.\n")
    with open(os.path.join(out, "SOURCES.txt"), "w") as f:
        f.write("Every file and the VocalSet recording it was cut from (start-end in seconds):\n\n")
        f.write("\n".join(sources) + "\n")
    folder_tags = {}
    for singer in args.singers.split(","):
        label = singer_label(singer)
        folder_tags[label] = {"Sound": "Voice", "Kind": "Sustained note", "Singer": "Women" if label.startswith("Female") else "Men"}
    source = {
        "id": "sonora-seraph-voices",
        "name": "Seraph Voices",
        "version": args.version,
        "description": "Recorded singers for Sonora Seraph's Real voices: three women and three men holding A, O and U at several "
                       "pitches. From VocalSet (CC BY 4.0): music made with them needs the credit line.",
        "author": "VocalSet: " + AUTHORS + "; selection by Sonora contributors",
        "licence": "CC-BY-4.0",
        "attribution": {"title": TITLE, "authors": AUTHORS, "sourceUrl": SOURCE_URL, "licenceUrl": LICENCE_URL, "changes": CHANGES},
        "styles": ["Choir"],
        "types": ["Vocals"],
        "tags": ["Seraph", "Real voices"],
        "source": "A selection from VocalSet (" + SOURCE_URL + ") made by scripts/packs/make_seraph_voices.py: " + "; ".join(sources),
        "folderTags": folder_tags,
        "fileTags": file_tags,
    }
    with open(os.path.join(out, "pack-source.json"), "w") as f:
        json.dump(source, f, indent=2)
    total = sum(os.path.getsize(w) for w in kept)
    print("%d notes, %.1f MB in %s" % (len(kept), total / 1e6, out))
    return 0 if kept else 1


if __name__ == "__main__":
    sys.exit(main())

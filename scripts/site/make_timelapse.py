#!/usr/bin/env python3
"""Renders the website's "building a track" timelapse (COM-3): the demo set of make_demo_set.py built up stage by stage
(tracks appear, their clips fill in, devices drop into the chain), then played back. Every frame is Sonora's own UI drawn
by ui_snapshot; nothing is recorded from a screen.

Usage: python3 scripts/site/make_timelapse.py OUT_DIR [--snapshot build/tools/ui_snapshot] [--width 1280]
Writes OUT_DIR/timelapse.webp (animated, for the site) and OUT_DIR/timelapse.gif, plus the frames in OUT_DIR/frames/.
Needs Pillow; the GIF also needs ffmpeg.
"""
import argparse
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import make_demo_set  # noqa: E402

# Tracks in set order: (id, devices on it). Groups have no clips or devices.
ORDER = [(10, 0), (11, 1), (12, 0), (13, 0), (14, 0), (20, 0), (21, 3), (22, 0), (23, 1), (24, 3), (25, 1), (30, 0),
         (31, 0), (32, 0)]
GROUPS = {10, 20, 30}
BARS = 72


def storyboard():
    """(stage, selected track index or None, seconds on screen, play-at beat or None, Prism window) per frame."""
    frames = []
    devices = {tid: 0 for tid, _ in ORDER}

    def add(tracks, bars, select, seconds, play=None, returns=False, window=False):
        frames.append(({"tracks": tracks, "bars": dict(bars), "devices": dict(devices), "returns": returns}, select, seconds, play,
                       window))

    add(0, {}, None, 0.8)
    for index, (tid, count) in enumerate(ORDER):
        if tid in GROUPS:
            add(index + 1, {}, index, 0.25)
            continue
        sweep = (16, 32, 48, BARS) if tid == 11 else (36, BARS)
        for bar in sweep:
            add(index + 1, {tid: bar}, index, 0.18)
        for n in range(1, count + 1):
            devices[tid] = n
            add(index + 1, {}, index, 1.0 if tid == 23 else 0.7)
            if tid == 23:  # the Prism opens in its own window, like a plug-in
                add(index + 1, {}, index, 1.8, window=True)
    add(len(ORDER), {}, 9, 0.6, returns=True)
    for beat in range(0, BARS * 4 + 1, 16):  # play it back, showing the Lead's Echo, Delay and Reverb
        add(len(ORDER), {}, 9, 0.14, play=beat, returns=True)
    frames[-1] = (*frames[-1][:2], 2.5, *frames[-1][3:])
    return frames


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("out")
    parser.add_argument("--snapshot", default="build/tools/ui_snapshot")
    parser.add_argument("--width", type=int, default=1280)
    args = parser.parse_args()
    from PIL import Image

    out = pathlib.Path(args.out)
    (out / "frames").mkdir(parents=True, exist_ok=True)
    smp = make_demo_set.make_samples(out / "Samples")
    plan = storyboard()
    images, durations = [], []
    for i, (stage, select, seconds, play, window) in enumerate(plan):
        name = "Sonora Demo.xml"  # the status bar names the set
        make_demo_set.build(out, stage, name, smp)
        png = out / "frames" / f"{i:03}.png"
        cmd = [args.snapshot, str(png), str(out / name), "--scale", "2", "--size", "1440x900", "--no-browser",
               "--lane-height", "34", "--ppb", "4"]
        if select is not None:
            cmd += ["--track", str(select)]
        if play is not None:
            cmd += ["--play-at", str(play)]
        if window:
            cmd += ["--wavetable-window"]
        subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        frame = Image.open(png).convert("RGB")
        images.append(frame.resize((args.width, args.width * frame.height // frame.width), Image.LANCZOS))
        durations.append(int(seconds * 1000))
        print(f"frame {i + 1}/{len(plan)}", flush=True)

    webp = out / "timelapse.webp"
    images[0].save(webp, save_all=True, append_images=images[1:], duration=durations, loop=0, quality=72, method=6)
    concat = out / "frames.txt"
    with open(concat, "w") as f:
        for i, d in enumerate(durations):
            f.write(f"file 'frames/{i:03}.png'\nduration {d / 1000:.3f}\n")
        f.write(f"file 'frames/{len(durations) - 1:03}.png'\n")
    gif = out / "timelapse.gif"
    subprocess.run(["ffmpeg", "-y", "-loglevel", "error", "-f", "concat", "-safe", "0", "-i", str(concat), "-vf",
                    f"scale={args.width}:-1:flags=lanczos,split[a][b];[a]palettegen=max_colors=192:stats_mode=full[p];"
                    "[b][p]paletteuse=dither=sierra2_4a", "-loop", "0", str(gif)], check=True, cwd=out)
    print(webp, gif)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""UI-13: compare a Sonora snapshot with an equally framed reference (requires Pillow).

No automatic resizing or registration: those can hide geometry errors. Use ui_snapshot
--size/--scale/--zoom and optional --reference-crop/--candidate-crop to align the inputs.
"""
import argparse
import json
import io
from pathlib import Path

from PIL import Image, ImageChops, ImageDraw, ImageCms


def crop_box(value):
    try:
        box = tuple(int(part) for part in value.split(","))
        if len(box) != 4 or box[2] <= box[0] or box[3] <= box[1]:
            raise ValueError()
        return box
    except ValueError as error:
        raise argparse.ArgumentTypeError("Use left,top,right,bottom with positive area") from error


def load(path, crop):
    with Image.open(path) as source:
        # Native screenshots can carry the monitor's Display P3 profile. Compare in sRGB,
        # the colour space used by Sonora's snapshots, rather than comparing encoded RGBs.
        if source.info.get("icc_profile"):
            profile = ImageCms.ImageCmsProfile(io.BytesIO(source.info["icc_profile"]))
            image = ImageCms.profileToProfile(source, profile, ImageCms.createProfile("sRGB"), outputMode="RGB")
        else:
            image = source.convert("RGB")
    if crop:
        if crop[0] < 0 or crop[1] < 0 or crop[2] > image.width or crop[3] > image.height:
            raise ValueError(f"Crop is outside {path}: {image.size}")
        image = image.crop(crop)
    return image


def compare(reference, candidate, tolerance):
    difference = ImageChops.difference(reference, candidate)
    histogram = difference.histogram()
    pixels = reference.width * reference.height
    mean_error = sum((index % 256) * count for index, count in enumerate(histogram)) / (pixels * 3)
    red, green, blue = difference.split()
    maximum = ImageChops.lighter(ImageChops.lighter(red, green), blue)
    mask = maximum.point(lambda value: 255 if value > tolerance else 0)
    mismatches = mask.histogram()[255]
    return difference, mask, {
        "width": reference.width,
        "height": reference.height,
        "pixel_tolerance": tolerance,
        "mean_channel_error": mean_error,
        "max_channel_error": max(maximum.getextrema()),
        "mismatched_pixels": mismatches,
        "mismatch_fraction": mismatches / pixels,
        "mismatch_bounds": mask.getbbox(),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("candidate", type=Path)
    parser.add_argument("--out", type=Path, required=True, help="Directory for comparison.png, difference.png and metrics.json")
    parser.add_argument("--reference-crop", type=crop_box)
    parser.add_argument("--candidate-crop", type=crop_box)
    parser.add_argument("--tolerance", type=int, default=8, help="Allowed per-channel difference (0..255)")
    parser.add_argument("--max-mismatch", type=float, default=1.0, help="Maximum mismatching fraction (0..1); exceeding it exits 1")
    args = parser.parse_args()
    if not 0 <= args.tolerance <= 255 or not 0 <= args.max_mismatch <= 1:
        parser.error("Tolerance must be 0..255 and max-mismatch must be 0..1")
    try:
        reference = load(args.reference, args.reference_crop)
        candidate = load(args.candidate, args.candidate_crop)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    if reference.size != candidate.size:
        parser.error(f"Frame sizes differ: {reference.size} vs {candidate.size}; frame or crop explicitly")
    difference, mask, metrics = compare(reference, candidate, args.tolerance)
    metrics.update(reference=str(args.reference.resolve()), candidate=str(args.candidate.resolve()))
    args.out.mkdir(parents=True, exist_ok=True)
    difference.save(args.out / "difference.png")
    overlay = candidate.copy()
    overlay.paste((255, 64, 64), mask=mask.point(lambda value: value // 2))
    board = Image.new("RGB", (reference.width * 3, reference.height + 24), "#222222")
    drawing = ImageDraw.Draw(board)
    for index, (label, image) in enumerate((("Reference", reference), ("Sonora", candidate), ("Mismatch overlay", overlay))):
        drawing.text((index * reference.width + 6, 6), label, fill="white")
        board.paste(image, (index * reference.width, 24))
    board.save(args.out / "comparison.png")
    (args.out / "metrics.json").write_text(json.dumps(metrics, indent=2) + "\n")
    print(json.dumps(metrics, indent=2))
    return 1 if metrics["mismatch_fraction"] > args.max_mismatch else 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""UI-12: audit indexed picker swatches against Sonora's palette (Pillow required).

Coordinates refer to the original capture pixels. Uses the median of a 9x9 interior patch,
and converts tagged monitor RGB to sRGB. A JPEG screenshot requires a nonzero tolerance.
"""
import argparse
import json
import re
import statistics
from pathlib import Path

from compare_ui import load


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path)
    parser.add_argument("--palette", type=Path, default=Path(__file__).resolve().parents[1] / "src/core/LiveColors.cpp")
    parser.add_argument("--names", type=Path)
    parser.add_argument("--origin", default="31,779", help="First swatch centre: x,y")
    parser.add_argument("--step", default="34,34", help="Horizontal and vertical centre spacing")
    parser.add_argument("--tolerance", type=int, default=8)
    parser.add_argument("--out", type=Path, required=True)
    args = parser.parse_args()
    try:
        origin = tuple(int(v) for v in args.origin.split(","))
        step = tuple(int(v) for v in args.step.split(","))
        if len(origin) != 2 or len(step) != 2 or min(step) <= 0 or not 0 <= args.tolerance <= 255:
            raise ValueError("Invalid geometry or tolerance")
        source = args.palette.read_text().split("kPalette{", 1)[1].split("};", 1)[0]
        colors = re.findall(r"0x([A-Fa-f0-9]{6})\b", source)
        if len(colors) != 70:
            raise ValueError("Expected exactly 70 indexed palette entries")
        names = {item["index"]: item["name"] for item in json.loads(args.names.read_text())} if args.names else {}
        image = load(args.reference, None)
        results = []
        for index, color in enumerate(colors):
            x = origin[0] + index % 14 * step[0]
            y = origin[1] + index // 14 * step[1]
            if x - 4 < 0 or y - 4 < 0 or x + 4 >= image.width or y + 4 >= image.height:
                raise ValueError(f"Swatch {index} is outside the reference")
            pixels = [image.getpixel((x + dx, y + dy)) for dx in range(-4, 5) for dy in range(-4, 5)]
            observed = tuple(int(statistics.median(pixel[channel] for pixel in pixels)) for channel in range(3))
            delta = max(abs(a - b) for a, b in zip(bytes.fromhex(color), observed))
            results.append({"index": index, "name": names.get(index), "expected": color.upper(),
                            "observed_srgb": "%02X%02X%02X" % observed, "max_channel_error": delta,
                            "passed": delta <= args.tolerance})
    except (OSError, ValueError, IndexError, KeyError) as error:
        parser.error(str(error))
    report = {"reference": str(args.reference.resolve()), "origin": origin, "step": step,
              "tolerance": args.tolerance, "passed": sum(row["passed"] for row in results),
              "total": len(results), "max_channel_error": max(row["max_channel_error"] for row in results),
              "capture_limit": "Monitor ICC profile converted to sRGB; interior median limits JPEG noise, not an exact RGB proof.",
              "swatches": results}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(report, indent=2) + "\n")
    print(f"{report['passed']}/{report['total']} swatches within {args.tolerance}; max channel error {report['max_channel_error']}")
    return 0 if report["passed"] == report["total"] else 1


if __name__ == "__main__":
    raise SystemExit(main())

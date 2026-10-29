#!/usr/bin/env python3
"""COM-4o: point every download link at this release's versioned installers and show its version.

usage: scripts/release/set_version_links.py [<version>]     (default: project() in CMakeLists.txt)

Rewrites site/*.html and publish/public/README.md (the public README):
- links to releases/latest/download/<name> or releases/download/v<old>/<name> become releases/download/v<version>/<name>,
- installer names (stable "Sonora-macOS.dmg" or an older version's "Sonora-0.1.4.dmg") become this version's names, so
  the file a visitor saves says which version it is,
- <span data-version>...</span> (site) and "Latest version: **...**" (README) show <version>.
The release workflow also attaches the version-free copies, so old links keep working. check_release_ready.sh refuses
a tag whose site still names another version. Prints the files it changed.
"""
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
V = r"\d+\.\d+\.\d+"
# (pattern matching the stable or any versioned name, versioned name for a version)
NAMES = [
    (re.compile(rf"Sonora-(?:macOS|{V})\.dmg"), lambda v, m: f"Sonora-{v}.dmg"),
    (re.compile(rf"Sonora-(?:Windows-x64|{V}-windows-x64)-setup\.exe"), lambda v, m: f"Sonora-{v}-windows-x64-setup.exe"),
    (re.compile(rf"Sonora-(?:Linux|{V})-x86_64\.AppImage"), lambda v, m: f"Sonora-{v}-x86_64.AppImage"),
    (re.compile(rf"(?:Sonora-Linux-amd64|sonora_{V}_amd64)\.deb"), lambda v, m: f"sonora_{v}_amd64.deb"),
    (re.compile(rf"Sonora-([A-Za-z0-9-]+?)-VST3-(?:macOS|{V})\.pkg"), lambda v, m: f"Sonora-{m.group(1)}-VST3-{v}.pkg"),
]
RELEASE_PATH = re.compile(rf"(github\.com/lordillusions/sonora/releases/)(?:latest/download|download/v{V})/")
SITE_VERSION = re.compile(rf"(<span data-version>){V}(</span>)")
README_VERSION = re.compile(rf"(Latest version: \*\*){V}(\*\*)")


def rewrite(text, version):
    text = RELEASE_PATH.sub(lambda m: f"{m.group(1)}download/v{version}/", text)
    for pattern, render in NAMES:
        text = pattern.sub(lambda m: render(version, m), text)
    text = SITE_VERSION.sub(lambda m: f"{m.group(1)}{version}{m.group(2)}", text)
    return README_VERSION.sub(lambda m: f"{m.group(1)}{version}{m.group(2)}", text)


def main():
    if len(sys.argv) > 1:
        version = sys.argv[1].lstrip("v")
    else:
        cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        version = re.search(rf"^project\(Sonora VERSION ({V})", cmake, re.M).group(1)
    if not re.fullmatch(V, version):
        sys.exit(f"not a version: {version}")
    readme = ROOT / "publish/public/README.md"  # the public tree has it as README.md
    for path in sorted((ROOT / "site").glob("*.html")) + [readme if readme.exists() else ROOT / "README.md"]:
        before = path.read_text(encoding="utf-8")
        after = rewrite(before, version)
        if after != before:
            path.write_text(after, encoding="utf-8")
            print(f"updated {path.relative_to(ROOT)}")
    print(f"download links now name {version}")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""COM-4b: seed a fresh archives folder from the published feed, so a CI runner can write deltas.

usage: fetch_previous_updates.py <feed url> <archives dir> [--count N]

make_release.sh needs the current appcast.xml and the full zips of the latest releases next to the new zip:
generate_appcast diffs against them (deltas) and keeps their items. A CI runner starts empty, so this downloads the live
appcast and the full archives of its N newest items (each checked against the enclosure's length). No feed yet (404)
means the first release with updates: nothing to fetch, and the new appcast starts with one item.
"""
import argparse
import os
import sys
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET

SPARKLE = "{http://www.andymatuschak.org/xml-namespaces/sparkle}"


def fetch(url):
    request = urllib.request.Request(url, headers={"User-Agent": "sonora-release"})
    with urllib.request.urlopen(request, timeout=120) as response:
        return response.read()


def version_key(text):
    return [int(part) if part.isdigit() else part for part in text.replace("-", ".").split(".")]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("feed")
    parser.add_argument("archives")
    parser.add_argument("--count", type=int, default=3)
    args = parser.parse_args()

    os.makedirs(args.archives, exist_ok=True)
    try:
        feed = fetch(args.feed)
    except urllib.error.HTTPError as error:
        if error.code == 404:
            print("fetch_previous_updates: %s does not exist yet: first release with updates, no deltas" % args.feed)
            return
        raise
    with open(os.path.join(args.archives, "appcast.xml"), "wb") as f:
        f.write(feed)

    items = []
    for item in ET.fromstring(feed).iter("item"):
        version = item.findtext(SPARKLE + "version")
        enclosure = item.find("enclosure")  # the full archive; deltas sit under <sparkle:deltas>
        if version is None or enclosure is None:
            continue
        if item.findtext(SPARKLE + "channel"):
            continue  # beta items: deltas to a stable release come from stable releases
        items.append((version_key(version), version, enclosure.get("url"), int(enclosure.get("length", "0"))))
    items.sort(reverse=True)

    for _, version, url, length in items[: args.count]:
        path = os.path.join(args.archives, os.path.basename(url))
        data = fetch(url)
        if length and len(data) != length:
            sys.exit("fetch_previous_updates: %s is %d bytes, the appcast says %d" % (url, len(data), length))
        with open(path, "wb") as f:
            f.write(data)
        print("fetch_previous_updates: %s (%s, %d bytes)" % (os.path.basename(path), version, len(data)))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""COM-4: keep the download URL of every file an earlier appcast already published.

usage: pin_urls.py <appcast before generate_appcast> <appcast after>

generate_appcast rewrites all enclosure URLs it touches with the new --download-url-prefix. Sonora publishes each release
on its own GitHub Release (its own URL prefix), so a file that was published before must keep its original URL. Only the
url="..." attributes change; EdDSA signatures cover the files, not the URLs, so they stay valid.
"""
import os
import re
import sys

URL = re.compile(r'url="([^"]+)"')


def published(path):
    with open(path, encoding="utf-8") as f:
        return {os.path.basename(url): url for url in URL.findall(f.read())}


def main():
    before, after = sys.argv[1], sys.argv[2]
    known = published(before)
    with open(after, encoding="utf-8") as f:
        text = f.read()
    pinned = 0

    def pin(match):
        nonlocal pinned
        url = match.group(1)
        old = known.get(os.path.basename(url))
        if old is None or old == url:
            return match.group(0)
        pinned += 1
        return 'url="%s"' % old

    text = URL.sub(pin, text)
    with open(after, "w", encoding="utf-8") as f:
        f.write(text)
    print("pin_urls: kept the original URL of %d previously published file(s)" % pinned)


if __name__ == "__main__":
    main()

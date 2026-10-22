#!/usr/bin/env python3
"""Checks that every local link, image and #anchor in site/*.html points at something that exists.

External (http/https/mailto) links are not fetched. Exit code 1 lists the broken references.
Usage: python3 site/check_links.py
"""
import html.parser
import pathlib
import sys
import urllib.parse

SITE = pathlib.Path(__file__).resolve().parent


class Collector(html.parser.HTMLParser):
    def __init__(self):
        super().__init__()
        self.refs, self.ids = [], set()

    def handle_starttag(self, tag, attrs):
        a = dict(attrs)
        if "id" in a:
            self.ids.add(a["id"])
        for key in ("href", "src"):
            if a.get(key):
                self.refs.append(a[key])


def parse(page):
    c = Collector()
    c.feed(page.read_text(encoding="utf-8"))
    return c


def main():
    pages = {p.name: parse(p) for p in sorted(SITE.glob("*.html"))}
    broken = []
    for name, c in pages.items():
        for ref in c.refs:
            url = urllib.parse.urlparse(ref)
            if url.scheme in ("http", "https", "mailto"):
                continue
            target = url.path or name
            if not (SITE / target).exists():
                broken.append(f"{name}: missing file {ref}")
            elif url.fragment and target.endswith(".html") and url.fragment not in pages[target].ids:
                broken.append(f"{name}: missing anchor {ref}")
    for line in broken:
        print(line)
    print(f"{len(pages)} pages, {sum(len(c.refs) for c in pages.values())} references, {len(broken)} broken")
    return 1 if broken else 0


if __name__ == "__main__":
    sys.exit(main())

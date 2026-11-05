#!/usr/bin/env python3
"""LIB-1: serves a pack_tool build folder on 127.0.0.1 for local end-to-end tests (docs/SAMPLE_PACKS.md).

    python3 scripts/packs/serve_packs.py <folder> [--port 8765] [--throttle KBps] [--drop-after BYTES]

Supports "Range: bytes=N-" (206) like GitHub release assets, so resumed downloads can be checked. --throttle slows the
body down (watch the progress bar); --drop-after closes the connection after that many body bytes, once per archive.
Loopback only, development only: never point a release at it.
"""

import argparse
import http.server
import os
import re
import sys
import time


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("folder")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--throttle", type=int, default=0, help="KB per second (0 = full speed)")
    parser.add_argument("--drop-after", type=int, default=0, help="drop each archive's first response after N body bytes")
    args = parser.parse_args()
    root = os.path.realpath(args.folder)
    dropped = set()

    class Handler(http.server.BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def do_GET(self):
            name = self.path.split("?")[0].lstrip("/")
            path = os.path.realpath(os.path.join(root, name))
            if not path.startswith(root + os.sep) or not os.path.isfile(path):
                self.send_response(404)
                self.send_header("Content-Length", "0")
                self.send_header("Connection", "close")
                self.end_headers()
                return
            size = os.path.getsize(path)
            start = 0
            match = re.match(r"bytes=(\d+)-$", self.headers.get("Range", ""))
            if match:
                start = int(match.group(1))
                if start >= size:
                    self.send_response(416)
                    self.send_header("Content-Range", "bytes */%d" % size)
                    self.send_header("Content-Length", "0")
                    self.send_header("Connection", "close")
                    self.end_headers()
                    return
                self.send_response(206)
                self.send_header("Content-Range", "bytes %d-%d/%d" % (start, size - 1, size))
            else:
                self.send_response(200)
            self.send_header("Content-Length", str(size - start))
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Type", "application/zip" if name.endswith(".zip") else "application/octet-stream")
            self.send_header("Connection", "close")
            self.end_headers()
            limit = size - start
            if args.drop_after and name.endswith(".zip") and name not in dropped:
                dropped.add(name)
                limit = min(limit, args.drop_after)
            with open(path, "rb") as f:
                f.seek(start)
                sent = 0
                while sent < limit:
                    chunk = f.read(min(65536, limit - sent))
                    if not chunk:
                        break
                    try:
                        self.wfile.write(chunk)
                    except (BrokenPipeError, ConnectionResetError):
                        return
                    sent += len(chunk)
                    if args.throttle:
                        time.sleep(len(chunk) / (args.throttle * 1024.0))
            self.close_connection = True

        def log_message(self, fmt, *values):
            sys.stderr.write("[serve_packs] %s %s\n" % (self.headers.get("Range", "-"), fmt % values))

    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), Handler)
    print("serving %s at http://127.0.0.1:%d/packs.json" % (root, args.port), flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()

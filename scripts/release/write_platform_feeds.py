#!/usr/bin/env python3
"""COM-4l: sign the Windows and Linux installers of a release and write their update feeds.

usage: write_platform_feeds.py --version X.Y.Z --dist DIR --key-file FILE --public-key KEY [--repo OWNER/NAME]

Reads from DIR the files the installers workflow built:
    Sonora-X.Y.Z-windows-x64-setup.exe   ->  appcast-windows.xml (WinSparkle, src/app/UpdaterWinSparkle.cpp)
    Sonora-X.Y.Z-x86_64.AppImage         ->  appcast-linux.xml, item sparkle:os="linux-x64-appimage"
    sonora_X.Y.Z_amd64.deb               ->  appcast-linux.xml, item sparkle:os="linux-x64-deb" (src/app/UpdaterLinux.cpp)
and writes both feeds into DIR. Signatures are Sparkle's EdDSA (Ed25519 over the whole file) with the same key as the
macOS appcast: --key-file is the base64 32-byte seed `generate_keys -x` exports. Every signature is checked against
--public-key (packaging/macos/updates.env) before anything is written, so a secret that does not match the key compiled
into the apps fails the release. Needs the OpenSSL 3 command line tool (pkeyutl -rawin). Each feed lists only this
release: WinSparkle and the Linux updater install the newest item, with no deltas.
"""
import argparse
import base64
import email.utils
import html
import os
import subprocess
import sys
import tempfile

# DER prefixes of an Ed25519 PKCS#8 private key and SubjectPublicKeyInfo (RFC 8410), followed by the 32 raw bytes.
PRIVATE_PREFIX = bytes.fromhex("302e020100300506032b657004220420")
PUBLIC_PREFIX = bytes.fromhex("302a300506032b6570032100")
WINDOWS_ARGUMENTS = "/SILENT /SP- /SUPPRESSMSGBOXES /NOCANCEL /NORESTART /CLOSEAPPLICATIONS"


def openssl(*args):
    return subprocess.run(["openssl", *args], check=True, capture_output=True).stdout


def sign(path, private_der, public_der):
    signature = openssl("pkeyutl", "-sign", "-rawin", "-keyform", "DER", "-inkey", private_der, "-in", path)
    if len(signature) != 64:
        sys.exit("write_platform_feeds: openssl returned a %d-byte signature for %s" % (len(signature), path))
    with tempfile.NamedTemporaryFile(delete=False) as f:
        f.write(signature)
    try:
        subprocess.run(["openssl", "pkeyutl", "-verify", "-rawin", "-pubin", "-keyform", "DER", "-inkey", public_der,
                        "-in", path, "-sigfile", f.name], check=True, capture_output=True)
    except subprocess.CalledProcessError:
        sys.exit("write_platform_feeds: the signing key does not match --public-key (SPARKLE_ED_PRIVATE_KEY vs updates.env)")
    finally:
        os.unlink(f.name)
    return base64.b64encode(signature).decode()


def item(version, notes_url, url, length, signature, os_name, extra=""):
    return """    <item>
      <title>Sonora {v}</title>
      <pubDate>{date}</pubDate>
      <link>https://sonoradaw.com/download.html</link>
      <sparkle:version>{v}</sparkle:version>
      <sparkle:shortVersionString>{v}</sparkle:shortVersionString>
      <description><![CDATA[<p>Sonora {v}. <a href="{notes}">What is new in this version</a>.</p>]]></description>
      <enclosure url="{url}" length="{length}" type="application/octet-stream" sparkle:os="{os}"{extra}
                 sparkle:edSignature="{sig}"/>
    </item>
""".format(v=html.escape(version), date=email.utils.formatdate(usegmt=True), notes=html.escape(notes_url),
           url=html.escape(url), length=length, os=os_name, extra=extra, sig=signature)


def feed(items):
    return ('<?xml version="1.0" encoding="utf-8"?>\n'
            '<rss xmlns:sparkle="http://www.andymatuschak.org/xml-namespaces/sparkle" version="2.0">\n'
            "  <channel>\n    <title>Sonora</title>\n" + "".join(items) + "  </channel>\n</rss>\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--dist", required=True)
    parser.add_argument("--key-file", required=True)
    parser.add_argument("--public-key", required=True)
    parser.add_argument("--repo", default="lordillusions/sonora")
    args = parser.parse_args()

    with open(args.key_file) as f:
        seed = base64.b64decode(f.read().strip())
    public = base64.b64decode(args.public_key)
    if len(seed) != 32 or len(public) != 32:
        sys.exit("write_platform_feeds: the key file must hold a base64 32-byte Ed25519 seed (generate_keys -x)")

    v = args.version
    prefix = "https://github.com/%s/releases/download/v%s/" % (args.repo, v)
    notes = "https://github.com/%s/releases/tag/v%s" % (args.repo, v)
    files = {
        "windows": "Sonora-%s-windows-x64-setup.exe" % v,
        "appimage": "Sonora-%s-x86_64.AppImage" % v,
        "deb": "sonora_%s_amd64.deb" % v,
    }
    with tempfile.TemporaryDirectory() as tmp:
        private_der, public_der = os.path.join(tmp, "key.der"), os.path.join(tmp, "pub.der")
        fd = os.open(private_der, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
        with os.fdopen(fd, "wb") as f:
            f.write(PRIVATE_PREFIX + seed)
        with open(public_der, "wb") as f:
            f.write(PUBLIC_PREFIX + public)
        signed = {}
        for kind, name in files.items():
            path = os.path.join(args.dist, name)
            if not os.path.isfile(path):
                sys.exit("write_platform_feeds: %s is missing" % path)
            signed[kind] = (prefix + name, os.path.getsize(path), sign(path, private_der, public_der))
            print("write_platform_feeds: signed %s (%d bytes)" % (name, signed[kind][1]))

    windows = item(v, notes, *signed["windows"], "windows-x64",
                   '\n                 sparkle:installerArguments="%s"' % WINDOWS_ARGUMENTS)
    linux = [item(v, notes, *signed["appimage"], "linux-x64-appimage"), item(v, notes, *signed["deb"], "linux-x64-deb")]
    for name, text in (("appcast-windows.xml", feed([windows])), ("appcast-linux.xml", feed(linux))):
        with open(os.path.join(args.dist, name), "w", encoding="utf-8") as f:
            f.write(text)
        print("write_platform_feeds: wrote %s" % name)


if __name__ == "__main__":
    main()

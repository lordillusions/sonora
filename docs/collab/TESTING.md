# Testing collab sessions

How to try and verify shared sessions, from unit tests to two Macs. Protocol: [PEER_PROTOCOL.md](PEER_PROTOCOL.md),
relay: [PROTOCOL.md](PROTOCOL.md).

Collab networking needs the optional dependencies: run `scripts/bootstrap.sh` once (libdatachannel and OpenSSL), then
configure with `-DSONORA_WITH_COLLAB_NET=ON` if the build directory existed before. Without them the app shows "no collab
networking" in the Collaborate panel and `collab_probe` refuses to run.

## 1. Automated checks (no network)

```bash
./build/tests/sonora_tests "collab"
```

```bash
./build/tests/sonora_tests "model codec"
```

These cover the exact model codec, object diff/patch, the host sequencer and guest client (including a host-plus-two-guests
fuzz test that must always converge), the wire protocol against corrupt input, invites and the safety code, and whole
sessions over in-memory links (join, edits, per-person undo, presence, live values, transport, sample download, guest
upload).

```bash
npm test --prefix packages/collab-relay
```

The relay's own tests (run `npm ci --prefix packages/collab-relay` once).

## 2. Real network, one command

`collab_probe` runs a host and two guests in one process over real WebRTC: on the local network and through a relay it
starts itself in open mode.

```bash
./build/tools/collab_probe tests/fixtures/two_tracks.xml
```

Use a real set to exercise sample transfer (it doesn't load plugins, so any set is safe):

```bash
./build/tools/collab_probe "/path/to/Some Project/Some Set.als" --guests 2 --edits 60
```

It prints `PASS` when guests receive an identical set with every sample, random concurrent edits from everyone converge,
undo, presence and transport travel, and a wrong ticket, a wrong secret, a malformed message and a declined LAN request
are refused. `--lan` or `--spawn-relay` runs one mode; `--relay URL` uses a running relay. It works in a private data folder
("Sonora Collab Probe") and deletes it afterwards.

## 3. Two Sonoras on one Mac

`SONORA_DATA_FOLDER` starts a separate instance with its own settings and caches, and allows it to run next to another.
On one Mac, keep the connection on the loopback address (macOS asks for Local Network permission for LAN addresses,
which an unsigned development build may not have):

```bash
SONORA_DATA_FOLDER="Sonora Collab A" SONORA_COLLAB_LAN_ADDRESS=127.0.0.1 SONORA_COLLAB_BIND_ADDRESS=127.0.0.1 build/src/Sonora_artefacts/Debug/Sonora.app/Contents/MacOS/Sonora "/path/to/Set.als"
```

```bash
SONORA_DATA_FOLDER="Sonora Collab B" SONORA_COLLAB_BIND_ADDRESS=127.0.0.1 build/src/Sonora_artefacts/Debug/Sonora.app/Contents/MacOS/Sonora
```

Then:

1. In A: **File > Share Session on Local Network**. The Collaborate panel opens; click **Copy Invite Link**.
2. In B: **File > Join Session...** (the link is filled in from the clipboard), enter a name, **Join**.
3. B opens the shared set ("(shared session)" in the title). Both top bars show the people button with two avatars.

Things to try:

- Edit in either window (move or duplicate a clip, edit notes, rename or colour a track, insert a device, change tempo
  or loop): the other follows within a moment.
- Edit the same clip in both at once: one edit wins; the other person sees it undone with "... changed the same thing first".
- Cmd+Z undoes only your own last edit; it's refused when someone changed that object since.
- Move a fader or a device knob: the other side's control moves too.
- Press Play in either window: both start together; Stop stops both. In B's panel, turn "Shared transport" off to
  play alone.
- Move the pointer over the Arrangement, Session view or piano roll: the other window shows a coloured cursor with your
  name, your selected clip outlined and your time selection.
- In A's panel: make B a listener (B can no longer edit), remove B, compare the safety words.
- Drag a sample from Finder onto an audio track in B: it's uploaded to A's project and appears for both.
- In B: **File > Save a Copy** writes the set with all samples next to it.
- Quit B and start it again with the same command, then join again: the session continues (A keeps the set).

Remove the test folders afterwards: `~/Library/Application Support/Sonora Collab A` and `... B`.

### Scripted version

`SONORA_COLLAB_AUTOTEST` makes the two instances share, join, make one edit each and print a hash of the whole shared
state every two seconds. The hashes must match:

```bash
SONORA_COLLAB_AUTOTEST="host:/tmp/invite.txt" SONORA_DATA_FOLDER="Sonora Collab A" SONORA_COLLAB_LAN_ADDRESS=127.0.0.1 SONORA_COLLAB_BIND_ADDRESS=127.0.0.1 build/src/Sonora_artefacts/Debug/Sonora.app/Contents/MacOS/Sonora "/path/to/Set.als"
```

```bash
SONORA_COLLAB_AUTOTEST="join:/tmp/invite.txt" SONORA_DATA_FOLDER="Sonora Collab B" SONORA_COLLAB_BIND_ADDRESS=127.0.0.1 build/src/Sonora_artefacts/Debug/Sonora.app/Contents/MacOS/Sonora
```

Look for `[sonora] collab autotest: state tempo=111.000000 track0=Renamed by guest ... hash=...` in both outputs.

## 4. Two Macs on one network

Run Sonora normally on both. The first time a Mac shares or joins, macOS asks to allow Sonora to find devices on the local
network: allow it. The host listens on TCP 47100 (the macOS firewall may ask too). Share on the local network on one Mac;
on the other, either paste the invite link or open **File > Collaborate...** and use **Ask to Join** next to the host's
session under "On this network", then allow the request on the host.

## 5. Over the internet (relay)

A relay introduces the two Macs; edits and samples then go directly between them (or as ciphertext through TURN).

- A development relay on one Mac (both instances on that Mac, as in section 3; the app accepts plain `http` only for
  `127.0.0.1`):

```bash
SONORA_RELAY_MODE=open PORT=8787 node packages/collab-relay/relay.mjs
```

- Real use: a self-hosted relay behind TLS with coturn for TURN, as in `packages/collab-relay/README.md`
  (`SONORA_RELAY_MODE=selfhost`, an access key from `node admin.mjs add`).

In Sonora: **File > Collaborate...**, **Change...** next to "Relay", enter the relay address (`https://...`; plain `http`
only for `127.0.0.1`) and the access key if the relay needs one (kept in the Keychain). Then **File > Share Session through
Relay**, copy the invite and send it. Guests only need the link.

Without TURN, networks with strict NAT can fail to connect ("Could not connect to the host ... may need a TURN relay").

## Not covered yet

Voice and camera (COL-7/8), Sonora Cloud sign-in and billing screens (COL-10 app side), host hand-over and guest
"keep a copy" policy (COL-9), and a two-Mac test over a real WAN with TURN.

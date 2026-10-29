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

1. In A: **File > Share Session...**, choose **On my Wi-Fi**, **Start Sharing**; click **Copy Invite Link**.
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
on the other, either paste the invite link or open **File > Share Session...** and use **Ask to Join** next to the host's
session under "On this network", then allow the request on the host.

## 5. Over the internet (relay)

A relay introduces the two Macs; edits and samples then go directly between them (or as ciphertext through TURN).

- A development relay on one Mac (both instances on that Mac, as in section 3; the app accepts plain `http` only for
  `127.0.0.1`):

```bash
SONORA_RELAY_MODE=open PORT=8787 node packages/collab-relay/relay.mjs
```

- Real use: the relay + Caddy + coturn stack on one VM, `packages/collab-relay/deploy/gcp.sh create` (README "Deploy on
  one Google Cloud VM"), then `deploy/gcp.sh token "Name"` for the access key.

In Sonora: **File > Share Session...**, **Connection Settings...** at the bottom, enter the server address (`https://...`;
plain `http` only for `127.0.0.1`; empty = the built-in default) and the access key if the relay needs one (kept in the
Keychain). Then **Anywhere (internet)** (the default) and **Start Sharing**: the invite link is copied to the clipboard
once the relay answers. A saved `http://127.0.0.1:8787` from before COL-3b is forgotten once at startup.
Guests only need the link.

Without TURN, the app asks a public STUN server (`stun.l.google.com`) for its internet address; networks with strict NAT
can still fail to connect ("Could not connect to the host ... may need a TURN relay").

Against a deployed relay from this Mac (no app): `collab_probe tests/fixtures/two_tracks.xml --relay https://<relay>
--token <key>` (the project's relay: `https://relay.sonoradaw.com`). Add `SONORA_COLLAB_ICE_POLICY=relay` to force every
link through TURN (proves coturn works; `relay-host`/`relay-guest` force one side only). Budgets scale with the path: the
probe times the relay's `/v1/health` and, above 60 ms, allows two minutes plus the set's samples and .als at 256 KiB/s per
guest for the join; after the join, steps get extra time per peer round trip. `SONORA_COLLAB_PROBE_JOIN_MS` still
overrides the join budget. The relay keeps a host's session 10 minutes after the host disconnects and an account hosts
at most 3: after killed runs, `gcp.sh ssh` then `cd /opt/sonora-relay/deploy && sudo docker compose restart relay`.

### Data-channel throughput (COL-3c)

`collab_probe --bench MB [--backlog KiB] [--lan | --relay URL --token KEY | --spawn-relay]` opens one raw host/guest link
(no session) and pushes MB in 64 KiB messages on Assets (at most `--backlog` queued in the data channel, default 1024;
the session uses 64), pinging over Ops every 250 ms. It prints MB/s, the idle and loaded Ops round trips, the host -> guest
delay of each ping, and usrsctp's counters (DATA chunks, retransmissions, T3 timeouts). With
`SONORA_COLLAB_ICE_POLICY=relay` against the deployed relay every byte goes Mac -> Virginia -> Mac (~370 ms round trip).
Tuning switches: `SONORA_COLLAB_SCTP="recv=KiB,send=KiB,cwnd=MTUs,burst=MTUs,cc=0..3,rtomin=ms,rtomax=ms,rtoinit=ms,sack=ms"`
(`rtc::SetSctpSettings`) and `SONORA_COLLAB_RTC_LOG=warning|info|debug|verbose`. Runs vary a lot (same settings 0.3 to
1.3 MB/s minutes apart); compare settings over several runs each. Results are in HANDOFF (COL-3c).

### Self-hosted relay stack, locally

The `deploy/docker-compose.yml` stack runs under Docker Desktop with an override (coturn on bridge ports instead of
host networking, relay port published for the probe, a private data folder):

```yaml
# override.yml
services:
  relay: { ports: ["8787:8787"], volumes: !override ["/tmp/relay-data:/data"] }
  caddy: { ports: !override ["8443:443"] }
  coturn: { network_mode: bridge, ports: ["3478:3478/udp", "3478:3478/tcp", "49160-49200:49160-49200/udp"] }
```

`.env`: `SONORA_RELAY_DOMAIN=localhost`, `SONORA_PUBLIC_IP=127.0.0.1`, `SONORA_PRIVATE_IP=<coturn container IP, usually
172.17.0.2>`, a random `SONORA_TURN_SECRET`. Then `docker compose --env-file .env -f docker-compose.yml -f override.yml up -d
--build`, `... run --rm -T relay node admin.mjs add --plan selfhost --name Probe`, and the probe with `--relay
http://127.0.0.1:8787 --token <key>`. Each account hosts at most 3 sessions and the probe does not always end its own:
`restart relay` between runs. `curl -k https://localhost:8443/v1/health` checks Caddy.

## Not covered yet

Voice and camera (COL-7/8), Sonora Cloud sign-in and billing screens (COL-10 app side), host hand-over and guest
"keep a copy" policy (COL-9), and a two-Mac test over a real WAN with TURN on the deployed VM (COL-3b).

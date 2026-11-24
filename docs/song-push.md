# Library push: new and edited songs reach the laptops at once

Built 2026-11-23 (branch `song-push`, on top of the network bridge). When a
library release is published, the changed songs and songbooks reach every
connected church laptop within seconds and go straight into its local
database. Songs, Bibles and everything else still live on the laptop, so
projection works with the internet down; a laptop that was offline catches up
when it reconnects. New versions of the program itself travel the same way
(see "App updates").

This is the "tier-3 live push" from AGENTS.md. Three recorded decisions shape
it: the projector app never talks to the internet (a companion program does,
CGF Lyrics `docs/plans/2026-08-09-live-platform-architecture.md`); everything
in the cloud lives in the CGF Vernon Cloudflare account; and the snapshot
stays the source of truth (a push only brings it forward sooner).

## How a release travels

1. Florian runs `publish_snapshot.py` (CGF Lyrics) as before. It writes
   snapshot v*N* and then calls `tools/push_release.py push`.
2. `push_release.py` compares v*N* with v*N-1* and lists the songs and
   songbooks that were added, changed or removed. It signs the list with the
   publisher key on Florian's PC and posts it to the push server.
3. The push server (`push-server/`, a Cloudflare Worker with one Durable
   Object per church) stores the release and sends it to every connected
   laptop.
4. On the laptop, the companion (`cgf-companion.exe`, started by the app)
   holds that connection and hands the release to the app over the network
   bridge on the same computer.
5. The app checks the signature, writes the songs into `spData.sqlite` in one
   transaction and refreshes the song list, the preview and the schedule.
   The companion acknowledges the new version to the server.

```
publish_snapshot.py ─► push_release.py ─► push server ─► cgf-companion ─► bridge ─► app + spData.sqlite
  (Florian's PC)          (signs)        (Cloudflare)    (laptop)       (127.0.0.1)
```

## Rules the code keeps

- **The screen is never changed by an update.** The song on the projector is
  its own copy (`current_song`). An edited song reaches the screen the next
  time someone puts it live. The song list, the preview (on the same verse)
  and the schedule's copies update at once.
- **Only the publisher can change songs.** Each part is signed (ECDSA P-256,
  SHA-256). The app checks it with Windows' own crypto (bcrypt) against the
  key in `cgf-cloud.json`. The server and the companion only relay; a
  changed payload is refused.
- **The laptop's own data stays.** Song ids above 500000 (quick-adds) are never
  touched, and a release that tries is refused whole. Usage counts (`count`,
  `date`) stay local.
- **All or nothing.** A release is applied in one transaction, after all its
  parts have arrived, or not at all.
- **Versions build on each other.** A release goes from v*N-1* to v*N*. The
  database's version is the higher of `version.txt` (the snapshot the sync
  agent installed) and the `CgfSync` table (the last release applied). A new
  snapshot replaces the whole database, `CgfSync` included.
- **Offline laptops catch up.** On reconnecting, the companion says which
  version the laptop has; the server sends every release since. It keeps the
  last 200. A laptop further behind is told to wait for the snapshot, which
  the sync agent installs at the next restart.
- **Bibles and other tables come with the snapshot.** A release carries only
  songs and songbooks. When anything else changed, the release says so
  (`other_changes`) and the next snapshot install brings it.
- The bridge accepts releases only from programs on the same computer.

## App updates

Built 2026-11-23, so the church laptop gets new versions of the program
itself without a visit. The same connection carries them.

On the laptop (from the install package) the program and the data live apart:

```
%USERPROFILE%\CGFProjector\
  CGFProjector.exe        the launcher (desktop shortcut, and --companion at sign-in)
  cgf-versions.txt        current=app-0.2.1, previous=app-0.2.0, seq, trial, starts, bad
  cgf-healthy.txt         the last version that ran a full minute
  app-0.2.1\              one version: the app, the companion, Qt; cgf-release.json (notes)
  app-0.2.0\              the previous version, kept as the fallback
  spData.sqlite, version.txt, cgf-cloud.json, CGFProjector.log, cgf-companion.log, cgf-launcher.log
```

1. Florian builds and deploys as usual (BUILD.md), then runs
   `python tools/push_release.py app-release --build-dir src/win32_build/bin --version 0.2.1 --notes "..." --launcher src/win32_build/launcher/CGFProjector.exe`.
   It uploads only files the server does not have yet and signs the list of
   every file of the version.
2. The server sends the list to every connected laptop, and to each laptop
   again when it connects.
3. The companion checks the signature and builds `app-0.2.1.partial`: files
   that are the same as in the current version (same hash) are copied, the
   rest downloaded and checked against their hash. Then the folder gets its
   name, `cgf-versions.txt` switches to it, and older folders are deleted
   (one still running stays until next time). The running app is not touched.
4. The next start opens the new version. If the app is open, the status bar
   shows "CGF Projector 0.2.1 is ready" with What's new and Restart now; the
   button is disabled while anything is on the projector. Nobody has to click
   it: the next start opens the new version anyway.

Because the companion starts at Windows sign-in and keeps running, an update
published during the week is usually installed before anyone opens the app
on Sunday.

**Going back by itself.** A new version is on trial until it has run a full
minute (or closed normally); it then writes `cgf-healthy.txt`. If it stops
with an error in its first 20 seconds, the launcher starts the previous
version at once; if it never runs a full minute in three starts, the fourth
start goes back. The failed version is listed under `bad` and is never
installed again; the next release replaces it. `cgf-launcher.log` says what
happened.

The launcher itself is plain Win32 and linked statically, so it starts even
if a version's Qt libraries are broken. A release can replace it too (it is
not running between starts). A broken companion cannot repair itself: test
companion changes with `tests/update` before releasing them.

## Install package

`python tools/make_install.py --version 0.2.0 --snapshot <spData-cgf-vN.sqlite> --device-file <cgf-cloud.json> --seq <N> --out <folder>`
writes `CGFProjector-0.2.0.zip`: `install.cmd`, `install.ps1` and the
`CGFProjector` folder above. `--seq` is the version's app release number on
the server, so the laptop does not download it again. The zip holds the
laptop's device token: keep it private, and remove the device if it goes
astray.

On the laptop: unzip, double-click `install.cmd`. It installs into
`%USERPROFILE%\CGFProjector` for the signed-in Windows user, no admin rights
needed; adds desktop and Start menu shortcuts and a Startup entry for the
companion; and starts the companion. One step asks for admin rights: a
Windows Firewall rule for the network bridge's port, so Windows does not ask
again after every update (`allow-network.cmd` does the same later;
docs/network-bridge.md). Installing again keeps the song
library, settings and device token, and adds the new version beside the old
one. SoftProjector 2.1 can stay installed: it keeps its own database and
settings.

## Files

| Where | What |
|---|---|
| `push-server/` | The Worker and Durable Object (`src/index.ts`, `src/release.ts`), tests in `test/` |
| `tools/push_release.py` | Publisher side: keys, devices, deltas, signing, push; tests in `tools/test_push_release.py` |
| `src/companion/` | `cgf-companion.exe` (Qt, runs without a window; its own `.pro`) |
| `src/sources/librarypush.cpp` | Checks signatures, buffers parts, applies releases to the database |
| `src/sources/softprojectorsync.cpp` | Main window side: starts the companion, refreshes the lists |
| `bridgeserver.cpp`, `livebridge.cpp` | `release` messages from local programs, the library version in `hello` |
| `tests/push/` | App checks and the end-to-end run |
| `src/launcher/` | The launcher (`CGFProjector.exe` in the install folder) |
| `src/sources/softprojectorupdate.cpp` | Healthy mark, the update bar, Help, What's new |
| `src/sources/cgfsignature.cpp` | The signature check, shared by the app and the companion |
| `tools/make_install.py`, `tools/install/` | The install package and its installer |
| `tests/update/` | App updates end to end |

## Settings

On Florian's PC, `%USERPROFILE%\.cgf-publish\` (outside every repository,
readable by his Windows user only): `push.json` (server address, church,
where `push_release.py` is), `signing-key.pem`, `publish-token.txt`.

On each laptop, `cgf-cloud.json` beside `spData.sqlite`, written by
`push_release.py add-device`: the server address, the church, the device name
and token, and the publisher's public key. It is git-ignored; never commit
one. Without it the app works as before and refuses pushed releases.

The companion logs to `cgf-companion.log` beside it (kept under 512 KB, one
old copy).

## Going live

Steps 1 to 3 were done on 2026-11-23 with Florian's go-ahead. The server with
app releases was redeployed the same evening (version
`4a15c42e-b7b6-43a8-8443-a2275f89f228`). App releases on it: 1 = app-0.2.0,
2 = app-0.2.1 (0.2.0 with the companion fix from the first live run; the
church laptop starts on 0.2.1). A live run against push.cgfvernon.ca with a
temporary device (removed afterwards) checked the companion's `wss://`
connection, a full download of 0.2.0 (107 files in 11 s), the update to 0.2.1
(2 files downloaded, 105 copied) and the restart on the downloaded
companion. The church laptop is registered as "Church laptop"; its package,
`CGFProjector-0.2.1.zip` with the July library (v3), is in
`C:\Users\Disterhoft\CGF\install-packages\` with its `cgf-cloud.json`, both
readable by Florian's Windows user only. The next library publish brings
the laptop's songs up to date.

1. Done: `push-server/` is deployed to the CGF Vernon Cloudflare account
   (the one with the `cgfvernon.ca` zone) as Worker `cgf-projector-push` at
   **https://push.cgfvernon.ca** (custom domain; first version
   `4f8c7fe7-54a4-41f5-b43b-657d9bc44f53`). Redeploy with
   `npx wrangler deploy` in `push-server/` after `npm test`. SQLite Durable
   Objects run on the free plan, and idle connections cost nothing while
   hibernating.
2. Done: `push_release.py init --server https://push.cgfvernon.ca` made the
   publisher key (key id `e2ae808c1b52746a`) and token in
   `%USERPROFILE%\.cgf-publish\`, and
   `push_release.py server-secret --wrangler-dir push-server` set the
   server's `PUBLISH_TOKEN` without showing it. Running `init` again keeps
   both. A new key would need every laptop's `cgf-cloud.json` remade.
3. Per laptop: `python tools/push_release.py add-device "Church laptop" --out <file>`,
   then build the install package with that file (above).
4. Publish as usual. `python tools/push_release.py status` shows which laptops
   are connected and which version each has.

## Protocol

### Release part (signed)

The payload is a JSON string; the signature covers its UTF-8 bytes exactly.

```jsonc
{"type": "release", "payload": "<JSON text>", "sig": "<base64 r||s, 64 bytes>", "key": "<key id>"}

// payload
{"kind": "cgf-release", "v": 1, "church": "cgf-vernon", "from": 3, "to": 4,
 "part": 1, "parts": 2, "published_at": "...", "snapshot": "spData-cgf-v4.sqlite",
 "snapshot_sha256": "...", "other_changes": false,
 "summary": {"songs_changed": 2, "songs_added": 1, "songs_deleted": 1, "songbooks_changed": 0},
 "songbooks": [{"id": 1, "name": "...", "info": ""}], "songbooks_deleted": [],
 "songs": [{"id": 7, "title": "...", "song_text": "...", "background": {"$b64": "..."}, ...}],
 "songs_deleted": [12]}
```

Songs rows carry the `Songs` columns by name; BLOBs travel as `{"$b64": ...}`.
Songbooks and deletions ride in part 1. A part stays under about 900 KB.

### Push server

| Request | Who | What |
|---|---|---|
| `GET /v1/<church>/ws` | laptop (device token) | WebSocket |
| `POST /v1/<church>/releases` | publisher token | `{"parts": [...]}`, whole releases only |
| `POST /v1/<church>/devices` | publisher token | `{"name"}` → `{"name", "token"}` (shown once) |
| `DELETE /v1/<church>/devices/<name>` | publisher token | removes it and closes its connection |
| `GET /v1/<church>/status` | publisher token | latest version, app release, laptops, their versions and app folders |
| `POST /v1/<church>/app/files/missing` | publisher token | `{"sha256": [...]}` → the ones not stored |
| `PUT /v1/<church>/app/files/<sha256>` | publisher token | a file as `qCompress` writes it (4-byte size + zlib), checked against its hash |
| `GET /v1/<church>/app/files/<sha256>` | laptop or publisher | the stored file |
| `POST /v1/<church>/app/releases` | publisher token | `{"release": <signed app release>}`; seq must rise, every file uploaded |

Tokens go in `Authorization: Bearer`, never in the address. The server stores
only a hash of each device token. Over the WebSocket:

```jsonc
// laptop → server
{"type": "hello", "version": 3, "app": "app-0.2.0"}  // songs after v3; no version: the app is closed
{"type": "ack", "version": 4}       // the app now has v4
"ping"                              // answered "pong" without waking the object
// server → laptop
{"type": "welcome", "device": "...", "latest": 4}
{"type": "release", ...}            // replayed after hello, or live after a publish
{"type": "resync", "have": 3, "latest": 9}   // too far behind: wait for the snapshot
{"type": "app", "payload": "...", "sig": "...", "key": "..."}  // the latest app release
// laptop → server, after installing one
{"type": "status", "app": "app-0.2.1"}
```

An app release payload: `{"kind": "cgf-app", "v": 1, "church", "seq",
"version", "folder": "app-<version>", "notes", "published_at", "files":
[{"path": "platforms/qwindows.dll", "sha256", "bytes"}], "launcher": {"sha256",
"bytes"}}`. The server keeps the files of the last three releases.

### Bridge (companion ↔ app)

`hello` carries `"library": {"version": N}` (-1 until the main window has
reported it) and a `{"type": "library", "version": N}` message follows each
change. A program on the same computer sends release parts as they came from
the server; the app answers that program only:

```jsonc
{"type": "releaseResult", "to": 4, "status": "applied", "version": 4}
// status: applied | duplicate | waiting (more parts to come) | gap (ask the
// server again from "version") | rejected (+ "reason")
```

`hello` also carries `"app": {"folder", "version"}`, the folder the app runs
from. When the companion has installed a newer one it sends
`{"type": "appUpdate", "folder", "version", "notes"}`, accepted from this
computer only.

## Tests

```
cd push-server && npm install && npm test                 # server, 18 tests
python tools/test_push_release.py                         # publisher, 11 tests
python tests/push/run_tests.py --db <snapshot>            # app, 30 checks
python tests/push/run_tests.py --db <snapshot> --e2e      # + wrangler dev and the companion
python tests/update/run_tests.py --db <snapshot>          # app updates, 23 checks
```

`<snapshot>` is a published snapshot such as CGF Lyrics'
`library/exports/publish/spData-cgf-v3.sqlite`; the tests copy it and add
made-up edits. The end-to-end run starts `wrangler dev` on port 8788, sets up
a throwaway publisher in `tests/push/out/`, registers a device, starts the
app offscreen (the companion starts with it) and publishes v4.

`tests/update` installs a package into `tests/update/out/laptop` against
`wrangler dev` on port 8789, runs the companion as it runs from sign-in and
publishes four versions: one new file (only that is downloaded), the app
started through the launcher (healthy mark, a song push in this layout), an
update while the app is open (the restart offer), and a version that
crashes at once (the launcher goes back by itself). The test versions carry
Qt's offscreen platform plugin so the app can run without a screen; the real
package carries only the Windows one.

## Not covered yet

- Laptops outside Florian's network need the snapshots hosted too (the sync
  agent's `source`), for a first install and for catching up after a long
  time offline.
- A schedule file saved before an update keeps the old song text inside it
  until it is saved again.
- A version that hangs at start (no window, no exit) is only caught by the
  three-starts rule.
- Not yet tested on the church laptop or its network.

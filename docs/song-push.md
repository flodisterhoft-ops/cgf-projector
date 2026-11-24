# Library push: new and edited songs reach the laptops at once

Built 2026-11-23 (branch `song-push`, on top of the network bridge). When a
library release is published, the changed songs and songbooks reach every
connected church laptop within seconds and go straight into its local
database. Songs, Bibles and everything else still live on the laptop, so
projection works with the internet down; a laptop that was offline catches up
when it reconnects.

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

Steps 1 and 2 were done on 2026-11-23 with Florian's go-ahead.

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
3. Per laptop: `python tools/push_release.py add-device "Booth laptop" --out <file>`,
   then copy that file as `cgf-cloud.json` next to `CGFProjector.exe`, with
   `cgf-companion.exe` from the same build.
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
| `GET /v1/<church>/status` | publisher token | latest version, laptops, their versions |

Tokens go in `Authorization: Bearer`, never in the address. The server stores
only a hash of each device token. Over the WebSocket:

```jsonc
// laptop → server
{"type": "hello", "version": 3}     // send me every release after v3
{"type": "ack", "version": 4}       // the app now has v4
"ping"                              // answered "pong" without waking the object
// server → laptop
{"type": "welcome", "device": "...", "latest": 4}
{"type": "release", ...}            // replayed after hello, or live after a publish
{"type": "resync", "have": 3, "latest": 9}   // too far behind: wait for the snapshot
```

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

## Tests

```
cd push-server && npm install && npm test                 # server, 14 tests
python tools/test_push_release.py                         # publisher, 8 tests
python tests/push/run_tests.py --db <snapshot>            # app, 30 checks
python tests/push/run_tests.py --db <snapshot> --e2e      # + wrangler dev and the companion
```

`<snapshot>` is a published snapshot such as CGF Lyrics'
`library/exports/publish/spData-cgf-v3.sqlite`; the tests copy it and add
made-up edits. The end-to-end run starts `wrangler dev` on port 8788, sets up
a throwaway publisher in `tests/push/out/`, registers a device, starts the
app offscreen (the companion starts with it) and publishes v4.

## Not covered yet

- Laptops outside Florian's network need the snapshots hosted too (the sync
  agent's `source`), for a first install and for catching up after a long
  time offline.
- App updates (a new `CGFProjector.exe`) can later use the same connection.
- A schedule file saved before an update keeps the old song text inside it
  until it is saved again.
- Not yet tested on the church laptop or its network.

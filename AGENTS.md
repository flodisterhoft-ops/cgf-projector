# CGF Projector — project master brief

Read this first. It is the authoritative context for all agents and chats
working on the CGF Vernon projection/worship platform. Paths re-verified
2026-10-05.

## What this is

**CGF Projector** is CGF Vernon's fork of [SoftProjector](https://github.com/SoftProjector/softprojector)
(GPLv3, attribution kept — see README and LICENSE). The church's operators have
used SoftProjector for ~10-15 years; this fork keeps their muscle memory while
stripping what they never used and wiring the app into a central CGF library.

- Fork repo: https://github.com/flodisterhoft-ops/cgf-projector (public)
- GitHub account: `flodisterhoft-ops`
- Branches: `master` = upstream Qt6 + build fix + guard rails + the
  `cgf-transform` transformation (merged 2026-07-26, 048a016).
- Working clone: `C:\Users\Disterhoft\Documents\AI Projects\CGF Vernon\CGF Projector`
  (this folder). The older space-free clone at `C:\Users\Disterhoft\CGF\cgf-projector`
  is one commit behind and not the working copy.
- User-facing install: `C:\Users\Disterhoft\CGF\CGFProjector\CGFProjector.exe`
  (refresh via robocopy after builds).
- Build: see **BUILD.md** (Qt 6.8.3 + MinGW 13.1 via aqtinstall; ~5 min from a
  clean machine). Installed on this machine at `C:\Users\Disterhoft\Qt`
  (checked 2026-10-31); a full build takes about 80 s.
- Status and history: `git log` here, plus the CGF Lyrics exporter/publisher tools
  below. The 2026-07-26 status snapshot is in
  `archive/agent-setup-2026-10-05/AGENTS-status-snapshot-2026-07-26.md`.

## The two-track strategy (decided 2026-07-26)

1. **Track 1 — this fork**: transition vehicle for the *current* rented
   building (fragile HDMI/CAT5e splitter chain) and the current operators.
   Simplify, rebrand, then make it a self-syncing offline client.
2. **Track 2 — the web platform** (the real long-term system): service planner,
   musician chord views, phone remote, OBS overlay, congregation view. In the
   new church building (~2 years out), projection itself likely becomes a
   fullscreen browser page per projector; the fork then retires or becomes the
   offline fallback.

## Architecture decisions (locked; challenge only with new evidence)

- **"No local database" means no visible database.** The app keeps an
  invisible, self-managing local cache, auto-synced from the central server.
  Operators never load files (no .spsc handling, no imports, no backups).
  Projection must keep working with the internet down; this is non-negotiable.
- **Library sync = snapshot down / proposals up.** Full published snapshots
  down (corpus is ~40 MB — no delta protocols as source of truth); local
  quick-adds upload as *proposals* into the master library's review queue.
  Never bidirectional merge.
- **Tier-3 live push (user decision 2026-07-26, replaces the freeze window):**
  per-song update pushes ride the WebSocket bridge and apply at any time,
  including mid-service — the app itself writes its local DB and refreshes all
  non-live UI (search/preview/schedule) instantly. The one guard: the
  currently projected song's on-screen copy is never changed implicitly.
  No operator action is ever required (user: "the guys are lazy, they'll
  never press it"): queued/schedule/preview songs update silently; the live
  song auto-applies the moment it leaves the screen; a reload nudge exists
  only as an optional shortcut. Projection renders from an in-memory
  copy, so DB writes can never disturb the screen by themselves. Push =
  best-effort freshness; the launch-time snapshot swap = guaranteed
  convergence (missed pushes self-heal). "Instant" means instant after
  publish, never per keystroke.
- **Real-time sync is for service plans only** (tiny data, WebSockets fine),
  never for the library.
- **The one worthwhile deep fork feature: a local WebSocket bridge** inside the
  app — broadcasts current on-screen state, accepts search/goto/next/prev.
  That single feature unlocks both phone remote control and the OBS livestream
  overlay (browser source, ~50% black background, auto-live whenever the desk
  shows something, manual hide for stream operator; replaces today's
  capture-card + luma-key setup).
- **Publish = explicit releases with version stamps**, not live master state.
  The curated master DB changes many times a day during curation; projectors
  consume releases only.
- **Multi-church later** (20-30 similar Russian-German churches): put
  `church_id` on all new server-side tables from day one. Keep NKJV /
  Schlachter 2000 Bible modules out of anything shared until rights are
  cleared (redistribution risk). Song lyrics: mostly old hymnody, lower risk.
- **English-only UI, trilingual content** (ru/de/en lives in the database).
- QSettings identifiers still say SoftProjector (deliberate, migration-free);
  revisit only with the sync work.

## Related projects on this machine

- **Master lyrics library** (the content source of truth):
  `C:\Users\Disterhoft\Documents\AI Projects\CGF Vernon\CGF Lyrics\`
  → read its `AGENTS.md` for current numbers and rules. As of 2026-10-01 the
  SQLite master holds 10,867 live songs, 20,694 live lyric versions and 58
  books/collections, with a review queue, a preserve-originals rule and the
  `verify_db.py --strict-current` gate. `songbook_entries.sp_song_id` maps all
  8,386 production SoftProjector rows.
- **Exporter and publisher** (in CGF Lyrics): `library/tools/export_spdata.py`
  (read-only on master; output `library/exports/spData-cgf.sqlite`) and
  `library/tools/publish_snapshot.py` (versioned immutable snapshots).
  `library/exports/sp_export_idmap.json` is append-only and is the id contract
  with every deployed client — never regenerate it.
- **Download agent** (this repo): `agent/cgf-sync-agent.ps1` and
  `agent/install-agent.ps1`. Registering it as a scheduled task and putting
  sqlite3.exe on clients need the user's approval.
- **Production SoftProjector 2.1** (the church's live system — do not touch):
  booth PC install `C:\Users\CGF Projector\SoftProjector`; a copy lives at
  `C:\Users\Disterhoft\OneDrive\CGF Vernon\SoftProjector Files\SoftProjector\`
  with the live `spData.sqlite` (8,386 songs, 4 Bibles, 124,425 verses,
  usage counters: 1,258 songs used, 5,798 total uses). Known hazard: the live DB
  is inside OneDrive; the plan is to move it local and keep OneDrive for
  backups only.
- **Offline songbook site**: `CGF Lyrics/cgf-songbook/` (active design);
  `CGF Lyrics/prototypes/lyrics-site/` is the older Next.js prototype.
- Smoke-test DB: a copy of the church DB is used for all testing; the
  original is never opened for writing by this project.

## Feature decisions from the operators' actual data

- Removed: HTTP module downloader (insecure, upstream-disabled), UI
  translations, Donate/dead links, forced dark theme (upstream bug — checked a
  registry value that never exists; now forced light).
- **Restored 2026-10-08 (user decision): Media and Pictures/slideshows.** They
  were cut on 2026-07-26 for having almost no use (0 media rows, 1 slide), but
  the operators avoided them because they were broken. The Qt 6 port had left
  them silent, black and freezing; the restore fixed and tested them (see
  BUILD.md "Restore log"). The sync agent now carries local Media, SlideShows
  and Slides rows into each new snapshot.
- Kept: Bible (F6) / Songs (F7) / Pictures / Media / Announcements (F8),
  schedule, song editor (future quick-add-that-uploads), **printing (user
  decision)**, usage counter (feeds analytics; counts should merge across
  machines via sync later).
- **Added 2026-10-31: PowerPoint/PDF import** in the Pictures tab. Each slide
  becomes a picture in a saved slide show, so operators step through it like
  a song (BUILD.md, "PowerPoint and PDF import"). PowerPoint draws the
  slides when the computer has it; otherwise LibreOffice, which the
  companion downloads into the data folder when the operator agrees (user
  decision 2026-11-27; BUILD.md, "LibreOffice for PowerPoint files").
- **Added 2026-11-27: rotate buttons** in the Pictures tab and the slide show
  editor, and photos load the right way up (camera rotation tag). See
  BUILD.md, "Rotating pictures"; `tests/pictures`.
- **What's new window (user decision 2026-11-28, app 0.2.9):** at the first
  start after the program or the library changed, one window lists the
  changes with icons and tags (New, Changed, Fixed, Removed); batches show
  counts, not song lists. It replaces the "Restart now" bar. Program items
  live in `src/whats-new.json`; add one per release before publishing
  (docs/whats-new.md).
- Planned add: **Share button** — song/setlist as link or PDF, possibly via
  Telegram (operators use Telegram).
- **Library from the cloud only (user decision 2026-11-27):** Bibles and
  songbooks come only from the central library through the cloud; nobody
  imports, exports or copies them on a laptop (Valentin's hand import of a
  Schlachter file could not be identified remotely). Since app 0.2.7 Bibles
  ride in the library push and apply live: held while a Bible passage is the
  projector's item, applied once it is not, no restart or message. Database
  Manager shows Bibles and songbooks read-only; the song editor cannot make
  songbooks. The library's Bibles: CGF Lyrics `library/tools/projector_bibles.json`
  (Schlachter 2000 v3 is id 14, replacing 13). See docs/library-from-cloud.md
  and docs/song-push.md.
- Pass 2 (blocked on sync + fixed CGF themes existing): gut Database Manager
  into a sync-status panel, reduce settings machinery to 2-3 CGF themes,
  quick-add upload flow.
- **Splash and icon (user decision 2026-11-23):** keep SoftProjector's cloud
  look the operators know; the splash says "CGF Projector", the icon shows
  "CGF" on a cloud. Drawn by `tools/branding/make_branding.py` (BUILD.md,
  "Splash and icon"). The church logo stays out of the public repo.
- **Front and back projectors (user decision 2026-11-24):** the back screen
  is display 2 and shows the same live item plus the time (Settings >
  General, "Show the time"). Displays remember their projector by name,
  serial number and position, so link drops can't swap front and back
  (BUILD.md, "Front and back projectors"; `tests/screens`). Needs a second
  video output: the church laptop's USB-C has no video, so a DisplayLink USB
  adapter feeds the back extender.
- **Classic Windows style (user decision 2026-11-23):** `main.cpp` sets Qt's
  "windowsvista" style, the look of SoftProjector 2.1. Qt 6's Windows 11
  style made the selected verse nearly invisible (faint grey, thin accent
  bar). The font stays Qt 6's Segoe UI 9 pt.
- Next milestones (as of 2026-11-23): the church laptop is installed and
  takes app releases (app 0.2.4, release 5, is the new splash and icon);
  publish the library once the
  CGF Lyrics round allows it (the laptop starts on the July v3), and host
  snapshots for remote laptops; phone
  remote → sermon watcher on the network bridge (OBS overlay and back-screen
  mirror are built; both await a test on the church network); proposals
  upload → web planner.

## Network bridge (built 2026-10-31; see docs/network-bridge.md)

- One port (8770) on the projector laptop serves the OBS overlay (`/stream`),
  its settings dock (`/stream/settings`), the back-screen mirror (`/screen`)
  and a WebSocket (`/ws`). Church
  network only; the app itself never talks to the internet. The code is in its
  own files (`livebridge`, `bridgeserver`, `softprojectorlive`, `src/web/`);
  `softprojector.cpp` only calls `publishLive()`.
- User decisions 2026-10-31: build order OBS text → pictures and video on the
  stream → back-screen mirror (tested on any laptop first; device not chosen)
  → phone remote → sermon watcher. One saved stream style, no profiles. No
  pairing code: anyone on the church network can control it, but browser pages
  must come from the bridge itself (Origin check).
- The stream follows the operator (Show/Hide, the output button), not the
  cable, so a dropped projector link doesn't blank it. Stream settings live in
  `cgf-network.json` beside the database, which snapshots replace.
- Sermon watcher (later): detection runs on the livestream PC beside
  cgf-translation and reuses its Soniox stream. A suggestion pre-selects the
  verse and the normal Go Live sends it (Enter already means next slide).
  Never auto-display.

## Library push (built 2026-11-23; see docs/song-push.md)

- Florian asked (2026-11-23) for new songs to reach the laptops "instantly"
  instead of on the sync agent's 10-minute poll. This is the tier-3 push
  above. Publishing (`publish_snapshot.py`) now calls `tools/push_release.py`,
  which diffs consecutive snapshots, signs the changed songs/songbooks and
  posts them to the push server.
- Push server: a separate Worker, `cgf-projector-push` (`push-server/`), in
  the CGF Vernon Cloudflare account, one SQLite Durable Object per church
  (`church_id` on every table). Kept apart from the Hub (`cgf-hub`) and its
  deploy runbook. Deployed 2026-11-23 (Florian's go-ahead) at
  https://push.cgfvernon.ca; redeploys need his go-ahead too.
- The app still never talks to the internet: `cgf-companion.exe` (Qt, built
  from `src/companion/`, started by the app) holds the connection and hands
  releases to the bridge on 127.0.0.1. The 2026-08-09 plan said "Python
  companion"; it is Qt so the church laptop needs no Python install and
  shares the app's Qt libraries.
- Safety: ECDSA P-256 signatures checked in the app (server and companion
  only relay); the projected song is never changed; quick-add ids >500000 and
  usage counts stay local; one transaction per release. The library version
  is max(`version.txt`, table `CgfSync`). Bibles ride in releases since app
  0.2.7 (held while a passage is on the projector; `needs: ["bibles"]` keeps
  older apps from half applying them). Snapshots stay the source of truth:
  other tables, and laptops offline for more than 200 releases, catch up
  through the sync agent's snapshot install.
- Secrets: publisher key and token in `%USERPROFILE%\.cgf-publish\`; each
  laptop's device token in its `cgf-cloud.json` (git-ignored, never commit).
- App updates (built 2026-11-23; Florian: he rarely gets to the church laptop
  and wants it always on the best version). Florian asked whether to move the
  connection into the app; decided: keep the companion, because a running
  program cannot replace its own files, the companion (started at Windows
  sign-in, `--stay`) installs updates while the app is closed, and network
  trouble cannot freeze the projector. Layout: launcher `CGFProjector.exe` in
  the install folder, one `app-<version>` folder per version, data in the
  install folder (`--data-dir`). A version that fails at start is rolled back
  by the launcher and never reinstalled. App files are stored in the push
  server's Durable Object (no R2, which would need a checkout in the
  dashboard). The laptop is installed from `tools/make_install.py`'s zip.

## The service order (spec seed for the web planner)

Sunday flow: 4 opening songs (stand on 3rd, sit after 4th) → intro
sermon/call to prayer (5-10 min) → song → sermon 1 (15-20) → song + program
items (songs/poems/testimonies, 2-3) → sermon 2 (15-20) → song (standing) +
program → sermon 3 (main, 30-40) → prayer w/ Lord's Prayer → standing:
needs/thanksgiving, prayer → announcements → greetings → closing blessing
("Die Gnade Jesu Christi… und die Gemeinschaft mit dem Heiligen Geist. Amen").
Roles: **Alex** = worship coordinator (collects program items, currently on
paper); **Flo (user)** plays piano — wants mobile chord view w/ transpose and
fast song flipping. The planner's default template is this liturgy, with
typed items and stand/sit cues (congregation view can show "please stand").

## Diagnosing a machine

- `CGFProjector.log` next to the exe: Qt warnings, monitor and sound device
  changes, and where the app put the display windows. Ask for it first when
  something went wrong at a service. `sync.log` in the same folder covers the
  sync agent.
- Projector hot-plug can be tested without hardware: run the real main window
  on Qt's offscreen platform and add or remove screens at runtime through the
  plugin's `setConfiguration` native resource (BUILD.md, "Church test prep").

## Working conventions

- This is a public repo, and git history is permanent. Read **NEVER-COMMIT.md**
  before pushing. Keep out of commits: real databases, tokens or URLs with
  secrets, screenshots with real announcement content, Bible modules, and the
  CGF Church logo or other church artwork.
- One commit per logical cut; build + smoke against the DB copy before each
  commit; log milestones in BUILD.md.
- Build dirs (`win32_build/` etc.) are gitignored at any depth.
- Upstream remote is wired; pull upstream fixes deliberately, never blindly.

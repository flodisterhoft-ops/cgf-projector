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
  clean machine). The Qt toolchain is not installed on this machine as of
  2026-10-05 (`C:\Users\Disterhoft\Qt` is gone); reinstall it per BUILD.md before
  building.
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

- Removed: Media player (0 media rows ever), Pictures/slideshows (1 slide in
  10 years), HTTP module downloader (insecure, upstream-disabled), UI
  translations, Donate/dead links, forced dark theme (upstream bug — checked a
  registry value that never exists; now forced light).
- Kept: Bible (F6) / Songs (F7) / Announcements (F8), schedule, song editor
  (future quick-add-that-uploads), **printing (user decision)**, usage counter
  (feeds analytics; counts should merge across machines via sync later).
- Planned add: **Share button** — song/setlist as link or PDF, possibly via
  Telegram (operators use Telegram).
- Pass 2 (blocked on sync + fixed CGF themes existing): gut Database Manager
  into a sync-status panel, reduce settings machinery to 2-3 CGF themes,
  quick-add upload flow. CGF logo swap pending logo file from Flo.
- Next milestones (as of the last commit): proposals upload → WebSocket bridge
  (remote + OBS + tier-3 push) → web planner.

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

## Working conventions

- This is a public repo, and git history is permanent. Read **NEVER-COMMIT.md**
  before pushing. Keep out of commits: real databases, tokens or URLs with
  secrets, screenshots with real announcement content, Bible modules, and CGF
  branding assets.
- One commit per logical cut; build + smoke against the DB copy before each
  commit; log milestones in BUILD.md.
- Build dirs (`win32_build/` etc.) are gitignored at any depth.
- Upstream remote is wired; pull upstream fixes deliberately, never blindly.

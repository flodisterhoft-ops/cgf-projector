# CGF Projector — project master brief

Read this first. It is the authoritative context for all agents and chats
working on the CGF Vernon projection/worship platform. Last updated: 2026-07-26.

## What this is

**CGF Projector** is CGF Vernon's fork of [SoftProjector](https://github.com/SoftProjector/softprojector)
(GPLv3, attribution kept — see README and LICENSE). The church's operators have
used SoftProjector for ~10-15 years; this fork keeps their muscle memory while
stripping what they never used and wiring the app into a central CGF library.

- Fork repo: https://github.com/flodisterhoft-ops/cgf-projector (public)
- GitHub account: `flodisterhoft-ops`
- Branches: `master` = upstream Qt6 + build fix + guard rails; `cgf-transform` =
  the transformation (rebrand + strips). Merge to master once approved.
- Local clone: `C:\Users\Disterhoft\cgf-projector` (space-free path, deliberate)
- User-facing install: `C:\Users\Disterhoft\CGFProjector\CGFProjector.exe`
  (Start Menu + Desktop shortcuts exist; refresh via robocopy after builds)
- Build: see **BUILD.md** (Qt 6.8.3 + MinGW 13.1 via aqtinstall; ~5 min from
  clean machine; toolchain at `C:\Users\Disterhoft\Qt`)

## The two-track strategy (decided 2026-07-26)

1. **Track 1 — this fork**: transition vehicle for the *current* rented
   building (fragile HDMI/CAT5e splitter chain) and the current operators.
   Simplify, rebrand, then make it a self-syncing offline client.
2. **Track 2 — the web platform** (the real long-term system): service planner,
   musician chord views, phone remote, OBS overlay, congregation view. In the
   NEW church building (~2 years out), projection itself likely becomes a
   fullscreen browser page per projector; the fork then retires or becomes the
   offline fallback.

## Architecture decisions (locked; challenge only with new evidence)

- **"No local database" means no VISIBLE database.** The app keeps an
  invisible, self-managing local cache, auto-synced from the central server.
  Operators never load files (no .spsc handling, no imports, no backups).
  Projection must keep working with the internet down — non-negotiable.
- **Library sync = snapshot DOWN / proposals UP.** Full published snapshots
  down (corpus is ~40 MB — no delta protocols as source of truth); local
  quick-adds upload as *proposals* into the master library's review queue.
  Never bidirectional merge.
- **Tier-3 live push (user decision 2026-07-26, replaces the freeze window):**
  per-song update pushes ride the WebSocket bridge and apply AT ANY TIME,
  including mid-service — the app itself writes its local DB and refreshes all
  non-live UI (search/preview/schedule) instantly. THE ONE GUARD: the
  currently-projected song's on-screen copy is never changed implicitly.
  NO OPERATOR ACTION IS EVER REQUIRED (user: "the guys are lazy, they'll
  never press it"): queued/schedule/preview songs update silently; the live
  song auto-applies the moment it leaves the screen; a reload nudge exists
  only as an optional shortcut. Projection renders from an in-memory
  copy, so DB writes can never disturb the screen by themselves. Push =
  best-effort freshness; the launch-time snapshot swap = guaranteed
  convergence (missed pushes self-heal). "Instant" = instant after PUBLISH,
  never per keystroke.
- **Real-time sync is for service PLANS only** (tiny data, WebSockets fine),
  never for the library.
- **The one worthwhile deep fork feature: a local WebSocket bridge** inside the
  app — broadcasts current on-screen state, accepts search/goto/next/prev.
  That single feature unlocks BOTH phone remote control AND the OBS livestream
  overlay (browser source, ~50% black background, auto-live whenever the desk
  shows something, manual hide for stream operator; replaces today's
  capture-card + luma-key setup).
- **Publish = explicit releases with version stamps**, not live master state.
  The curated master DB changes many times a day during curation; projectors
  consume releases only.
- **Multi-church later** (20-30 similar Russian-German churches): put
  `church_id` on all new server-side tables from day one. Keep NKJV /
  Schlachter 2000 Bible modules OUT of anything shared until rights are
  cleared (redistribution risk). Song lyrics: mostly old hymnody, lower risk.
- **English-only UI, trilingual CONTENT** (ru/de/en lives in the database).
- QSettings identifiers still say SoftProjector (deliberate, migration-free);
  revisit only with the sync work.

## Related projects on this machine

- **Master lyrics library** (the content source of truth):
  `C:\Users\Disterhoft\Documents\AI Projects\Livestream\CGF Lyrics\`
  → read its `AGENTS.md`. SQLite master with 8,264 live songs, 10k+ versions,
  25 songbooks, review queue, nothing-ever-deleted rule, verify_db.py gate.
  `songbook_entries.sp_song_id` maps ALL 8,386 production SoftProjector rows —
  the exporter/adapter foundation already exists.
- **Production SoftProjector 2.1** (the church's live system — do not touch):
  booth PC install `C:\Users\CGF Projector\SoftProjector`; a copy lives at
  `C:\Users\Disterhoft\OneDrive\CGF Vernon\SoftProjector Files\SoftProjector\`
  with the live `spData.sqlite` (8,386 songs, 4 Bibles, 124,425 verses,
  usage counters: 1,258 songs used, 5,798 total uses). KNOWN HAZARD: live DB
  inside OneDrive; plan is to move it local and keep OneDrive for backups only.
- **Offline songbook site**: `CGF Lyrics/cgf-songbook/` (active design);
  `lyrics-site/` is the older Next.js prototype (future publish target).
- Smoke-test DB: a COPY of the church DB is used for all testing; the
  original is never opened for writing by this project.

## Feature decisions from the operators' actual data

- REMOVED: Media player (0 media rows ever), Pictures/slideshows (1 slide in
  10 years), HTTP module downloader (insecure, upstream-disabled), UI
  translations, Donate/dead links, forced dark theme (upstream bug — checked a
  registry value that never exists; now forced light).
- KEPT: Bible (F6) / Songs (F7) / Announcements (F8), schedule, song editor
  (future quick-add-that-uploads), **printing (user decision)**, usage counter
  (feeds analytics; counts should merge across machines via sync later).
- Planned add: **Share button** — song/setlist as link or PDF, possibly via
  Telegram (operators use Telegram).
- Pass 2 (blocked on sync + fixed CGF themes existing): gut Database Manager
  into a sync-status panel, reduce settings machinery to 2-3 CGF themes,
  quick-add upload flow. CGF logo swap pending logo file from Flo.

## The service order (spec seed for the web planner)

Sunday flow: 4 opening songs (stand on 3rd, sit after 4th) → intro
sermon/call to prayer (5-10 min) → song → sermon 1 (15-20) → song + program
items (songs/poems/testimonies, 2-3) → sermon 2 (15-20) → song (standing) +
program → sermon 3 (main, 30-40) → prayer w/ Lord's Prayer → standing:
needs/thanksgiving, prayer → announcements → greetings → closing blessing
("Die Gnade Jesu Christi… und die Gemeinschaft mit dem Heiligen Geist. Amen").
Roles: **Alex** = worship coordinator (collects program items, currently on
paper); **Flo (user)** plays piano — wants mobile chord view w/ transpose and
fast song flipping. The planner's default template IS this liturgy, with
typed items and stand/sit cues (congregation view can show "please stand").

## Working conventions

- Public repo: read **NEVER-COMMIT.md** before pushing. No real databases, no
  tokens/URLs-with-secrets, no screenshots with real announcement content, no
  Bible modules, no CGF branding assets.
- One commit per logical cut; build + smoke against the DB copy before each
  commit; log milestones in BUILD.md.
- Build dirs (`win32_build/` etc.) are gitignored at any depth.
- Upstream remote is wired; pull upstream fixes deliberately, never blindly.

## Current status snapshot (2026-07-26)

Build gate PASSED (Qt6 master compiles in ~5 min, one 3-line fix; smoke-tested
vs church DB copy). Transformation branch pushed: rebrand (CGF Projector 0.1),
media+pictures cut, downloader+translations cut, light theme fix. Deployed app
82 MB (was 106), C++ 20,697 → 18,378 lines, tabs 5 → 3. Remaining gate items:
60-min dual-display soak on booth-class hardware; second-person build repro.
`cgf-transform` merged to master (048a016) after user verification.
**Exporter DONE** (2026-07-26): `CGF Lyrics/library/tools/export_spdata.py` —
deterministic, read-only on master, one Songs row per live placement (10,312),
all 8,386 sp_song_ids preserved, 1,926 new ids minted >12377 and recorded
append-only in `library/exports/sp_export_idmap.json` (never regenerate this
file — it is the id contract with every deployed client). Usage counters
(5,798 uses / 1,258 songs) carried from master. Bibles/Announcements/Themes/
Settings copied from the donor snapshot. Output `library/exports/spData-cgf.sqlite`,
smoke-tested in CGF Projector.
**Download agent DONE** (2026-07-26): `agent/cgf-sync-agent.ps1` — manifest-driven,
transport-agnostic, sha256-verified, never swaps under a running app (stages),
prev-1..N rollback, optional sqlite3 carry-forward of local Settings +
quick-adds (reserved client id band >500000, set by the exporter). Publisher:
`CGF Lyrics/library/tools/publish_snapshot.py` (versioned immutable snapshots,
monotonic content_version, sha256-dedup). End-to-end tested on this machine
against a local publish dir. NOT yet registered as a scheduled task and no
sqlite3.exe on clients yet (both pending user approval).
Next milestones: proposals upload → WebSocket bridge (remote + OBS + tier-3
push) → web planner.

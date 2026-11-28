# Library from the cloud (2026-11-27)

**Decision (Florian, 2026-11-27):** Bibles and songbooks come only from the
central library, through the cloud. Nobody imports, exports or copies Bible
or songbook files on a laptop.

Why: Valentin imported a Schlachter 2000 file by hand on the church laptop.
It became Bible 14 and is the second Bible on the screen, but from outside
nobody could tell which of the three reviewed files it was. The published
snapshot still carries the old, unreviewed Schlachter (id 13).

Until app 0.2.7 a laptop could only get Bibles from a snapshot, and no
snapshot could reach the church laptop: it has no sync agent, snapshots are
not hosted anywhere it can reach, and the push connection carried only songs
and app updates.

## Built first: Bibles in the song push (app 0.2.7)

Florian (2026-11-27): updates must apply at once, without a restart, a
warning or a flash; while something is presented they wait and apply as soon
as it is no longer live. Songs already worked that way, so Bibles now ride in
the same releases (docs/song-push.md, "Rules the code keeps"):

- `projector_bibles.json` (CGF Lyrics `library/tools/`) lists the library's
  Bibles; `projector_bibles.py` builds them for `export_spdata.py` and makes a
  Bible-only release from the latest snapshot (`republish`).
- `push_release.py` compares the Bibles of consecutive snapshots and sends a
  changed Bible whole, with the library's list and `bible_replaces`.
- The app holds the release while a Bible passage is the projector's item and
  applies it, in one transaction with the version stamp, once it is not. It
  then removes any Bible the library does not list and moves screen settings
  that named a removed Bible to its replacement.
- Tests: `tools/test_push_release.py` (14), `tests/push` (52, and end to end
  through a local push server and the companion with a held Bible release).

The rest of this page is the original plan for whole snapshots through the
cloud. Section 2 (install at start) is replaced by the live apply above;
whole snapshots are still needed for a first install and for laptops offline
for more than 200 releases.

## The original plan

### 1. Snapshots travel like app updates

- `publish_snapshot.py` (CGF Lyrics) calls `tools/push_release.py push` as
  now. That also uploads the snapshot itself, compressed and stored in chunks
  in the church's Durable Object the way app files are, and posts a signed
  library release:
  `{"kind": "cgf-library", "v": 1, "church", "version", "file": {"sha256", "bytes"}, "published_at"}`.
- The server keeps the two newest snapshots. It sends the latest library
  release to a laptop when it connects and to every connected laptop after a
  publish. A laptop too far behind for song releases (`resync`) gets it too.
- The companion downloads it in the background to
  `library\spData-cgf-v<N>.sqlite.partial`, checks the hash and renames it.
  It never touches `spData.sqlite`.

### 2. The app installs it at start

At start, before any window reads the database, and only when the
downloaded snapshot is newer than what the laptop has:

- Check the signature (`cgfsignature`, the same publisher key as song
  releases), the hash and SQLite's integrity check.
- In one transaction, replace the library tables: `Songs` (ids up to 500000;
  the laptop's own usage counts are kept), `Songbooks`, `BibleVersions`,
  `BibleBooks`, `BibleVerse`.
- Everything else stays as the laptop has it: settings, announcements,
  themes, pictures and media, slide shows, quick-add songs (ids above 500000).
- Record the version in `CgfSync` and `version.txt`, so later song releases
  build on it.
- A screen Bible setting that names a Bible the library no longer has falls
  back to the library's default, so the projector never shows an empty
  Bible slot.
- If any step fails, the database stays as it was, the log says why and the
  next start tries again.

Projection is never touched: the swap happens before anything can be live.
A publish during the week is downloaded while the laptop is on (the
companion starts at Windows sign-in) and installed at the next start.

This replaces the PowerShell sync agent's whole-file swap for laptops. The
agent was never installed on the church laptop and retires.

### 3. The app loses its file buttons (built in 0.2.7)

- Database Manager becomes a read-only library panel: library version and
  date, the Bibles, and the songbooks with their song counts.
- Import, export and delete of Bibles and songbooks are removed.
- The song editor can no longer create a songbook. New songs (quick-adds)
  stay; they go into an existing songbook and later upload as proposals.
- Theme import and export stay until the fixed CGF themes exist (Pass 2).

### 4. The library carries the reviewed Bibles (built)

`export_spdata.py` stops copying all Bibles from the 2026-07-20 donor. It
reads a Bible list kept in CGF Lyrics:

| id | Bible | Source |
|---|---|---|
| 2 | New King James Version (1982) | donor |
| 3 | Русский Синодальний Перевод | donor |
| 5 | German: Die Neue Luther (2009) | donor |
| 14 | German: Schlachter 2000 (v3) | `library/exports/bibles/Schlachter2000-reviewed-v3.spb` |

The old Schlachter (id 13) is dropped. v3 takes id 14 because Valentin's
import has that id on the church laptop: its screen setting keeps working
and the text under it becomes v3. The exporter checks verse counts (31,171
for v3), sample verses (Psalm 23:1 "Der HERR") and that the other three
Bibles match the donor exactly.

### Rights

The snapshots hold NKJV and Schlachter 2000. On the push server they are
served to registered device tokens only, never at a public address, and they
never go into git (NEVER-COMMIT.md). Florian chose cloud delivery on
2026-11-27; anything shared with other churches still leaves these Bibles out.

## Order of work

1. Exporter Bible list and checks (CGF Lyrics, scratch output only).
2. App: install at start, the Database Manager panel, the editor change;
   tests against a copy of the church database.
3. Push server storage, `push_release.py` upload, companion download; the
   push server's tests and `tests/update`.
4. Florian's go-aheads: push server redeploy, app release, first library
   publish.
5. Check the church laptop over the bridge: `hello` lists "German:
   Schlachter 2000 (v3)" and the new library version.

## Open

- What the first publish carries: the current master (all song work since
  July) or the July songs with only the Bibles new.

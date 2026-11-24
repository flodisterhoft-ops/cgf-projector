# Building CGF Projector (Windows)

Verified 2026-07-26 on Windows 11, starting from a machine with **no Qt installed**.
Total time from nothing to a running exe: about five minutes on a fast connection.

## Toolchain (one-time, ~4 GB, no admin rights needed)

Requires Python 3 with pip.

```
pip install aqtinstall
aqt install-qt   windows desktop 6.8.3 win64_mingw --modules qtmultimedia qtwebsockets --outputdir C:\Users\<you>\Qt
aqt install-tool windows desktop tools_mingw1310 qt.tools.win64_mingw1310 --outputdir C:\Users\<you>\Qt
```

Qt already installed without WebSockets (needed since 2026-10-31 for the
network bridge): add just that module with
`aqt install-qt windows desktop 6.8.3 win64_mingw --modules qtwebsockets --noarchives --outputdir C:\Users\<you>\Qt`.

## Build

From Git Bash (adjust `<you>`):

```
export PATH="/c/Users/<you>/Qt/Tools/mingw1310_64/bin:/c/Users/<you>/Qt/6.8.3/mingw_64/bin:$PATH"
cd src
qmake softProjector.pro -spec win32-g++
mingw32-make -j8
```

Output: `src/win32_build/bin/SoftProjector.exe`

## Deploy (self-contained portable folder)

```
windeployqt --qmldir qml win32_build/bin/SoftProjector.exe
```

The resulting `bin/` folder (~106 MB) is portable. The app reads
`spData.sqlite` from the folder the exe lives in.

## Fixes needed vs upstream master (97a88e7, 2025-09-16)

- `3rdparty/headers/qmediaplaylist.h` — removed `#include <qmediaenumdebug.h>`
  and the two `Q_MEDIA_ENUM_DEBUG(...)` lines. That header was removed in newer
  Qt 6 releases; it only provided debug pretty-printing for enums. No
  functional impact. (Since 2026-10-08 the whole vendored `3rdparty/`
  QMediaPlaylist is gone: its only use was two Qt 5 loop enums, which meant
  the wrong thing to Qt 6 and were replaced by `QMediaPlayer::Infinite`/`Once`.)

## Build-gate log (2026-07-26)

| Step | Result |
|---|---|
| Toolchain install (aqt, Qt 6.8.3 + MinGW 13.1) | 83 s |
| Compile (41 sources, one error → 3-line fix → rebuild) | ~90 s total |
| Executable | 2.2 MB (deployed folder 106 MB) |
| Launch against copy of production DB (8,386 songs, 124,425 verses, 4 Bibles) | Main window up, no crash (15 s smoke) |
| Compiler warnings | 91, non-fatal — to be triaged during transformation |

**Verdict: Qt 6 master is viable as the CGF Projector base.**

Remaining gate items before heavy investment:
1. 60-minute dual-display soak on booth-class hardware with the real workload.
2. A second person reproduces this build from this file alone.

## Transformation log — branch `cgf-transform` (2026-07-26)

Evidence-based strip, one commit per cut, smoke-tested against a copy of the
production database after each build:

| Commit | Cut | Why |
|---|---|---|
| Rebrand | CGF Projector 0.1, About attribution, Donate/dead-help removed | make it ours, GPL-clean |
| Media + Pictures | tabs, widgets, video-in-display (C++ and QML), picture settings page, vendored QMediaPlaylist, `QT -= multimedia multimediawidgets` | 0 Media rows, 1 slide in ~10 years of church use |
| Downloader + translations | legacy HTTP module downloader (insecure, upstream-disabled), Language menu + de/ru/cs/ua/hy UI translations | content comes from the CGF library; English-only UI (content stays trilingual) |

Numbers:
- C++ sources: 20,697 → 18,378 lines
- Deployed app: ~106 MB → **82 MB** (ffmpeg/multimedia gone)
- Operator tabs: 5 → **3** (Bible F6, Songs F7, Announcements F8)
- Printing **kept** (user decision); schedule, song editor, usage counter kept

Kept as inert data types so old schedules still load: `VideoInfo`, `SlideShow`.
Deliberately deferred to pass 2 (needs sync + fixed CGF themes to exist first):
Database manager gutting, settings-machinery reduction, quick-add upload flow.

## Restore log — branch `restore-media-pictures` (2026-10-08)

User decision: bring back Media and Pictures. The operators had avoided them
because they were broken, not unwanted. Restored by reverting 86edd9d (keeping
the build-dir `.gitignore` fix and dropping the downloader/translations again),
then fixing the Qt 5 → Qt 6 port damage:

- Media tab: no `QAudioOutput` (silent); string connects to signals and slots
  Qt 6 removed (`videoAvailableChanged`, `stateChanged`, `error`, `setVolume`,
  `setMuted`), so Go Live never enabled and play/pause could not pause; going
  live from the schedule busy-waited on the GUI thread (froze the app); stale
  "loaded" status after `stop()` and a queued `pause()` overriding `play()`;
  library paths reloaded as URL scheme `c:`; SQL broke on apostrophes;
  infinite recursion in `VideoPlayerWidget::setFullScreen`.
- Projector screen (`DisplayArea.qml`): `MediaPlayer` had no `videoOutput` or
  `audioOutput` (black, silent video); volume/mute/seek used removed APIs; no
  end-of-video signal; frozen last frame under later slides; Qt 5 playlist
  enums passed as Qt 6 `loops` (background video played 3 times, others kept
  the previous count). Vendored `3rdparty/` QMediaPlaylist removed.
- Pictures: empty slideshows loaded a blank slide; manual sort dropped slides;
  unreadable images became blank slides.
- Sync agent: carries local `Media`, `SlideShows` and `Slides` into new
  snapshots; runs the SQL from a BOM-free file (piping into sqlite3 prepended a
  byte-order mark that broke the ATTACH, so no new snapshot could ever install);
  `-bail` + one transaction; discards a half-merged staged copy and its journal.

Bug review of the rest of the code, by area, then fixes. The headline items:
theme tables migrated at start-up from the 2.1 layout (`use_disp_2`, inverted
meaning, missing colour columns), without which every theme save failed;
splitter settings decoded from hex again (the stored value doubled on every
launch); text layout loops can no longer freeze the app; uninitialised
"colour box" flags; schedule save with apostrophes, empty-schedule deletes and
failed saves; display windows follow monitors being connected/disconnected and
a missing screen number no longer crashes start-up; Unicode-aware search
(Cyrillic/umlauts); songs without verse headers are projected; Bible
Go Live/selection fixes; announcement delete mapping; print and database
manager fixes. Remaining known issues are listed at the end of this section.

Verification (2026-10-08): clean build with no errors (compiler warnings 83 →
51, all unused-parameter or deprecated-but-working). A scratch test harness
driving the real classes passes 42 media/picture/display checks and 18
main-window/schedule/settings/song checks. Song parsing was compared old vs
new on all 10,312 songs: 1,801 songs went from no slides to their full text,
and no song lost a distinct lyric line. The app was run twice on a copy of the
church database: migration applied, theme saves persist, startup log clean
(the ~9,600 font warnings are gone). Deployed app is ~106 MB again (FFmpeg
multimedia back).

Known remaining issues (not fixed in this round):
- Monitor connect/disconnect handling is untested on real hardware (needs the
  projector chain). Test it before relying on it in a service.
- Data: songs 794 ("Chorushine.") and 860 ("Verser 3") have typo'd stanza
  labels; fix them in the master library.
- The sync agent does not carry local themes or announcements forward; a new
  snapshot replaces them with the published ones (decide: central or local?).
- Bible "colour box behind text" is painted during measurement, so the box is
  sized for the first, largest font (only matters if a theme turns it on).
- `isAnnounceTitle` lacks German "Dia"; `BibleVerse` has no index (17-20 ms
  per lookup); the `Settings` table has no unique key, so rows can duplicate.
- Theme fonts are now stored in Qt 6's format; SoftProjector 2.1 (Qt 5) may not
  read them, so never open a CGF Projector database with 2.1.

## Network bridge (2026-10-31)

Phase 1 of the network outputs: the OBS stream overlay, with pictures and
video. Design, addresses and protocol: `docs/network-bridge.md`. New Qt
module: WebSockets (`Qt6WebSockets.dll`, picked up by `windeployqt`).

Verification (2026-10-31):
- Clean build; no compiler warnings from the new files.
- A scratch harness drives the real main window offscreen against a copy of
  the church database and checks what the bridge sends: 57 checks pass. They
  cover songs, Clear, Hide/Show, Bible lookups in all four Bibles (verse
  ranges, RST's own psalm numbering, RST book names cut to Russian),
  announcements, pictures (JPEG, stable address across Hide/Show), video file
  serving with HTTP ranges (206/416/HEAD), page serving, path traversal,
  405, the WebSocket Origin check, style saving to `cgf-network.json`,
  manual hide and junk messages.
- In a browser (Chromium; OBS 32 ships Chromium 127): lower third and full
  screen, one to four Bibles stacked, NKJV/RST [supplied words] in italics,
  pictures pillarboxed full screen, video following play/pause/seek/stop to
  within about 30-50 ms, settings dock at OBS-dock width.
- The dev exe starts the bridge within 0.5 s against the DB copy and closes
  normally with `taskkill` (no `/F`).

Back-screen mirror (`/screen`, same day): the front display emits its rendered
text and background layers after each change (`ProjectorDisplayScreen::frameShown`);
while a mirror is connected the bridge thread encodes them (PNG text, JPEG
background), drops repeats by comparing pixels, and the page crossfades them
with the projector's 500 ms fade. 16 more harness checks (73 in all) cover
subscribing (also right after start-up), frame sizes and types, unchanged backgrounds keeping their
address, no frames without a mirror. In a browser at 1080p the mirror matched
the projector's song and three-Bible layouts, and played the live video in
step (within about 40 ms).

Not yet tested: OBS on the livestream PC over the church network, and a real
back-screen device. Windows Firewall asks once on the church laptop (admin
click). If the host church's Wi-Fi isolates devices, the laptop needs
Tailscale. Videos a browser can't play (WMV, most AVI) and audio-only media
don't appear on the stream or the mirror.

## Church test prep (2026-10-31)

Before the first test on the church projector chain (one HDMI output, a
splitter and CAT extenders to the front and back projectors; the link drops
when a cable is touched), a second pass over projector link drops:

- A live video kept playing hidden during a drop, but when the link returned
  it started over from 0:00, and a paused video started playing. Putting the
  display windows back no longer redraws a live video. An extra output
  (display 2-4) that connects while a video is live starts the video where
  display 1 is, muted, and paused if display 1 is (from the review of PR #1).
  A seek asked for while a video file is still loading is applied once it has
  loaded; Qt 6 ignored it.
- Settings > OK while the projector was gone saved the greyed-out defaults:
  display 1 moved onto the operator's screen when the link returned, and
  display 2 was cleared. The saved screens are kept while only one screen
  exists.
- While the projector is gone, re-placing the display windows no longer
  switches to one-screen mode, where the next Go Live covers the operator's
  screen.
- Video sound follows the Windows default output (projector page and Media tab
  preview). Qt 6 keeps a player on the device that was the default when it
  started, so sound over HDMI stayed silent after a drop.
- `CGFProjector.log` next to the exe records Qt warnings, every monitor
  added, removed or changed (name, model, resolution, refresh rate, scale),
  sound output changes, and where the app put the display windows. Above 1 MB
  at start-up it moves to `CGFProjector-previous.log`; one session stops
  writing at 4 MB.

Verification: a scratch harness ran the real main window on Qt's offscreen
platform, with a copy of the home database and a silent test video, and
unplugged and replugged a fake projector screen through the offscreen plugin's
runtime screen configuration. 31 checks pass: drop and return, a loose cable
(five drops in 0.6 s, windows re-arranged once), output left off, Settings > OK
and Go Live during a drop, and a playing or paused video across a drop. With
the three fixes reverted, the 6 matching checks fail. A three-screen case
added after the review (a back screen plugged in while a video plays, then
replugged while paused) brings it to 37; with the join disabled, its 3 video
checks fail. On the offscreen
platform Qt hides windows moved off a removed screen by itself, so "hidden at
once" can't tell the app's hide from Qt's there. On Windows, Qt moves them to
the primary screen and the app's `screenRemoved` handler hides them
(qwindowsscreen.cpp, Qt 6.8.3). Still untested on real projectors.

Known remaining issues:
- Display screens are stored as indexes into the screen list. If Windows makes
  the projector primary after a reconnect, the order changes and display 1
  can land on the operator's screen. Not seen so far; the log records
  primary-screen changes.
- Settings > OK while a video is live restarts the video (the redraw reloads
  it).

## PowerPoint and PDF import (2026-10-31)

Pictures tab > "Import PowerPoint or PDF...": each slide becomes a picture,
saved as a slide show named after the file, selected and ready for Go Live.
Operators step through it like a song, and it can go in the schedule. It is
a slide show rather than a Media item because Media plays a file through;
slides need stepping.

- PowerPoint files (.pptx .ppt .pptm .ppsx .pps .odp) are exported by
  PowerPoint on the same computer, through COM. Hidden slides are left out.
  Without PowerPoint the app says so: PDF files import without it.
- PDF pages are drawn by Windows (`Windows.Data.Pdf`, Windows 8.1 and later),
  so no Office and no extra Qt module (Qt PDF would have meant a new download
  and a bigger deployment).
- The work is in `src/scripts/import-presentation.ps1`, built into the exe as
  a resource and run with Windows PowerShell 5.1 from a temporary folder; the
  app stays live meanwhile. Cancel lets the script close PowerPoint's copy of
  the file before it stops.
- Slides are drawn at the size Settings > Pictures stores pictures (default:
  fit 1280x1280), so text isn't blurred by a second scaling. PDF pages come
  out larger on displays scaled above 100 % and are fitted smoothly.

Verification: the script on a test deck (German and Russian text, a hidden
slide, a Cyrillic file name) and its PDF; a harness driving the real Pictures
tab: import, re-import "(2)", PDF, reload from the database, Go Live, broken
files, cancel (PowerPoint closed afterwards) and a 40-slide deck. Animations,
videos and sound in slides are not carried over.

## Library push (2026-11-23)

New and edited songs reach the laptops seconds after a publish. Design,
protocol and the going-live steps: `docs/song-push.md`. New pieces:

- The companion, a second small program beside the app. Build it after the
  app (same Git Bash PATH as above), then deploy it into the same folder:

  ```
  cd src/companion
  qmake companion.pro -spec win32-g++
  mingw32-make -j8 release
  windeployqt --no-translations ../win32_build/bin/cgf-companion.exe
  ```

  `windeployqt` adds the TLS plugin (`tls/qschannelbackend.dll`) it needs for
  `wss://`. The app links `bcrypt` (Windows) to check release signatures.
- The push server in `push-server/` (Node): `npm install`, `npm test`,
  `npm run check`; `npm run dev` serves it locally on port 8787.
- The publisher side, `tools/push_release.py`, needs Python with
  `cryptography`.

Verification (2026-11-23):
- Clean builds of the app and the companion; no compiler warnings from the
  new files.
- Push server: 14 tests (access, replay in order, the snapshot fallback for
  broken chains, live broadcast and acks, keep-alives, retention, connections
  kept across hibernation, device removal and token rotation, and that the
  entry module exports only what the Workers runtime accepts).
- Publisher: 8 tests (changed/added/removed songs, quick-adds and usage counts
  never travel, other tables flagged, parts split and signed, a changed
  payload fails the signature).
- App (`tests/push/run_tests.py`, offscreen against a copy of the v3
  snapshot): 30 checks. Python-made signatures verify in the app and a
  tampered one is refused; parts in any order; the database, song list,
  search, preview (same verse) and schedule update; the live song and the
  schedule's saved flag stay; quick-adds and usage counts stay; duplicates,
  gaps, a new songbook, and a release touching a quick-add id (refused,
  rolled back).
- End to end (`--e2e`): `wrangler dev`, the companion started by the app, and
  `push_release.py push` brought the app from v3 to v4; the companion log
  shows the release passed on and applied in the same second as the push.
  The server recorded the laptop at v4.
- The network bridge's 73 checks still pass (its runner now links `LIBS`
  from the project file too).

Deployed 2026-11-23: Worker `cgf-projector-push` at https://push.cgfvernon.ca
(version `4f8c7fe7`), `PUBLISH_TOKEN` set. Not yet done: hosting snapshots
for laptops outside Florian's network, a test on the church laptop and
network.

## App updates and the install package (2026-11-23)

The program itself now updates over the push connection; the church laptop
is installed from a zip. Design: `docs/song-push.md` ("App updates", "Install
package"). Branches merged first: the network bridge
(`claude/loving-lovelace-dfe9f8`) and PR #1 (`claude/zen-fermat-5fa2db`:
projector link drops, CGFProjector.log, PowerPoint/PDF import).

Build order, after the app (same Git Bash PATH as above):

```
cd src/companion && qmake companion.pro -spec win32-g++ && mingw32-make -j8 release
windeployqt --no-translations ../win32_build/bin/cgf-companion.exe
cd ../launcher && qmake launcher.pro -spec win32-g++ && mingw32-make release
```

The launcher lands in `src/win32_build/launcher/CGFProjector.exe` (it must not
overwrite the app of the same name in `bin/`). It links statically and needs
only Windows' own DLLs.

Publishing a new version, once the server with app releases is deployed:

```
python tools/push_release.py app-release --build-dir src/win32_build/bin --version 0.2.1 \
    --notes "What changed" --launcher src/win32_build/launcher/CGFProjector.exe
```

An install package for a laptop:

```
python tools/push_release.py add-device "Church laptop" --out <private folder>/cgf-cloud.json
python tools/make_install.py --version 0.2.0 --snapshot <spData-cgf-vN.sqlite> \
    --device-file <private folder>/cgf-cloud.json --out <private folder>
```

Verification (2026-11-23), all on this PC:
- Clean builds of the app, companion and launcher; no warnings from the new
  files.
- Push server: 18 tests (4 new: uploads checked against their hash, releases
  refused until every file is there and when seq does not rise, delivery to
  laptops with and without the app open, files served to devices only,
  cleanup after three releases).
- Publisher: 11 tests (3 new: data files stay out of a release, the
  qCompress format, signed app releases).
- `tests/update`: 23 checks. A package (56 MB zip) installed with
  `install.ps1`; release t2 (one new file plus the offscreen plugin)
  downloaded 2 files and copied 106; the companion restarted itself on t2;
  the app, started through the launcher without Qt on PATH, marked itself
  healthy and took a song push (library v3 to v4); t3 published while the
  app ran was offered as "ready" and app-t1 was removed; t4, whose program
  stops with code 3, was rolled back to t3 within its first seconds and
  marked bad.
- After the merge and these changes: bridge 73 checks, push 30 checks and
  the end-to-end push pass.

Live (2026-11-23, Florian's go-ahead): the server with app releases is
deployed (`4a15c42e`); app 0.2.0 is release 1, 0.2.1 release 2. The first
live run, in a folder without a launcher, found that the companion stopped
when it could not restart itself; fixed in 0.2.1 (it adds a missing launcher
and keeps running if the restart fails). With 0.2.1 a temporary device
downloaded 2 files, copied 105 and restarted on the downloaded companion.
"Church laptop" is registered and its zip built; the exact zip installs with
`install.ps1`.

On the church laptop (2026-11-23, Florian watching): installed from the zip
(0.2.1). Test update 0.2.2 was installed 3 s after publishing; the status bar
offered it, Restart now reopened the app as 0.2.2. Windows Firewall then asked
again (new program path); after `allow-network.cmd` (one port rule), test
update 0.2.3 restarted without any prompt. Checked over Tailscale: the
laptop's bridge reports app-0.2.3, library v3. Test releases 0.2.2 and 0.2.3
are on the server, so the next real version is 0.2.4 or later.

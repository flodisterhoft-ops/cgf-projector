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
  Without PowerPoint, LibreOffice does it since 2026-11-28 ("LibreOffice for
  PowerPoint files" below).
- PDF pages are drawn by Windows (`Windows.Data.Pdf`, Windows 8.1 and later),
  so no Office and no extra Qt module (Qt PDF would have meant a new download
  and a bigger deployment).
- The work is in `src/scripts/import-presentation.ps1`, built into the exe as
  a resource and run with Windows PowerShell 5.1 from a temporary folder; the
  app stays live meanwhile. Cancel lets the script close PowerPoint's copy of
  the file before it stops.
- Slides are drawn at the size Settings > Pictures stores pictures (default:
  fit 1280x1280), so text isn't blurred by a second scaling. PDF pages come
  out larger on displays scaled above 100 % and are fitted smoothly. (Since
  2026-11-28 they are drawn for a 1920 x 1080 projector instead: see
  "Presentation steps".)

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

## Splash and icon (2026-11-23)

Florian saw the old "softProjector" splash on the church laptop and asked for
the same cloud look with the CGF name, and "CGF" on the icon.

- `python tools/branding/make_branding.py` draws them into `src/`: the splash
  (`icons/splash.png`, 450 x 180, and `icons/splash@2x.png`), the window and
  taskbar icon (`icons/app-16.png` to `app-256.png`) and `cgf-projector.ico`
  (app and launcher). Clouds and colours follow SoftProjector's splash; the
  lettering is Arial Rounded MT Bold, which comes with Microsoft Office
  (`C:\Windows\Fonts\ARLRDBD.TTF`). Each size is drawn on its own so small
  icons stay sharp. A run takes about 80 s.
- Above 100% display scaling the splash and the About picture use the 2x
  file. The splash window is translucent, so its edges blend with the desktop
  (the old 1-bit mask is gone).
- The application name is now "CGF Projector". Windows shows it for windows
  without a title of their own, such as the splash. Nothing depends on it:
  settings live in the database.
- Titles and messages that still said SoftProjector now say CGF Projector or
  name the file type ("Songbook file (*.sps)", "Schedule file (*.spsc)"). The
  About box lost the upstream tagline; the upstream credits stay.

Checked on this PC (one 2560 x 1440 screen at 150%, a copy of the July
library): the splash shows the new clouds with soft edges over the desktop;
the title bar, taskbar, About box and Settings show the new icon and titles;
Explorer's icon for both exes is the new one at 16, 32, 48 and 256 px.

Published (Florian's go-ahead) as app 0.2.4, release 5, from 03347c1 with
the launcher: 106 files, 2 uploaded (the app and the launcher). The church
laptop was connected and reported app-0.2.4 installed 5 s later; it opens
from the app's next start (or Restart now in the status bar). The desktop
shortcut points at the replaced launcher; Windows may show the old icon there
until it refreshes its icon cache (sign out or restart).

## Classic Windows style (2026-11-23)

Florian could hardly tell which verse was selected on the church laptop. On
Windows 11, Qt 6.8 uses its Windows 11 style, which marks the selected row
with a faint grey fill and a thin bar in the Windows accent colour (purple on
the laptop). SoftProjector 2.1 ran on Qt 5 with the classic "windowsvista"
style, and `main.cpp` now sets that style: the selected row is light blue
with a blue outline, the row under the mouse lighter blue, and the selection
grey while another window is active. Tabs, buttons and scroll bars look as
they did in 2.1.

The font stays Qt 6's Segoe UI 9 pt (Qt 5 used MS Shell Dlg 2 at 8 pt). That
is why lists are less dense than in 2.1; the style does not change the row
height (18 px either way). Kept because it is easier to read; 8 pt would
bring back the old density.

Checked on this PC: a small Qt program drew list rows with both styles
(selected, under the mouse, window active or not), and the real app, its main
window captured in the background, showed the classic style.

Published (Florian's go-ahead) as app 0.2.5, release 6, from f367967 with the
launcher: 106 files, 1 uploaded (the app; the launcher was unchanged). The
church laptop reported app-0.2.5 installed 5 s later.

## Front and back projectors (2026-11-24)

Florian asked for separate control of the front projector and the back one
(for the people preaching), as ProPresenter does in the host church. The back
screen shows what the front shows, plus the time (his choice).

Wiring: the laptop's one HDMI output went into a CAT extender set to copy the
signal to both projectors, so both showed one picture and no setting could
change that. The church laptop (Lenovo LOQ 15IAX9E) has a single video output: its
USB-C port is data-only (Lenovo PSREF), so a USB-C-to-HDMI adapter shows
nothing. A second output needs a DisplayLink USB-to-HDMI adapter (its own
driver; any USB 3 port). Then laptop HDMI → front extender, the adapter → back
extender, Windows set to "Extend these displays", and in CGF Projector
Settings > General: Primary Display Screen = front, Secondary = back, "Show
the time".

The app could already show on up to four screens, each with its own look
(display 2 settings in the Bible, Songs, Announcements and Passive pages).
Added:

- The time of day in the top right corner of display 2 (`DisplayArea.qml`,
  6.5% of the screen height, white on a dark box, the laptop's time format).
  The stream and the back-screen mirror don't show it.
- Each display remembers its projector by name, serial number and position
  (`screenIdentity` in `spfunctions.cpp`), besides the screen number. Qt lists
  a screen that drops and comes back last, so with numbers alone a loose link
  could put display 1 on the back and the time on the front until the app
  restarted. A display keeps its projector while that stays connected; a
  display whose projector is missing waits, hidden (display 1 still takes any
  free projector, as before); settings from before 0.2.6 use the number once
  and remember the projector it finds. The log says which projector each
  display is waiting for.
- Settings shows the projectors' names ("2 - EPSON ... (1920x1080)"). OK
  without changes, or while a projector is unplugged, keeps the saved ones.

Checks: `python tests/screens/run_tests.py --db <CGF database>` runs the real
main window offscreen with a fake laptop, front and back and unplugs and
replugs them (front, back, both, back first), changes them in the Settings
form, and saves the settings: 34 checks pass. With the old number-only rule,
13 fail, among them the swap. The offscreen plugin adds screens in name order
and the first one is the operator's, hence the names "0 Laptop", "1 Front",
"2 Back". Also bridge 73 and push 30 checks pass. Not yet tried on the real
projectors.

Published (Florian's go-ahead) as app 0.2.6, release 7, from 13ce2d3 with the
launcher: 106 files, 1 uploaded (the app). The church laptop reported
app-0.2.6 installed 5 s later. It still has one picture for both projectors
until the DisplayLink adapter is in.

## Rotating pictures (2026-11-27)

Valentin, who runs the app, reported that pictures could not be turned and
that photos which look upright in Windows sometimes came in sideways.

- Sideways photos: phones and cameras store a photo as the sensor saw it and
  add a tag (Exif orientation) that says which way is up. Windows honours the
  tag, `QPixmap::load` does not. `loadPictureFile` in `spfunctions.cpp` reads
  through `QImageReader` with `setAutoTransform(true)`; the Pictures tab, the
  slide show editor and every background or wallpaper picked from a file use
  it. Pictures added before this fix stay as they were stored: turn them with
  the new buttons or add them again.
- Rotate left and Rotate right (`SlideShowItem::rotate`) beside the up and
  down arrows in the Pictures tab and in the slide show editor. In the tab, a
  picture of a saved slide show is written back at once, and the schedule's
  own copy of it is turned too (`SoftProjector::pictureRotated`). Pictures
  opened from a schedule item turn in the schedule only: a schedule file
  carries slide ids from the day it was saved, so they are never written to
  the database. In the editor the turn is saved with Save. The picture on the
  projector stays as it is until the next Go Live.
- Adding pictures selects the first new one, so it shows in the preview.
- Icons: `python tools/branding/make_rotate_icons.py`.

Checks: `python tests/pictures/run_tests.py --db <CGF database>` makes its own
photos with and without the tag and drives the real main window offscreen:
42 checks pass (it leaves `pictures-tab.png` beside the test exe). Screens 34
pass. Not yet tried with Valentin's photos.

## Bibles in the library push (2026-11-27)

Valentin imported a Schlachter 2000 file by hand on the church laptop (Bible
14, second on the screen), so it had two Schlachters and nobody could tell
remotely which file it was. Florian decided that Bibles and songbooks come
only from the cloud, and that updates apply at once without a restart,
warning or flash, waiting while something is presented
(docs/library-from-cloud.md).

- Releases carry Bibles (docs/song-push.md): a changed Bible whole, the
  library's Bible list and `bible_replaces`. The laptop ends with exactly the
  library's Bibles; settings that named a removed Bible move to its
  replacement. `needs: ["bibles"]` stops 0.2.6 and older from half applying
  one; the companion waits for an app that announces the feature.
- While a Bible passage is the projector's item the release is held; 0.3 s
  after something else goes live it applies (151 ms for Schlachter 2000 on
  Florian's PC) and the companion gets the result.
- Database Manager: Bible and songbook Import, Edit, Export and Delete are
  hidden. The song editor no longer offers "new songbook".
- Library side (CGF Lyrics): `library/tools/projector_bibles.json` and
  `projector_bibles.py`; `export_spdata.py` builds its Bibles from them
  (a full scratch export passed, 22,635 songs). Schlachter 2000 v3 is id 14
  and replaces 13. `projector_bibles.py republish` makes a Bible-only release
  from the latest snapshot.

Checks: publisher 14, `tests/push` 52 plus end to end 9 (a held Bible release
through wrangler dev and the companion, 6 parts of about 0.9 MB), bridge 73,
`tests/update` 23. All pass.

Published (Florian's go-ahead, "push the latest Schlachter now"):
- App 0.2.7, release 8, from 7d72fec with the launcher: 106 files, 14
  uploaded. The church laptop was offline from 20:49, so it installs at its
  next boot.
- Push server redeployed with the hold for releases that need a newer app
  (c83ce38, version `285c420a-36be-4fc4-acb2-aaf7e64fb85c`; server tests 21).
  Without it, a laptop booting into 0.2.6 could take the Bible release before
  its update installed.
- Library v4: `projector_bibles.py republish --no-push` (July songs, Bibles
  2, 3, 5 and 14 "German: Schlachter 2000 (v3)", 31,171 verses), pushed with
  this branch's `tools/push_release.py`: v1->v2 and v2->v3 empty, v3->v4 in 6
  parts (Bible 14 replaced, 13 removed). The server is at v4.

At the laptop's next boot: the old companion installs 0.2.7 and restarts on
it; its hello reports app-0.2.7, the server sends v4, and the app applies it
at start (or once a Bible passage leaves the screen). Check: the bridge's
`hello` at ws://<laptop>:8770/ws lists Bibles 2, 3, 5, 14 and library v4.

`%USERPROFILE%\.cgf-publish\push.json` names the main checkout's
`tools/push_release.py`. Before library-bibles was merged into master that
copy had no Bible support, so v4 was pushed with this branch's tool; the
merge (with app 0.2.8) gives master's tool Bible support, and normal
publishes use it again.

## App 0.2.8 (2026-11-27)

`library-bibles` (app 0.2.7, release 8, published alone earlier that evening)
was merged into master beside picture rotation: 6d3bb54. Checks on the merged
tree: publisher 14, `tests/pictures` 42, `tests/screens` 34, `tests/bridge`
73, `tests/push` 52 plus end to end, `tests/update` 23. All pass.

Published (Florian's go-ahead) as app 0.2.8, release 9, from 6d3bb54 with the
launcher: 106 files, 2 uploaded (the app and the companion). The church
laptop was offline, still on app-0.2.6; it installs only the latest release
when it next connects, so it goes straight to 0.2.8.

## LibreOffice for PowerPoint files (2026-11-28)

Valentin's laptop has no PowerPoint, so "Import PowerPoint or PDF" refused
his presentation. Florian's decision (2026-11-27): PowerPoint when the
computer has it, LibreOffice otherwise, fetched when it is first needed.
Microsoft's online converter (Graph, `format=pdf`) was ruled out: its
documentation limits conversion to files of 2 MB.

- Order, in `src/scripts/import-presentation.ps1`: PowerPoint through COM;
  without it LibreOffice (`soffice --headless --convert-to pdf`), whose PDF
  the existing page drawing turns into slides; with neither, exit code 2.
  Hidden slides are left out either way. A file that is neither a zip (pptx,
  odp) nor Office's older container (ppt) is refused before LibreOffice sees
  it, because LibreOffice would import a text file as one page of text.
- `PresentationImport::findLibreOffice` looks in the data folder
  (`libreoffice/program/soffice.exe` beside the database), then for an
  installed LibreOffice (registry, Program Files).
- With neither program the app asks once: "Download LibreOffice now?"
  (about 375 MB). The companion does it, `cgf-companion --fetch-libreoffice
  --data-dir <folder>` (`src/companion/fetchoffice.cpp`), so the app itself
  stays off the internet. It downloads LibreOffice 26.2.7 from The Document
  Foundation (download.documentfoundation.org, then the archive server),
  accepts only the file whose SHA-256 is built in, and unpacks it with
  `msiexec /a` into `<data folder>/libreoffice`. That is Windows Installer's
  unpack-only mode: no administrator rights, nothing in the Start menu,
  nothing registered. Dictionaries, menu translations, help and spare icon
  sets are removed afterwards (1.5 GB becomes about 740 MB; the slides come
  out pixel for pixel the same). The import then runs again by itself.
- LibreOffice keeps its settings in `<data folder>/libreoffice-profile`, apart
  from a LibreOffice the person uses themselves. The script writes one
  setting there: PowerPoint's default font, Aptos, exists only inside Office,
  and LibreOffice would replace it with a wider font and a serif italic.
  Calibri is on every Windows computer and 4.5 % narrower than Aptos, so no
  line gets longer than in PowerPoint.
- A newer LibreOffice: change the two addresses and the SHA-256 in
  `fetchoffice.cpp` together (the hash is on the download server, `<file>.sha256`).
- For checks and odd cases: `CGF_IMPORT_ENGINE=libreoffice` or `powerpoint`
  forces one program, `CGF_LIBREOFFICE` names a soffice.exe,
  `CGF_LIBREOFFICE_URL` another address for the same file.

How it looks: the test deck (German and Russian text, a picture, gradient and
shadow, rotated text, WordArt, a table) drawn by both programs side by side.
Layout, pictures, shapes and the table match; the font differs (Calibri for
Aptos) and LibreOffice shrinks overflowing text a little more. Animations,
videos and sound are not carried over, as with PowerPoint. The first import
after the download took 20 to 40 s (LibreOffice sets itself up), later ones
about 5 s, the same as PowerPoint.

Checks: `python tests/presentations/run_tests.py --db <CGF database> --msi
<LibreOffice msi>` drives the real main window offscreen as a computer
without PowerPoint and serves the installer from this computer: Cancel at the
question, a failed download, a file with the wrong SHA-256, the real download
and unpacking (in a folder with spaces in its path), imports afterwards
without a question, a Cyrillic file name, the Pictures tab, Cancel during the
conversion (LibreOffice stopped), a broken file; PowerPoint and PDF as before.
21 checks pass. The companion's real download from the official server was
run once by hand: 141 s on Florian's PC, 373 MB. Also `tests/update` 23,
`tests/pictures` 42 and `tests/push` 52 plus end to end pass. Not yet tried on
a laptop without PowerPoint, and not published.

## Presentation steps (2026-11-28)

An imported presentation was one still per slide. Florian asked (2026-11-28)
for the animations too, reduced to a few cheap ones ("convert all animations
to something simple like fade in"), for videos and sounds in slides to play,
and for the import to be built on the presentations people really bring.

What real files use: `tools/presentations/survey.py` on the 780 readable
.pptx files on Florian's PC (5,479 slides; school work, church services,
youth evenings). 21 have animations and 21 have transitions. Of the 834
effects that make something appear or leave, 77 % are a plain fade; then
float in, zoom, wipe, appear and a few fly-ins. Text built line by line is
the most common animation by far (741 lines). Fewer than half of the effects
start on a click; the others start with or after another one. Transitions
are nearly all fade, with a few wipes and one each of push, cover, uncover
and split. 39 videos and 9 sounds are inside slides. Garamond is the most
used font after Calibri.

How it works:

- `PresentationSteps::expand` (`src/sources/presentationsteps.cpp`) reads the
  .pptx (a zip of XML) and writes a copy in which every step of every slide
  is a slide of its own, without animations: shapes that have not appeared
  yet are taken out, text lines that have not appeared yet are made invisible
  in place (transparent letters, a space for a bullet), so the lines above
  never move. PowerPoint or LibreOffice then draws that copy as before.
  `.ppt`, `.pps` and `.odp` are saved as `.pptx` by the office program first
  (`-ToPptx` in the import script).
- A step is `SlideStep` (`presentationsteps.hpp`): slide and step number, the
  effect, its direction and length, whether it follows by itself and after
  how long, and the media it starts. It is kept as one line of text
  (`cgf-step:{...}`) in the `Slides.path` column, which slide shows and
  schedule files already carry, so nothing in the database changes.
- PowerPoint's effects become ours by what they do, not by their names: a
  wipe filter is a wipe from that side; a move with a fade is a float; a move
  alone is a fly-in from its side; a change of size is a zoom; box, circle,
  diamond and plus are a box opening from the middle; a barn-door filter is a
  split; visibility alone is a cut; everything else is a fade. Transitions:
  fade, cut, push, wipe, cover, uncover, split; any other is a fade of its
  length. A slide without a transition keeps the app's usual fade.
- Effects that start together are one step; "after previous" starts a step
  that follows by itself. Sequences that start when a shape is clicked follow
  as further clicks, because the operator has no mouse on the slide. Steps
  that show nothing new (an emphasis or a motion path alone) are left out. A
  video nothing starts gets a step of its own.
- On the projector (`ProjectorDisplayScreen::renderSlideStep`,
  `DisplayArea.qml`): one step forward plays the step's effect; any other way
  to a still (a jump, a step back, the first picture) is the usual fade. The
  effect works on the part of the picture that differs between the two
  stills: wipe, split and box show the new still through a growing window
  over that part; fly, float and zoom move that part, cut out with soft edges
  from the difference. No shape is drawn separately, so this works the same
  with PowerPoint and LibreOffice. Push, cover and uncover move whole stills.
  A fade lays the new still over the old one and fades it in: the app's usual
  cross-fade of two layers dips toward black half way, which made a white
  slide flash grey every time a line appeared.
- Videos and sounds are copied into `<data folder>/slide-media` under names
  made from their content, and play at their step: the video in its place on
  the slide, sound on display 1 only. Animated GIFs keep moving over the
  still. A linked video is found where the file says or beside the
  presentation. Deleting a slide show deletes its media unless another slide
  show uses the same files.
- A sound or video set to "hide during show" is left off the stills.
- Parts a slide cannot share with its copies are dropped (notes, comments,
  custom data) or copied (charts, SmartArt, theme overrides): PowerPoint
  refuses a file in which two slides share a "tags" part, which two of
  Florian's files have.
- If the office program will not draw the rewritten copy, the file itself is
  drawn, one still per slide; `CGFProjector.log` says so.
- LibreOffice gets a longer list of font replacements for fonts that come
  with Office and not with Windows (`$FontReplacements` in the import script;
  Garamond, Century Gothic and Montserrat are the common ones in Florian's
  files), chosen by measured width with `tools/presentations/fonts.py`, and
  only for fonts the computer lacks.

Not carried over: movement along paths, spins and other emphasis effects (the
object stays as it is), animations of single chart bars or SmartArt parts
(the whole chart appears), text that types letter by letter (a wipe), slides
that advance by themselves after a time, online videos, and sound that runs
across slides. The stream overlay and the back-screen mirror get every still
without the motion and without slide sound.

For checks and odd cases: `CGF_IMPORT_STEPS=0` imports one still per slide.

Tools (`tools/presentations/`): `survey.py` counts what a set of files uses;
`corpus.py` runs the splitter over a set and has PowerPoint or LibreOffice
draw the copies, to find files they refuse; `fonts.py` measures font widths;
`stepstool` is the splitter as a command (build with qmake and mingw32-make
in its folder). Their reports name no files unless asked to.

Releasing: the app now needs `Qt6Xml.dll`. Run `windeployqt` again before
`push_release.py app-release` (`tests/update` fails on an incomplete package).

Checks: `python tests/steps/run_tests.py --db <CGF database> [--video <file>]
[--soffice <soffice.exe>]` has PowerPoint make a deck with each kind of
animation, imports it through PowerPoint and LibreOffice and steps through it
on the real main window offscreen, watching the projector's effect layer, the
steps that follow by themselves and the media players: 60 checks pass (it
leaves `frame-*.png` beside the test exe, the projector in the middle of a
push, a wipe and a fly-in). With `--record <presentation> --frames <folder>`
it runs no checks and records the projector stepping through a real file, as
pictures and a GIF: how the grey flash was found. `tools/presentations/corpus.py` on Florian's
files: the splitter read all 780; PowerPoint and LibreOffice each drew every
still of the 21 files with steps and of 25 of the others. Also
`tests/presentations` 21, `tests/pictures` 42, `tests/screens` 34,
`tests/bridge` 73, `tests/push` 52 and publisher 14 pass.

Merged into master on Florian's word (2026-11-28, a fast-forward to d1ea200).
On master, with the build deployed again (`Qt6Xml.dll`): `tests/update` 23
and the push end-to-end run pass. A test package, 0.2.9-test2, was made for
Florian's laptop without PowerPoint. Not yet tried with an operator or on
that laptop, and not published.

## What's new window (2026-11-28)

Florian asked for a window at the first start after an update instead of the
bar at the bottom right, with icons, short lines and counts instead of song
lists for big batches, mainly for Valentin (docs/whats-new.md).

- `WhatsNewDialog` (`whatsnew.cpp`) opens 1.5 s after start on an installed
  laptop when something changed since it last opened; Help > What's new opens
  it again. Program items come from `src/whats-new.json` (in the resources);
  library changes are recorded per applied release (`LibraryPush::
  libraryRecorded`) in `cgf-whats-new.json` beside the database.
- The update bar and its Restart now button are gone; an installed update
  only logs "App update X is ready; it opens at the next start".
- Wording: the window's texts were reviewed with GPT Astra 6.2 on high effort
  (Florian's request) and taken as it wrote them.

Checks: `tests/push` 69 (17 new: what was recorded, the lines, the 5-song
limit, 5,000 songs, shown once, Help menu, two screenshots) plus end to end
9, `tests/update` 23, bridge 73, screens 34, pictures 42, publisher 14.

## Proposals up (2026-11-28)

Operators' new songs and song fixes go from the laptops to the CGF Lyrics
master, accepted by default, with a Telegram message to Florian that has an
Undo button (his decision, 2026-11-28). Design, protocol and setup:
`docs/proposals-up.md`. Branch `claude/quizzical-golick-70ddec`.

- App: `src/sources/proposals.cpp` (new, in `softProjector.pro`); the song
  editor and `LibraryPush::apply` call it. The companion relays the new
  messages, so app and companion go out together in one release.
- Push server: `push-server/src/proposals.ts`, a `proposals` table and three
  routes. Needs a redeploy, then `tools/telegram_setup.py` for the bot's
  secrets (a second deployment). Both wait for Florian's go-ahead.
- Florian's PC: `tools/pull_proposals.py` and
  `tools/import_projector_proposals.py`, which loads CGF Lyrics' own tools and
  writes the master under its rules.

Checks: server 33, `tools/test_pull_proposals.py` 2, `tests/push` 52 plus
`--proposals` 30 plus end to end 9 (a proposal and its undo through
`wrangler dev` and the companion), `tests/proposals/run_import_test.py` 20 on a
scratch copy of the master (apply, undo and keep, each past `verify_db.py
--strict-current`). The master itself was never written.

Deployed (Florian's go-ahead, 2026-11-28) from this branch: Worker version
`81f3914b-0fbe-41f0-a135-2e3bbf741079` at push.cgfvernon.ca, after 33 server
tests. Checked live: status still answers (library v4, app release
app-0.2.8, the church laptop last seen on app-0.2.6), the proposals list is
empty, and the Telegram route refuses calls without its secret (401). The
bot's secrets are not set yet, so proposals would be stored and nobody told.

Not done: the bot (`tools/telegram_setup.py`, Florian), the app release.

Slide size (Florian's decision, 2026-11-28): presentations and PDFs are drawn
to fit 1920 x 1080, the size of the church's projectors, whatever Settings >
Pictures gives photos (`-Size 1920 -Height 1080` to the import script). A
slide has no size of its own; at that setting's default of 1280 every slide
was stretched one and a half times on the projector, which softens text, and
more than the projector's size would show nothing more. A 4:3 slide comes out
1440 x 1080. Stills take more room: 1.15 times on a 4:3 picture book, 1.67
times on a 16:9 deck with photos (184 KB a still instead of 110), measured on
two of Florian's files. A slide show imported before keeps its size until it
is imported again. `tests/steps` 60 and `tests/presentations` 21 pass with it.

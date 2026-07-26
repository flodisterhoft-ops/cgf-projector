# Building CGF Projector (Windows)

Verified 2026-07-26 on Windows 11, starting from a machine with **no Qt installed**.
Total time from nothing to a running exe: about five minutes on a fast connection.

## Toolchain (one-time, ~4 GB, no admin rights needed)

Requires Python 3 with pip.

```
pip install aqtinstall
aqt install-qt   windows desktop 6.8.3 win64_mingw --modules qtmultimedia --outputdir C:\Users\<you>\Qt
aqt install-tool windows desktop tools_mingw1310 qt.tools.win64_mingw1310 --outputdir C:\Users\<you>\Qt
```

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
  functional impact.

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

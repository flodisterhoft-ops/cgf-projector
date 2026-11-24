# Network bridge

CGF Projector shares what the projector shows with browsers on the church
network: the OBS stream overlay and the back-screen mirror so far. The phone
remote and sermon watcher use the same bridge later. The bridge serves
the local network only; the app itself never talks to the internet (see the
architecture record in CGF Lyrics, `docs/plans/2026-08-09-live-platform-architecture.md`).

## Addresses

One port, default **8770**, on every network interface of the projector laptop:

| Address | What |
|---|---|
| `http://<laptop>:8770/stream` | OBS Browser Source, 1920×1080, transparent |
| `http://<laptop>:8770/stream/settings` | Stream settings; an OBS custom dock or any browser |
| `http://<laptop>:8770/screen` | Back-screen mirror: identical to the front projector |
| `http://<laptop>:8770/` | Links to the pages |
| `http://<laptop>:8770/api/state` | Current state as JSON, for debugging |
| `ws://<laptop>:8770/ws` | The bridge protocol (below) |

`<laptop>` is the laptop's Windows name. If the host church's Wi-Fi blocks
traffic between devices, put the laptop on Tailscale and use its Tailscale name.

The first time the app runs, Windows Firewall asks whether to let it on the
network. Someone with admin rights must click Allow on the church laptop.

### OBS setup (livestream PC)

1. Sources → + → Browser. URL `http://<laptop>:8770/stream`, width 1920,
   height 1080. Leave "Shutdown source when not visible" off.
2. Docks → Custom Browser Docks… → name "Stream", URL
   `http://<laptop>:8770/stream/settings`.

The overlay is transparent when nothing is shown and after the laptop has
been unreachable for 10 seconds, so a dead laptop never leaves stale text.

### Back screen

Any small PC or laptop at the back projector, on the church network (the
existing CAT cable can carry Ethernet instead of HDMI): open
`http://<laptop>:8770/screen` full screen, for example with
`chrome --kiosk http://<laptop>:8770/screen` started with Windows, and set it
never to sleep. It shows exactly what the front shows, from the projector's
own rendered text and background with its fades, plus the live video. If the
laptop becomes unreachable, the last picture stays and after 10 seconds a
small grey dot appears in the corner.

## Files and switches

- `cgf-network.json` beside `spData.sqlite` holds the stream style
  (`"stream"`), and optionally `"port"` and `"enabled": false` to turn the
  bridge off. A library snapshot replaces the database, so none of this lives
  in it. The manual "Hide from stream" is not saved; it resets when the app
  restarts.
- `CGF_BRIDGE_PORT` overrides the port (tests, a second instance).
- `CGF_BRIDGE_LOCAL=1` listens on this computer only: tests run without a
  Windows Firewall prompt.
- `CGF_WEB_DIR=<repo>/src/web` serves the pages from disk instead of the
  built-in copies, so page edits need only a browser refresh.

## Code

| File | Role |
|---|---|
| `src/sources/softprojectorlive.cpp` | Main-window side: `publishLive()` describes the screen; called from `updateScreen()`, Clear and the output button in `softprojector.cpp` |
| `ProjectorDisplayScreen::frameShown` | The front screen's rendered layers after each change, for the mirror |
| `src/sources/livebridge.cpp` | GUI-thread facade: drops repeats, converts pictures once, throttles video positions |
| `src/sources/bridgeserver.cpp` | Own thread: HTTP, WebSocket, Bible lookups (own read-only DB connection), settings file |
| `src/web/` (`web.qrc`) | The pages; plain HTML/CSS/JS modules, no build step |

The network runs on its own thread so neither clients nor the unindexed
`BibleVerse` lookups can make a projector transition stutter.

## Protocol

JSON text messages over the WebSocket. Browsers may only connect from pages
the bridge served (the `Origin` must match `Host`); programs without an
`Origin` (companion, watcher) may connect freely. There is no pairing code
(decision 2026-10-31): anyone on the church network can control it.

### Server → client

On connect: `hello`, `style`, the current `live`, and `video` if one is loaded.

```jsonc
{"type": "hello", "protocol": 1, "app": "CGF Projector", "host": "CGF-LAPTOP", "port": 8770,
 "addresses": ["192.168.1.23"],
 "bibles": [{"id": "2", "name": "New King James Version (1982)", "abbr": "NKJV"}],
 "sample": { /* John 3:16 as a "bible" item, for previews */ }}

{"type": "style", "style": { /* see defaultStyle in src/web/assets/bridge.js */ }, "hidden": false}

{"type": "live", "seq": 42,
 "visible": true,       // the operator is showing something (Show/Hide and the output button)
 "cleared": false,      // Clear: text gone, background stays
 "bibles": ["2", "5", "3"],   // the projector's Bibles in screen order
 "kind": "bible",       // "song" | "bible" | "announcement" | "picture" | "video" | "none"
 "bible": {"verseIds": ["B043C003V016"], "projected": ["2", "5", "3"],
           "versions": [{"id": "2", "name": "…", "abbr": "NKJV", "book": "John",
                         "reference": "John 3:16", "text": "…",
                         "verses": [{"chapter": 3, "verse": 16, "text": "…"}]}]}}
```

`versions` holds every installed Bible that has the verses, each with its own
numbering (RST Psalm 22 = NKJV Psalm 23). Other kinds carry one object named
after the kind:

```jsonc
"song":         {"id": 123, "number": 245, "title": "…", "songbook": "…",
                 "stanzaTitle": "Verse 1", "text": "line\nline", "index": 0, "count": 5}
"announcement": {"title": "…", "text": "…", "index": 0, "count": 3}
"picture":      {"name": "…", "index": 2, "count": 10, "url": "/media/picture/40"}
"video":        {"name": "clip.mp4", "seq": 41, "url": "/media/video/41",
                 "mime": "video/mp4", "audioOnly": false}
```

```jsonc
{"type": "video", "seq": 41, "state": "playing", "position": 12345, "duration": 60000}
{"type": "tick"}   // every 10 s; a client hearing nothing for 25 s reconnects
// Only to clients that subscribed to frames (the back screen):
{"type": "frame", "seq": 7, "text": "/frame/text/7", "back": "/frame/back/5",
 "width": 1920, "height": 1080, "fade": true, "ms": 500}
```

A frame is the projector's own rendering: a transparent PNG text layer and a
JPEG background at the projector's resolution. The background address
changes only when the background does. The projector converts frames only
while a mirror is connected.

`video` arrives on play/pause/stop, after a seek, and every 5 s while
playing; clients play on by themselves in between. Its `seq` matches
`live.video.seq`. Media addresses serve only the live file, with HTTP range
support for video, and 404 once something else went live.

### Client → server

```jsonc
{"type": "setStyle", "style": { /* the whole style object */ }}   // saved, sent to everyone
{"type": "setHidden", "hidden": true}                              // manual hide for the stream
{"type": "subscribe", "frames": true}                              // send me rendered frames
```

Programs on the laptop itself (the companion) may also send signed library
`release` messages; `hello` then carries `library.version`. See
`docs/song-push.md`.

Planned: `command` (next/prev/goto/hide for the phone remote) and `suggest`
(sermon watcher).

## Tests

`tests/bridge/` drives the real main window offscreen and checks what the
bridge sends (73 checks: live state for every kind, Bible lookups, media
serving with ranges, the pages, the Origin check, style saving, mirror
frames). It needs a copy of the CGF church database, which it copies first:

```
python tests/bridge/run_tests.py --db C:/Users/<you>/CGF/CGFProjector/spData.sqlite
```

`--serve` keeps the bridge up on port 8779 for a browser instead; write
commands such as `song 214 0`, `bible B043C003V016`, `video <path>`, `hide`
or `quit` into `tests/bridge/out/bin/cmd.txt`.

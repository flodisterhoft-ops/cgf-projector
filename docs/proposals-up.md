# Proposals up: operators' song changes reach the library

Built 2026-11-28 (branch `claude/quizzical-golick-70ddec`). When an operator
on a church laptop adds a song or fixes one in the song editor, the change
goes to the CGF Lyrics master as a proposal and Florian gets a Telegram
message with an Undo button. This is the "proposals up" half of "snapshot
down / proposals up" (AGENTS.md); songs never merge both ways.

## Decisions (Florian, 2026-11-28)

- **Accepted by default, with an undo.** Operators change songs to improve
  them, so a change goes into the master unless Florian presses Undo. This
  replaces the plan's "Florian reviews before anything applies".
- **The operator's fix stays on the laptop** from the moment it is saved, also
  when a release brings the same song, until the master has it or Florian
  undoes it.
- **Valentin is left out for now.** Only Florian gets the messages.
- **An accepted new song replaces the laptop's own copy** once the library's
  copy arrives; its usage count moves along.
- **Who:** the laptop's device name and the Windows user. The editor asks
  nothing.

## The path

```
song editor ─► CgfProposals ─► bridge ─► cgf-companion ─► push server ─► Telegram (Florian)
 (laptop)       (spData)                    (laptop)     (Cloudflare)       │ Undo / Keep
                                                              ▲              ▼
Florian's PC: import_projector_proposals.py ─ pull, apply, mark ┘ ◄── the undo flag
                   └─► CGF Lyrics master (review_queue kind projector_proposal)
```

1. The song editor saves as before, then `Proposals::recordSave` compares the
   text fields with what was there and writes a row to `CgfProposals`. A
   change of fonts, colours or background only is not proposed.
2. When the companion is connected to the server and the app has said hello,
   it sends `proposalsReady`; the app sends every queued proposal. Offline,
   they wait; nothing about projection depends on them.
3. The server stores the proposal (`proposals` table, `church_id` on every
   row), answers `proposalStored`, and 30 seconds later sends Florian one
   Telegram message per proposal. A song saved again before the message went
   out replaces the earlier proposal, so one fix makes one message.
4. Undo under the message sets the proposal's undo flag; Keep clears it.
   Either goes to the laptop at once if it is connected (else at its next
   hello), and the laptop puts the library's text back or the operator's.
5. On Florian's PC, `python tools/import_projector_proposals.py` pulls the
   proposals that need work, makes the master match his choice, and tells the
   server what the master has now (`applied` or `undone`). The laptop hears
   `applied`, and the next release of the song ends its own copy.
6. The next publish carries the change to every laptop.

Example message:

```
Church laptop (CGF Projector): changed "Amazing Grace" (Hymns 12): 1 line changed, 1 line added.
"Amazing grace how sweet the sond"
→ "Amazing grace how sweet the sound"
[Undo]
```

## On the laptop

`CgfProposals` in `spData.sqlite`:

| column | |
|---|---|
| `uuid` | the proposal's id everywhere |
| `kind` | `edit` (a library song, id up to 500000) or `new` (a quick-add, above) |
| `song_id` | the laptop's song id |
| `base` | the text the operator started from (JSON), empty for `new` |
| `proposed` | the operator's text (JSON) |
| `library` | the library's latest text of the song, for an undo |
| `replaces` | the earlier proposal for the song, when it had been sent already |
| `state` | `queued` → `sent` → `applied` → `done`, or `undone`, `rejected`, `superseded` |

Text fields: title, songbook, number, song text, category, key (`tune`),
words, music, notes. Fonts, colours and backgrounds stay on the laptop.

Rules the code keeps:

- **The screen is never changed.** The projected song is its own copy
  (`current_song`); neither a release, nor an override, nor an undo touches
  it.
- **The operator's text wins until the master has it.** Inside a release's
  transaction (`LibraryPush::apply`), after the songs are written,
  `Proposals::applyOverrides` writes the operator's text back over each song
  with a queued or sent change, and keeps the library's new text in `library`
  for an undo. A release that brings exactly the operator's text, or any
  release of the song after the master reported `applied`, ends the override.
- **Saved again** before sending: the queued proposal is updated. After
  sending: a new proposal that starts from the first one's text, with
  `replaces`; the first becomes `superseded`.
- **An accepted new song** (`applied`, kind `new`): when a release brings a
  library song in the same songbook with the same title, the quick-add row is
  deleted, its count added to the library song's and its date kept if the
  library song has none. The song list then shows it once.
- The server's `rejected` (malformed) ends a proposal; `later` (more than 100
  a day from one laptop) keeps it queued.

## The push server

Proposals arrive over the laptop's WebSocket, so the companion needs no new
connection. The server never sends a proposal to another laptop.

```jsonc
// laptop → server
{"type": "proposal", "uuid", "kind": "edit", "song_id": 7, "base": {...}, "proposed": {...},
 "created_at", "user": "CGF Projector", "library_version": 4, "replaces": null}
{"type": "proposalStateAck", "uuid", "state": "undone"}
// server → laptop
{"type": "proposalStored", "uuid", "status": "stored"}     // duplicate | later | rejected (+ reason)
{"type": "proposalState", "uuid", "song_id": 7, "state": "undone"}  // pending | applied | undone
```

Between the app and the companion the same messages pass through the bridge
(local programs only), plus `proposalsReady` (companion → app) and
`proposalStateResult` (app → companion, sent on as `proposalStateAck`).

| Request | Who | What |
|---|---|---|
| `GET /v1/<church>/proposals[?state=]` | publisher token | every proposal with its state, undo flag and payload |
| `POST /v1/<church>/proposals/<uuid>` | publisher token | `{"state"?, "undo"?, "note"?}`; tells the laptop if that changes what it shows |
| `POST /v1/<church>/telegram` | Telegram (`X-Telegram-Bot-Api-Secret-Token`) | Undo and Keep presses, from Florian's chat only |

Server states: `new` → `received` (pulled) → `applied` or `undone`; `replaced`
for one superseded before its message. The undo flag is separate: it is what
Florian wants, the state is what the master has. Limits: 64 KB a proposal,
100 a day per laptop.

Secrets (Worker secrets, set by `tools/telegram_setup.py`, never in git, chat
or logs): `TELEGRAM_BOT_TOKEN`, `TELEGRAM_CHAT_ID`, `TELEGRAM_WEBHOOK_SECRET`.
Without them proposals are stored and nobody is told. A failed message is
tried again by the object's alarm every five minutes, at most ten times.
Telegram's Bot API is free.

## In the master (CGF Lyrics)

`tools/import_projector_proposals.py` lives here and loads CGF Lyrics' own
tools (`pipeline_safety`, `segment_syllables`, `cgf_lib`, `export_spdata`)
from `..\CGF Lyrics\library\tools`, so it runs against that checkout's live
code. It follows that project's write rules: one `BEGIN IMMEDIATE`
transaction, an online checkpoint inside the lock (`library/backups/`),
`verify_db.py --strict-current` over the uncommitted state before the commit
(about five minutes on the 2.4 GB master), a report in
`library/reports/projector-proposals/`. `--dry-run` does all of it and rolls
back.

How a proposal lands, per laptop song id (`songbook_entries.sp_song_id`, or
`library/exports/sp_export_idmap.json` for ids the exporter minted):

- The laptop's starting text must be what the exporter gives for that entry
  today (its pinned version, with chorus repeats written out). If the master
  moved on, the proposal is not applied: it gets an open review_queue row and
  waits for Florian; the laptop keeps the operator's text meanwhile.
- **Text:** a new version (`source_kind = 'projector_proposal'`), pinned to
  the entry. If the entry showed the song's primary version, the new one
  becomes primary and the old one stays as a secondary version; if it showed
  a secondary version, the new one is secondary too. Chords stay on the old
  version. Both versions get fresh syllable sets in the right phase.
- **Punctuation or capitals only:** the master keeps one version per wording
  (`norm_text`), so this waits for Florian (open queue row).
- **Title:** the shown version's title; for the primary, the song's title as
  well, with the old one kept as an alt title. **Number:** the entry's.
  **Key, words, music** without a text change: the shown version, in place.
  **Songbook move, notes:** recorded only.
- **New song:** a new master song (`origin = 'document'`) with one version and
  an entry in its songbook; the next export gives it an id.
- **Undo:** the entry, the primary flag, titles and details go back to the
  recorded before values; the operator's version stays as a secondary one.
  An undone new song becomes `rejected` (hidden, kept). **Keep** after an
  undo re-applies, reusing the operator's version.

Each proposal has one review_queue row, kind `projector_proposal`,
`identity_key` `projector_proposal:<uuid>`, with the laptop, the user, the
before and after values and a history. It is `closed` once applied or undone
(the open count stays the list of real questions) and `open` when it waits
for Florian.

## Setup (Florian, once, needs his go-ahead to deploy)

1. Redeploy the push server: `cd push-server; npm test; npx wrangler deploy`.
   Done 2026-11-28: version `81f3914b-0fbe-41f0-a135-2e3bbf741079`.
2. In Telegram, open @BotFather, `/newbot`, choose a name. Keep the token to
   yourself: not in chat, a repository or a log. Send the new bot any message.
3. In a normal PowerShell window in this repository:
   `python tools/telegram_setup.py --wrangler-dir push-server`. It asks for the
   token without showing it, finds your chat, stores the three secrets in one
   `wrangler secret bulk` (another deployment), points the Undo button at
   `https://push.cgfvernon.ca/v1/cgf-vernon/telegram` and sends a test
   message.
4. Publish an app release with this version (0.2.9) so the church laptop
   records and sends proposals; the companion inside it relays them.

Then, after a message: `python tools/import_projector_proposals.py` (pulls,
applies, verifies, commits, reports back), and publish as usual.
`python tools/pull_proposals.py list` shows what is waiting;
`pull_proposals.py undo <uuid>` does what the button does.

## Files

| Where | What |
|---|---|
| `src/sources/proposals.cpp` | `CgfProposals`, the editor hook, overrides in a release, states from the server |
| `src/sources/editwidget.cpp` | calls `Proposals::recordSave` after a save |
| `src/sources/librarypush.cpp` | calls `Proposals::applyOverrides` inside the release transaction |
| `src/sources/softprojectorsync.cpp`, `bridgeserver.cpp` | wiring to the companion |
| `src/companion/companion.cpp` | relays proposals and states |
| `push-server/src/proposals.ts`, `index.ts` | storage, routes, Telegram |
| `tools/pull_proposals.py` | list, pull, mark, undo, keep |
| `tools/telegram_setup.py` | one-time bot setup |
| `tools/import_projector_proposals.py` | applies proposals to the master |

## Tests

```
cd push-server && npm test                                   # 33, 12 of them proposals and Telegram
python tools/test_pull_proposals.py                          # 2
python tests/push/run_tests.py --db <snapshot>               # --proposals: 30 checks
python tests/push/run_tests.py --db <snapshot> --e2e         # + a proposal and its undo through wrangler dev
python tests/proposals/run_import_test.py --scratch <folder> # the importer on a scratch copy of the master, 20 checks
```

The importer test copies the master into the scratch folder (2.4 GB; delete
it afterwards), makes made-up proposals for real songs of the newest
snapshot and runs apply, undo and keep, each through the full gate. It
passed on 2026-11-28 against the master of 2026-11-27 (10,927 live songs)
with snapshot v4: all three runs committed past `verify_db.py
--strict-current`. The scratch copy grows to about 9 GB with its
checkpoints. A run on anything but the master writes its checkpoint and
report beside that copy, never into CGF Lyrics.

## Not covered yet

- Deleting a song on a laptop is not proposed.
- The laptop shows nothing about a proposal's state; the operator's text
  simply stays.
- Other placements of the same song (other songbooks) keep their old version
  until someone repins them.
- Chords for the operator's new version have to be charted again.
- Not tested on the church laptop or with the real bot.

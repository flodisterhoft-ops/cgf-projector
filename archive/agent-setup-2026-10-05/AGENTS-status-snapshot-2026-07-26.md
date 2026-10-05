# CGF Projector status snapshot (2026-07-26)

Moved verbatim from AGENTS.md on 2026-10-05. Historical; see `git log` for what happened since.

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

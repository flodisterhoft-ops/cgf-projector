"""The master importer against a scratch copy of the CGF Lyrics master.

    python tests/proposals/run_import_test.py --scratch <empty folder> [--snapshot v4.sqlite]

Copies the master (SQLite online backup, the master is only read) into the
scratch folder, makes made-up proposals for real songs of the published
snapshot, and runs tools/import_projector_proposals.py three times: apply,
undo, keep again. Each run goes through verify_db.py --strict-current before
its commit (about five minutes each). Delete the scratch folder afterwards:
it holds a full copy of the library.
"""
import argparse
import json
import sqlite3
import subprocess
import sys
import uuid as uuidlib
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOLS = REPO / "tools"
sys.path.insert(0, str(TOOLS))
import import_projector_proposals as imp  # noqa: E402

checks = failures = 0


def check(cond, msg):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
    print(("ok  : " if cond else "FAIL: ") + msg, flush=True)


def ro(path):
    con = sqlite3.connect(f"file:{Path(path).as_posix()}?mode=ro", uri=True)
    con.row_factory = sqlite3.Row
    return con


def proposal(kind, song_id, base, proposed, undo=False, state="received"):
    u = str(uuidlib.uuid4())
    return {"uuid": u, "device": "Test laptop", "kind": kind, "song_id": song_id, "title": proposed["title"],
            "received_at": f"2026-11-28T10:00:{len(made):02d}Z", "updated_at": "", "state": state, "undo": undo,
            "note": None, "notified": True, "laptop_state": None,
            "proposal": {"uuid": u, "kind": kind, "song_id": song_id, "base": base, "proposed": proposed,
                         "created_at": "", "user": "tester", "library_version": 4, "replaces": None}}


made = []


def run(args, inbox, db):
    cmd = [sys.executable, str(TOOLS / "import_projector_proposals.py"), "--lyrics", args.lyrics,
           "--db", str(db), "--inbox", str(inbox), "--no-pull"]
    p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    print(p.stdout[-3000:], p.stderr[-3000:], sep="\n")
    return p.returncode


def write_inbox(inbox, rows):
    inbox.mkdir(parents=True, exist_ok=True)
    for f in inbox.glob("*.json"):
        f.unlink()
    for r in rows:
        (inbox / f"{r['uuid']}.json").write_text(json.dumps(r, ensure_ascii=False), "utf-8")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--scratch", required=True)
    ap.add_argument("--lyrics", default=str(imp.default_lyrics()))
    ap.add_argument("--snapshot", help="published snapshot the laptop has (default: the newest)")
    a = ap.parse_args()
    imp.load_lyrics_tools(a.lyrics)
    lib = Path(a.lyrics) / "library"
    scratch = Path(a.scratch)
    scratch.mkdir(parents=True, exist_ok=True)
    db = scratch / "cgf_lyrics-scratch.sqlite"
    if not db.exists():
        print("copying the master (online backup) ...", flush=True)
        src = ro(lib / "cgf_lyrics.sqlite")
        dst = sqlite3.connect(db)
        src.backup(dst)
        dst.close()
        src.close()
    snap = Path(a.snapshot) if a.snapshot else sorted((lib / "exports" / "publish").glob("spData-cgf-v*.sqlite"),
                                                      key=lambda p: int(p.stem.rsplit("v", 1)[1]))[-1]
    laptop = ro(snap)
    con = ro(db)
    inverse = imp.idmap_inverse()

    # Real songs whose laptop copy matches the master: one shown as primary,
    # one whose entry pins a secondary version.
    picks = {"primary": None, "pinned": None}
    for row in laptop.execute("SELECT * FROM Songs WHERE id <= 500000 AND length(song_text) > 80 ORDER BY id"):
        e = imp.find_entry(con, row["id"], inverse)
        if e is None:
            continue
        vid, v, title, text = imp.shown(con, e)
        if not (imp.same(text, row["song_text"]) and imp.same(title, row["title"])):
            continue
        key = "primary" if vid == imp.primary_of(con, e["song_id"]) else "pinned"
        if picks[key] is None:
            picks[key] = (dict(row), dict(e), vid)
        if all(picks.values()):
            break
    check(all(picks.values()), f"found laptop songs that match the master: {[(k, v and v[0]['id']) for k, v in picks.items()]}")

    def fields(row):
        return {k: row[k] for k in ("title", "songbook_id", "number", "song_text", "category", "tune", "words", "music", "notes")}

    prow, pentry, pvid = picks["primary"]
    srow, sentry, svid = picks["pinned"]
    base_p, base_s = fields(prow), fields(srow)
    edit_p = dict(base_p, title=base_p["title"] + " (test fix)", song_text=base_p["song_text"] + "\nTest fix line")
    edit_s = dict(base_s, song_text=base_s["song_text"].replace("\n", "\nTest inserted line\n", 1))
    # Punctuation only, on top of the operator's first fix.
    punct = dict(edit_p, song_text=edit_p["song_text"] + "!!!")
    stale = dict(base_s, song_text="Something the master never had")
    book = laptop.execute("SELECT id FROM Songbooks ORDER BY id LIMIT 1").fetchone()[0]
    fresh = {"title": "Projector test new song", "songbook_id": book, "number": 0, "song_text": "Verse 1\nA new test song\nWith two lines"}
    rows = [proposal("edit", prow["id"], base_p, edit_p), proposal("edit", srow["id"], base_s, edit_s),
            proposal("new", 500777, None, fresh), proposal("edit", prow["id"], edit_p, punct),
            proposal("edit", srow["id"], dict(base_s, song_text="stale base"), stale),
            proposal("edit", srow["id"], base_s, dict(base_s, title="Undone before import"), undo=True)]
    made.extend(rows)
    rows = []
    for i, r in enumerate(made):
        r["received_at"] = f"2026-11-28T10:00:{i:02d}Z"
        rows.append(r)
    write_inbox(scratch / "inbox", rows)
    primary_before = imp.primary_of(con, pentry["song_id"])
    con.close()

    print("--- apply", flush=True)
    check(run(a, scratch / "inbox", db) == 0, "the import commits (verify_db --strict-current passed)")
    con = ro(db)
    qd = lambda u: json.loads(con.execute("SELECT details FROM review_queue WHERE kind = 'projector_proposal' "
                                         "AND json_extract(details, '$.identity_key') = ?", (f"projector_proposal:{u}",)).fetchone()[0])
    d0 = qd(rows[0]["uuid"])
    new_p = d0["after"]["created_version_id"]
    e = con.execute("SELECT * FROM songbook_entries WHERE id = ?", (pentry["id"],)).fetchone()
    check(e["version_id"] == new_p and imp.primary_of(con, pentry["song_id"]) == new_p,
          "a text fix to the shown primary: a new primary version, pinned to the entry")
    check(con.execute("SELECT is_primary FROM song_versions WHERE id = ?", (primary_before,)).fetchone()[0] == 0,
          "the old version stays, as a secondary one")
    check(con.execute("SELECT canonical_title FROM songs WHERE id = ?", (pentry["song_id"],)).fetchone()[0] == edit_p["title"]
          and con.execute("SELECT 1 FROM song_alt_titles WHERE song_id = ? AND title = ?",
                          (pentry["song_id"], pentry["canonical_title"])).fetchone() is not None,
          "the title changes and the old one stays searchable")
    _, _, title, text = imp.shown(con, imp.find_entry(con, prow["id"], inverse))
    check(imp.same(text, edit_p["song_text"]) and title == edit_p["title"], "the next export gives the laptop's text")
    d1 = qd(rows[1]["uuid"])
    se = con.execute("SELECT * FROM songbook_entries WHERE id = ?", (sentry["id"],)).fetchone()
    check(se["version_id"] == d1["after"]["created_version_id"] and imp.primary_of(con, sentry["song_id"]) != se["version_id"],
          "a fix to a pinned secondary version: a new secondary version, the song's primary unchanged")
    d2 = qd(rows[2]["uuid"])
    s2 = con.execute("SELECT * FROM songs WHERE id = ?", (d2["after"]["song_id"],)).fetchone()
    check(s2["status"] == "imported" and s2["canonical_title"] == fresh["title"]
          and con.execute("SELECT sp_song_id FROM songbook_entries WHERE id = ?", (d2["after"]["entry_id"],)).fetchone()[0] is None,
          "a new song: a new master song in its songbook, its id left to the export")
    check(con.execute("SELECT COUNT(*) FROM song_syllable_sets WHERE song_version_id IN (?,?,?) AND status = 'active'",
                      (new_p, d1["after"]["created_version_id"], d2["after"]["version_id"])).fetchone()[0] == 3,
          "the new versions have syllable sets")
    status = lambda u: con.execute("SELECT status FROM review_queue WHERE json_extract(details, '$.identity_key') = ?",
                                   (f"projector_proposal:{u}",)).fetchone()[0]
    check(qd(rows[3]["uuid"])["resolution"]["state"] == "conflict" and status(rows[3]["uuid"]) == "open",
          "a punctuation-only change waits for Florian (open queue row)")
    check(qd(rows[4]["uuid"])["resolution"]["state"] == "conflict", "a change from a stale base waits for Florian")
    check(qd(rows[5]["uuid"])["resolution"]["state"] == "undone" and status(rows[5]["uuid"]) == "closed",
          "undone before the import: recorded, not applied")
    con.close()

    print("--- undo", flush=True)
    for r in rows[:3]:
        r.update(state="applied", undo=True)
    write_inbox(scratch / "inbox", rows[:3])
    check(run(a, scratch / "inbox", db) == 0, "the undo commits (verify_db passed)")
    con = ro(db)
    e = con.execute("SELECT * FROM songbook_entries WHERE id = ?", (pentry["id"],)).fetchone()
    check(e["version_id"] == pentry["version_id"] and imp.primary_of(con, pentry["song_id"]) == primary_before
          and con.execute("SELECT canonical_title FROM songs WHERE id = ?", (pentry["song_id"],)).fetchone()[0] == pentry["canonical_title"],
          "undo: the entry, the primary and the title are as before")
    check(con.execute("SELECT is_primary FROM song_versions WHERE id = ?", (new_p,)).fetchone()[0] == 0,
          "the operator's version stays, as a secondary one")
    check(con.execute("SELECT version_id FROM songbook_entries WHERE id = ?", (sentry["id"],)).fetchone()[0] == sentry["version_id"],
          "the pinned entry points at its old version again")
    check(con.execute("SELECT status FROM songs WHERE id = ?", (d2["after"]["song_id"],)).fetchone()[0] == "rejected",
          "an undone new song is hidden (rejected), not deleted")
    con.close()

    print("--- keep after all", flush=True)
    for r in rows[:3]:
        r.update(state="undone", undo=False)
    write_inbox(scratch / "inbox", rows[:3])
    check(run(a, scratch / "inbox", db) == 0, "the re-apply commits (verify_db passed)")
    con = ro(db)
    e = con.execute("SELECT * FROM songbook_entries WHERE id = ?", (pentry["id"],)).fetchone()
    check(e["version_id"] == new_p and imp.primary_of(con, pentry["song_id"]) == new_p,
          "keep: the operator's version is back, with no second copy of it")
    check(con.execute("SELECT status FROM songs WHERE id = ?", (d2["after"]["song_id"],)).fetchone()[0] == "imported",
          "the new song is back")
    con.close()
    print(f"{checks} checks, {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())

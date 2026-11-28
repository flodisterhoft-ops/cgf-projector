"""Apply operators' song changes from the church laptops to the CGF Lyrics master.

Florian's decision (2026-11-28): a change an operator makes on a laptop is
accepted by default, and the Telegram message about it has an Undo button.
This tool makes the master match: it pulls the proposals (pull_proposals.py),
applies the new ones, takes back the undone ones and re-applies the ones kept
after an undo. Nothing is deleted. docs/proposals-up.md.

    python tools/import_projector_proposals.py            # pull, apply, report
    python tools/import_projector_proposals.py --dry-run  # everything, then roll back

It follows CGF Lyrics' rules for a master write (its AGENTS.md): one locked
transaction, an online checkpoint taken inside the lock, verify_db.py
--strict-current over the uncommitted state before the commit (about five
minutes), and a report in library/reports/projector-proposals/.

How a change lands:
- A text change becomes a new version of the song, pinned to the laptop's
  songbook entry. It is the song's primary version when the entry showed the
  primary; the old version stays as a secondary one. Chords stay on the old
  version (a chart cannot follow edited words by itself).
- A title change updates the shown version's title; when that is the primary,
  the song's title too, and the old title stays searchable as an alt title.
- A number change moves the entry's number. A songbook move and notes are
  recorded only.
- A change that only touches punctuation or capitals, and a change whose
  starting text is no longer what the master has, is not applied: it gets an
  open review_queue row for Florian, and the laptop keeps showing it.
- A new song becomes a new master song in its songbook; the next export gives
  it its id, and the laptop's own copy gives way to it then.
- Undo puts the entry, the primary flag and the titles back as they were; an
  undone new song is set to rejected (hidden, kept).
Every proposal has one review_queue row (kind projector_proposal) with the
before and after values, so each step can be reversed.
"""
import argparse
import json
import os
import shutil
import sqlite3
import sys
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))


def default_lyrics():
    """CGF Lyrics beside this repository, in the CGF Vernon folder (worktrees included)."""
    if os.environ.get("CGF_LYRICS_DIR"):
        return Path(os.environ["CGF_LYRICS_DIR"])
    for parent in HERE.parents:
        if parent.name == "CGF Vernon":
            return parent / "CGF Lyrics"
    return Path.home() / "Documents" / "AI Projects" / "CGF Vernon" / "CGF Lyrics"


if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

KIND = "projector_proposal"
LIVE = ("imported", "review", "approved")


def load_lyrics_tools(lyrics):
    """CGF Lyrics' own helpers, imported from its library/tools."""
    tools = Path(lyrics) / "library" / "tools"
    if not (tools / "pipeline_safety.py").exists():
        raise SystemExit(f"CGF Lyrics tools not found in {tools}; pass --lyrics")
    sys.path.insert(0, str(tools))
    global cgf_lib, ps, seg, syl, expand_chorus_repeats, IDMAP
    import cgf_lib  # noqa: F401
    import pipeline_safety as ps
    import segment_syllables as seg
    import syllable_lib as syl
    from export_spdata import IDMAP, expand_chorus_repeats


# ---------------------------------------------------------------------------
# Text helpers

def clean(text):
    """Line ends as the master stores them, no trailing spaces, no blank edges."""
    t = str(text or "").replace("\r\n", "\n").replace("\r", "\n")
    return "\n".join(line.rstrip() for line in t.split("\n")).strip("\n").strip()


def same(a, b):
    return clean(a) == clean(b)


def now():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


# ---------------------------------------------------------------------------
# The master: entries, versions, queue rows

def idmap_inverse():
    path = Path(IDMAP)
    if not path.exists():
        return {}
    return {int(sp): int(entry) for entry, sp in json.loads(path.read_text("utf-8")).items()}


def find_entry(con, sp_id, inverse):
    q = ("SELECT e.*, s.status AS song_status, s.canonical_title FROM songbook_entries e "
         "JOIN songs s ON s.id = e.song_id WHERE {} AND s.status NOT IN ('merged','rejected')")
    row = con.execute(q.format("e.sp_song_id = ?"), (sp_id,)).fetchone()
    if row is None and sp_id in inverse:
        row = con.execute(q.format("e.id = ? AND e.sp_song_id IS NULL"), (inverse[sp_id],)).fetchone()
    return row


def primary_of(con, song_id):
    row = con.execute("SELECT id FROM song_versions WHERE song_id = ? AND is_primary = 1", (song_id,)).fetchone()
    return row["id"] if row else None


def version(con, vid):
    return con.execute("SELECT * FROM song_versions WHERE id = ?", (vid,)).fetchone()


def shown(con, entry):
    """What the laptop got for this entry (export_spdata.py's rules)."""
    vid = entry["version_id"] or primary_of(con, entry["song_id"])
    v = version(con, vid)
    title = (v["title"] if entry["version_id"] and v["title"] else entry["canonical_title"]) or ""
    return vid, v, title, expand_chorus_repeats(v["lyrics_raw"] or "")[0]


def queue_row(con, uuid):
    return con.execute("SELECT * FROM review_queue WHERE kind = ? AND json_extract(details, '$.identity_key') = ?",
                       (KIND, f"{KIND}:{uuid}")).fetchone()


def save_queue(con, uuid, details, status, ref_table, ref_id):
    details["identity_key"] = f"{KIND}:{uuid}"
    text = json.dumps(details, ensure_ascii=False, sort_keys=True)
    row = queue_row(con, uuid)
    if row is None:
        con.execute("INSERT INTO review_queue(kind, ref_table, ref_id, details, status) VALUES (?,?,?,?,?)",
                    (KIND, ref_table, ref_id, text, status))
    else:
        con.execute("UPDATE review_queue SET ref_table = ?, ref_id = ?, details = ?, status = ? WHERE id = ?",
                    (ref_table, ref_id, text, status, row["id"]))


def set_primary(con, song_id, vid, touched):
    """Makes vid the song's one primary version; notes every version that changed."""
    for r in con.execute("SELECT id, is_primary FROM song_versions WHERE song_id = ?", (song_id,)).fetchall():
        want = 1 if r["id"] == vid else 0
        if r["is_primary"] != want:
            con.execute("UPDATE song_versions SET is_primary = ? WHERE id = ?", (want, r["id"]))
            touched.add(r["id"])


def add_alt_title(con, song_id, title):
    nt = cgf_lib.norm_title(title)
    if nt:
        con.execute("INSERT OR IGNORE INTO song_alt_titles(song_id, title, norm_title) VALUES (?,?,?)",
                    (song_id, title, nt))


def set_title(con, entry, vid, was_primary, new_title, touched_titles):
    v = version(con, vid)
    if (v["title"] or "") != new_title:
        touched_titles.setdefault(vid, v["title"])
        con.execute("UPDATE song_versions SET title = ? WHERE id = ?", (new_title, vid))
    song = con.execute("SELECT canonical_title FROM songs WHERE id = ?", (entry["song_id"],)).fetchone()
    if was_primary and song["canonical_title"] != new_title:
        add_alt_title(con, entry["song_id"], song["canonical_title"])
        con.execute("UPDATE songs SET canonical_title = ?, updated_at = datetime('now') WHERE id = ?",
                    (new_title, entry["song_id"]))
    if entry["title_as_listed"] and entry["title_as_listed"] != new_title:
        con.execute("UPDATE songbook_entries SET title_as_listed = ? WHERE id = ?", (new_title, entry["id"]))


def insert_version(con, song_id, p, like, uuid):
    raw = clean(p["song_text"])
    lang = (like["language"] if like is not None else None) or cgf_lib.detect_language(raw)
    cur = con.execute(
        "INSERT INTO song_versions(song_id, title, lyrics_raw, lyrics_norm, language, words_by, music_by, "
        "category, tune, notes, source_kind, source_ref, is_primary) VALUES (?,?,?,?,?,?,?,?,?,?,?,?,0)",
        (song_id, str(p.get("title") or "").strip(), raw, cgf_lib.norm_text(raw), lang,
         str(p.get("words") or "") or None, str(p.get("music") or "") or None,
         str(p.get("category") or "") or None, str(p.get("tune") or "") or None, "{}",
         "projector_proposal", f"projector:{uuid}"))
    return cur.lastrowid


# ---------------------------------------------------------------------------
# One proposal

class Conflict(Exception):
    """Not applied: Florian decides (an open review_queue row)."""


def apply_edit(con, prop, entry, ctx, check_base=True):
    p, base = prop["proposed"], prop.get("base") or {}
    vid, v, title, text = shown(con, entry)
    primary = primary_of(con, entry["song_id"])
    if check_base and not (same(base.get("song_text"), text) and same(base.get("title"), title)):
        raise Conflict("the master changed since the laptop's copy")
    before = {"entry_id": entry["id"], "entry_version_id": entry["version_id"], "entry_number": entry["number"],
              "entry_title_as_listed": entry["title_as_listed"], "primary_version_id": primary,
              "shown_version_id": vid, "canonical_title": entry["canonical_title"], "version_titles": {}}
    after = {"song_id": entry["song_id"]}
    was_primary = vid == primary
    target = vid
    meta = {c: str(p.get(k) or "") or None for k, c in (("tune", "tune"), ("words", "words_by"), ("music", "music_by"))
            if str(p.get(k) or "") != str(v[c] or "")}
    if same(p.get("song_text"), text) and meta:
        # Key or authors only: the shown version's details change in place.
        before["version_meta"] = {"version_id": vid, **{c: v[c] for c in meta}}
        con.execute("UPDATE song_versions SET " + ", ".join(f"{c} = ?" for c in meta) + " WHERE id = ?",
                    (*meta.values(), vid))
    elif not same(p.get("song_text"), text):
        norm = cgf_lib.norm_text(clean(p["song_text"]))
        twin = con.execute("SELECT id FROM song_versions WHERE song_id = ? AND lyrics_norm = ?",
                           (entry["song_id"], norm)).fetchone()
        if twin is not None and twin["id"] == vid:
            raise Conflict("only punctuation or capitals changed (the master keeps one version per wording)")
        if twin is not None:
            target = twin["id"]
            after["reused_version_id"] = target
        else:
            target = insert_version(con, entry["song_id"], p, v, prop["uuid"])
            ctx["new_versions"].add(target)
            after["created_version_id"] = target
        con.execute("UPDATE songbook_entries SET version_id = ? WHERE id = ?", (target, entry["id"]))
        if was_primary:
            set_primary(con, entry["song_id"], target, ctx["phase_changed"])
    new_title = str(p.get("title") or "").strip()
    if new_title and new_title != title:
        set_title(con, entry, target, was_primary, new_title, before["version_titles"])
    number = str(p.get("number") if p.get("number") is not None else "")
    if number and number != "0" and number != str(entry["number"] or ""):
        con.execute("UPDATE songbook_entries SET number = ? WHERE id = ?", (number, entry["id"]))
    after["version_id"] = target
    after["recorded_only"] = {k: p.get(k) for k in ("songbook_id", "notes", "category")
                              if str(p.get(k) or "") != str(base.get(k) or "")}
    return before, after


def undo_edit(con, d, ctx):
    b, a = d["before"], d["after"]
    entry = con.execute("SELECT * FROM songbook_entries WHERE id = ?", (b["entry_id"],)).fetchone()
    con.execute("UPDATE songbook_entries SET version_id = ?, number = ?, title_as_listed = ? WHERE id = ?",
                (b["entry_version_id"], b["entry_number"], b["entry_title_as_listed"], entry["id"]))
    if b["primary_version_id"] is not None:
        set_primary(con, entry["song_id"], b["primary_version_id"], ctx["phase_changed"])
    m = dict(b.get("version_meta") or {})
    if m:
        vid = m.pop("version_id")
        con.execute("UPDATE song_versions SET " + ", ".join(f"{c} = ?" for c in m) + " WHERE id = ?", (*m.values(), vid))
    for vid, old in b["version_titles"].items():
        con.execute("UPDATE song_versions SET title = ? WHERE id = ?", (old, int(vid)))
    con.execute("UPDATE songs SET canonical_title = ?, updated_at = datetime('now') WHERE id = ? AND canonical_title <> ?",
                (b["canonical_title"], entry["song_id"], b["canonical_title"]))


def apply_new(con, prop, ctx):
    p = prop["proposed"]
    book = con.execute("SELECT id FROM songbooks WHERE id = ?", (p.get("songbook_id"),)).fetchone()
    if book is None:
        raise Conflict(f"songbook {p.get('songbook_id')} is not in the master")
    title = str(p.get("title") or "").strip()
    raw = clean(p["song_text"])
    lang = cgf_lib.detect_language(raw)
    song = con.execute("INSERT INTO songs(canonical_title, language, status, origin, review_note, updated_at) "
                       "VALUES (?,?,?,?,?, datetime('now'))",
                       (title, lang, "imported", "document", f"{KIND}:{prop['uuid']}")).lastrowid
    vid = insert_version(con, song, p, None, prop["uuid"])
    con.execute("UPDATE song_versions SET is_primary = 1 WHERE id = ?", (vid,))
    number = p.get("number")
    entry = con.execute("INSERT INTO songbook_entries(songbook_id, number, song_id, version_id, title_as_listed) "
                        "VALUES (?,?,?,?,?)", (book["id"], str(number) if number else None, song, vid, title)).lastrowid
    add_alt_title(con, song, title)
    ctx["new_versions"].add(vid)
    return {}, {"song_id": song, "version_id": vid, "entry_id": entry, "created_version_id": vid}


def handle(con, f, ctx):
    """One inbox file; returns the outcome for the report and the server."""
    prop = f["proposal"]
    uuid = f["uuid"]
    want_applied = not f["undo"]
    row = queue_row(con, uuid)
    d = json.loads(row["details"]) if row else None
    base_d = {"uuid": uuid, "device": f["device"], "user": prop.get("user"), "received_at": f["received_at"],
              "laptop_song_id": prop["song_id"], "kind": prop["kind"], "proposed": prop["proposed"],
              "base": prop.get("base"), "replaces": prop.get("replaces")}
    applied = bool(d and d.get("resolution", {}).get("state") == "applied")
    if d and d.get("resolution", {}).get("state") == "conflict" and row["status"] == "open":
        if not want_applied:
            d["resolution"] = {"state": "undone", "at": now(), "reason": "undone before it was applied"}
            save_queue(con, uuid, d, "closed", row["ref_table"], row["ref_id"])
            return {"uuid": uuid, "state": "undone", "note": "undone (it had not been applied)"}
        return {"uuid": uuid, "state": None, "note": "waiting for Florian: " + d["resolution"]["reason"]}

    if want_applied and applied:
        return {"uuid": uuid, "state": "applied", "note": "already in the master"}
    if not want_applied and not applied:
        if d is None:
            d = dict(base_d)
        d.setdefault("history", []).append({"at": now(), "state": "undone", "applied": False})
        d["resolution"] = {"state": "undone", "at": now(), "reason": "undone before it was applied"}
        save_queue(con, uuid, d, "closed", row["ref_table"] if row else None, row["ref_id"] if row else None)
        return {"uuid": uuid, "state": "undone", "note": "not applied (undone first)"}

    if not want_applied:  # applied, now undone
        if prop["kind"] == "new" and not d.get("before"):
            con.execute("UPDATE songs SET status = 'rejected', updated_at = datetime('now') WHERE id = ?",
                        (d["after"]["song_id"],))
            # A hidden song keeps no current syllable set (verify_db); Keep stages a new one.
            con.execute("UPDATE song_syllable_sets SET status = 'superseded' WHERE status IN ('active','skipped') "
                        "AND song_version_id IN (SELECT id FROM song_versions WHERE song_id = ?)",
                        (d["after"]["song_id"],))
        else:
            undo_edit(con, d, ctx)
        d.setdefault("history", []).append({"at": now(), "state": "undone"})
        d["resolution"] = {"state": "undone", "at": now()}
        save_queue(con, uuid, d, "closed", row["ref_table"], row["ref_id"])
        return {"uuid": uuid, "state": "undone", "note": f"master song {d['after']['song_id']} back as before"}

    # Apply (first time, or again after an undo was taken back).
    try:
        if prop["kind"] == "new" and d and d.get("after", {}).get("song_id") and not d.get("before"):
            con.execute("UPDATE songs SET status = 'imported', updated_at = datetime('now') WHERE id = ?",
                        (d["after"]["song_id"],))
            ctx["new_versions"].update(r["id"] for r in con.execute(
                "SELECT id FROM song_versions WHERE song_id = ?", (d["after"]["song_id"],)))
            before, after = {}, d["after"]
        elif prop["kind"] == "new":
            prev = queue_row(con, prop["replaces"]) if prop.get("replaces") else None
            prev_d = json.loads(prev["details"]) if prev else None
            if prev_d and prev_d.get("resolution", {}).get("state") == "applied":
                entry = find_entry_by_id(con, prev_d["after"]["entry_id"])
                before, after = apply_edit(con, prop, entry, ctx, check_base=False)
            else:
                before, after = apply_new(con, prop, ctx)
        else:
            entry = find_entry(con, prop["song_id"], ctx["inverse"])
            if entry is None:
                raise Conflict(f"laptop song {prop['song_id']} is not a placement in the master")
            before, after = apply_edit(con, prop, entry, ctx)
    except Conflict as e:
        d = dict(base_d)
        d["resolution"] = {"state": "conflict", "at": now(), "reason": str(e)}
        save_queue(con, uuid, d, "open", None, None)
        return {"uuid": uuid, "state": None, "note": f"not applied, for you to review: {e}"}
    if d and d.get("before") and not before.get("entry_id"):
        before = d["before"]
    d = d or dict(base_d)
    if not d.get("before"):
        d["before"] = before
    d["after"] = after
    d.setdefault("history", []).append({"at": now(), "state": "applied", "version_id": after.get("version_id")})
    d["resolution"] = {"state": "applied", "at": now()}
    save_queue(con, uuid, d, "closed", "song_versions", after.get("version_id"))
    return {"uuid": uuid, "state": "applied",
            "note": f"master song {after['song_id']}, version {after.get('version_id')}"}


def find_entry_by_id(con, entry_id):
    return con.execute("SELECT e.*, s.status AS song_status, s.canonical_title FROM songbook_entries e "
                       "JOIN songs s ON s.id = e.song_id WHERE e.id = ?", (entry_id,)).fetchone()


# ---------------------------------------------------------------------------
# Syllables (segment_syllables.py's staging, for just the changed versions)

def stage_schema_temp():
    out = []
    for stmt in seg.STAGE_SCHEMA.split(";"):
        s = stmt.strip()
        if not s or s.upper().startswith("PRAGMA"):
            continue
        out.append(s.replace("CREATE TABLE ", "CREATE TEMP TABLE ", 1) + ";")
    return "\n".join(out)


def restage(con, version_ids):
    """New syllable sets for versions that are new or changed phase, inside the transaction."""
    if not version_ids:
        return {}
    syl.ensure_schema(con)
    out = {}
    for phase in (seg.PHASE_PRIMARY, seg.PHASE_SECONDARY):
        records = seg.select_phase_records(con, phase)
        mine = [r for r in records if r["version_id"] in version_ids]
        if not mine:
            continue
        resources = syl.build_resources(records)
        for t in ("staged_syllables", "staged_sets"):
            con.execute(f"DROP TABLE IF EXISTS temp.{t}")
        for stmt in stage_schema_temp().split(";\n"):
            if stmt.strip():
                con.execute(stmt.rstrip(";"))
        for r in mine:
            seg._stage_one(con, phase, r, resources)
        state = seg._current_set_state(con)
        for r in mine:
            con.execute("UPDATE temp.staged_sets SET base_digest = ?, base_phase = ? WHERE version_id = ?",
                        (*state.get(r["version_id"], (None, None)), r["version_id"]))
        if phase == seg.PHASE_SECONDARY:
            seg._assert_secondary_precondition(con)
        seg._assert_no_source_drift(con, "temp", phase)
        ids = [r["version_id"] for r in mine]
        marks = ",".join("?" * len(ids))
        con.execute(f"UPDATE song_syllable_sets SET status = 'superseded' "
                    f"WHERE status IN ('active','skipped') AND song_version_id IN ({marks})", ids)
        con.execute(
            "INSERT INTO song_syllable_sets(song_version_id, lyrics_text, lyrics_sha256, tokenizer_version, "
            "segmenter_version, content_digest, status, review_status, provenance_json) "
            "SELECT version_id, lyrics_text, source_sha256, tokenizer_version, segmenter_version, content_digest, "
            "set_status, review_status, provenance_json FROM temp.staged_sets ORDER BY version_id")
        con.execute(
            "INSERT INTO song_syllable_words(syllable_set_id, line_index, word_index, char_start, char_end, word_text, "
            "normalized_word, language, script, syllables_json, syllable_count, method, model, source_ref, confidence, "
            "review_status, flags_json) "
            "SELECT cur.id, w.line_index, w.word_index, w.char_start, w.char_end, w.word_text, w.normalized_word, "
            "w.language, w.script, w.syllables_json, json_array_length(w.syllables_json), w.method, w.model, "
            "w.source_ref, w.confidence, w.review_status, w.flags_json FROM temp.staged_syllables w "
            "JOIN song_syllable_sets cur ON cur.song_version_id = w.version_id AND cur.status = 'active' "
            "ORDER BY w.version_id, w.ordinal")
        for t in ("staged_syllables", "staged_sets"):
            con.execute(f"DROP TABLE temp.{t}")
        out[phase] = ids
    return out


# ---------------------------------------------------------------------------

def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--lyrics", default=str(default_lyrics()), help="the CGF Lyrics folder")
    ap.add_argument("--db", help="the database to change (default: the CGF Lyrics master)")
    ap.add_argument("--inbox", help="where proposals wait (default: <lyrics>/library/proposals/inbox)")
    ap.add_argument("--no-pull", action="store_true", help="use the inbox as it is; do not call the server")
    ap.add_argument("--dry-run", action="store_true", help="apply and verify, then roll back; nothing is sent")
    args = ap.parse_args(argv)

    load_lyrics_tools(args.lyrics)
    lib = Path(args.lyrics) / "library"
    db = Path(args.db) if args.db else Path(cgf_lib.DB_PATH)
    inbox = Path(args.inbox) if args.inbox else lib / "proposals" / "inbox"
    done_dir = inbox.parent / "done"
    # Reports of a run on another database (a scratch copy) stay beside it,
    # out of the master's audit trail.
    is_master = db.resolve() == Path(cgf_lib.DB_PATH).resolve()
    report_dir = (lib / "reports" if is_master else db.parent / "reports") / "projector-proposals"
    cfg = None
    if not args.no_pull:
        import pull_proposals
        cfg = pull_proposals.load_config()
        pull_proposals.pull(cfg, inbox)
    files = sorted((json.loads(p.read_text("utf-8")) for p in inbox.glob("*.json")),
                   key=lambda f: f["received_at"]) if inbox.exists() else []
    if not files:
        print("No proposals need work.")
        return 0
    print(f"{len(files)} proposal(s) to look at; opening {db}")

    con = ps.connect_for_mutation(db)
    outcomes = []
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    report = report_dir / f"{stamp}{'-dry-run' if args.dry_run else ''}.json"
    pending = None
    try:
        con.execute("BEGIN IMMEDIATE")
        backup = None if args.dry_run else ps.online_backup(db, "projector-proposals")
        ctx = {"inverse": idmap_inverse(), "new_versions": set(), "phase_changed": set()}
        for f in files:
            outcomes.append(handle(con, f, ctx))
            print(f"  {f['uuid']}  {f['device']}: {f['proposal']['proposed'].get('title', '')}: {outcomes[-1]['note']}")
        changed = ctx["new_versions"] | ctx["phase_changed"]
        staged = restage(con, changed)
        if not any(o["state"] for o in outcomes) and not changed:
            con.rollback()
            print("Nothing to write.")
            return 0
        print("Checking the master (verify_db.py --strict-current, a few minutes) ...")
        verification = ps.verify_uncommitted(con)
        payload = {"generated": now(), "dry_run": args.dry_run, "database": str(db),
                   "backup": str(backup) if backup else None, "verification": verification,
                   "syllables_restaged": staged, "outcomes": outcomes}
        if args.dry_run:
            con.rollback()
            report_dir.mkdir(parents=True, exist_ok=True)
            report.write_text(json.dumps(payload, ensure_ascii=False, indent=2), "utf-8")
            print(f"Dry run: verified and rolled back. Report: {report}")
            return 0
        pending = ps.stage_json_report(report, payload)
        ps.commit_with_pending_report(con, pending)
    except Exception as e:
        if con.in_transaction:
            con.rollback()
        print("ROLLED BACK - no changes written")
        if isinstance(e, ps.VerificationError):
            # What the gate found, so the cause is visible without a rerun.
            print(json.dumps(e.result, ensure_ascii=False, indent=2, default=str)[-6000:])
        raise
    finally:
        con.close()
    ps.finalize_json_report(pending, report)
    print(f"Committed. Report: {report}")

    # Tell the server (and through it the laptops) what the master has now.
    if cfg is not None:
        from pull_proposals import request
        done_dir.mkdir(parents=True, exist_ok=True)
        for o in outcomes:
            body = {"note": o["note"][:500]}
            if o["state"]:
                body["state"] = o["state"]
            request(cfg, "POST", f"/proposals/{o['uuid']}", body)
            if o["state"]:
                src = inbox / f"{o['uuid']}.json"
                if src.exists():
                    shutil.move(str(src), str(done_dir / f"{stamp}-{o['uuid']}.json"))
    return 0


if __name__ == "__main__":
    sys.exit(main())

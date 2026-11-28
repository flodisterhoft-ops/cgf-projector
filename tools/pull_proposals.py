"""Operators' song changes from the push server (docs/proposals-up.md).

Church laptops send each song an operator adds or changes as a proposal; the
push server keeps them and Florian's Undo choice from Telegram. This tool
brings them to Florian's PC, where CGF Lyrics'
library/tools/import_projector_proposals.py applies them to the master (it
runs `pull` and `mark` itself).

Commands:
  list [--all]                 proposals on the server (default: those needing work)
  pull --inbox DIR             write each proposal needing work to DIR/<uuid>.json
  mark UUID --state S [--note] tell the server what the master now has
                               (received, applied or undone)
  undo UUID / keep UUID        the same as the Telegram buttons

A proposal needs work when it is new, or when the master does not match
Florian's choice yet (applied but undone since, or undone but kept since).
Settings and the publisher token: push_release.py (%USERPROFILE%\\.cgf-publish).
"""
import argparse
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from push_release import load_config, request  # noqa: E402

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")


def needs_work(p):
    state, undo = p["state"], p["undo"]
    if state in ("new", "received"):
        return True
    if state == "applied" and undo:
        return True
    if state == "undone" and not undo:
        return True
    return False


def fetch(cfg):
    return request(cfg, "GET", "/proposals")["proposals"]


def line(p):
    title = p["proposal"]["proposed"].get("title", "")
    flag = " (undo)" if p["undo"] else ""
    return f"{p['uuid']}  {p['received_at'][:16]}  {p['state']:<9}{flag:<7} {p['kind']:<4} song {p['song_id']:<7} {p['device']}: {title}"


def cmd_list(args):
    rows = fetch(load_config())
    rows = rows if args.all else [p for p in rows if needs_work(p)]
    for p in rows:
        print(line(p))
    print(f"{len(rows)} proposal(s)")


def write_atomic(path, data):
    tmp = path.with_suffix(".json.partial")
    tmp.write_text(json.dumps(data, ensure_ascii=False, indent=2), "utf-8")
    os.replace(tmp, path)


def pull(cfg, inbox):
    """Writes every proposal needing work; marks new ones received. Returns them."""
    inbox = Path(inbox)
    inbox.mkdir(parents=True, exist_ok=True)
    out = []
    for p in fetch(cfg):
        if not needs_work(p):
            continue
        if p["state"] == "new":
            p = request(cfg, "POST", f"/proposals/{p['uuid']}", {"state": "received"})
        write_atomic(inbox / f"{p['uuid']}.json", p)
        out.append(p)
    return out


def cmd_pull(args):
    got = pull(load_config(), args.inbox)
    for p in got:
        print(line(p))
    print(f"{len(got)} proposal(s) in {args.inbox}")


def cmd_mark(args):
    body = {"state": args.state}
    if args.note:
        body["note"] = args.note
    p = request(load_config(), "POST", f"/proposals/{args.uuid}", body)
    print(line(p))


def cmd_undo(args, undo):
    p = request(load_config(), "POST", f"/proposals/{args.uuid}", {"undo": undo})
    print(line(p))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("list"); p.add_argument("--all", action="store_true"); p.set_defaults(fn=cmd_list)
    p = sub.add_parser("pull"); p.add_argument("--inbox", required=True); p.set_defaults(fn=cmd_pull)
    p = sub.add_parser("mark"); p.add_argument("uuid")
    p.add_argument("--state", required=True, choices=["received", "applied", "undone"])
    p.add_argument("--note"); p.set_defaults(fn=cmd_mark)
    p = sub.add_parser("undo"); p.add_argument("uuid"); p.set_defaults(fn=lambda a: cmd_undo(a, True))
    p = sub.add_parser("keep"); p.add_argument("uuid"); p.set_defaults(fn=lambda a: cmd_undo(a, False))
    args = ap.parse_args(argv)
    args.fn(args)


if __name__ == "__main__":
    main()

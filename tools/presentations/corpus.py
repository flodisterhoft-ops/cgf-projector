#!/usr/bin/env python3
"""Runs the presentation step splitter over many real files, to find the ones
it gets wrong before an operator does (BUILD.md, "Presentation steps").

    python tools/presentations/corpus.py <file, folder or @list.txt> ...
            [--render powerpoint|libreoffice|both] [--soffice <soffice.exe>]
            [--limit N] [--only-steps] [--every N] [--keep <folder>] [--per-file]
            [--qt <Qt folder>]

For every .pptx .pptm .ppsx it runs stepstool (the app's own splitter,
src/sources/presentationsteps.cpp; build it first, see stepstool.pro) and
counts the stills and effects. With --render it also has PowerPoint or
LibreOffice draw the rewritten file through the app's import script and
checks that as many pictures come out as there are steps: a rewritten file
the office program refuses, or draws a different number of slides from, is
the failure this is here to catch. --keep leaves each rewritten file and its
pictures in a folder for a look.

Files that live only in the cloud are skipped, nothing is changed, and the
report has no file names unless --per-file is given.
"""

import argparse
import collections
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
sys.path.insert(0, str(HERE))
import survey  # noqa: E402  (collect, cloud_only)

TOOL = HERE / "stepstool" / "out" / "bin" / "stepstool.exe"
SCRIPT = REPO / "src" / "scripts" / "import-presentation.ps1"


def render(engine, expanded, out, soffice, profile):
    powershell = pathlib.Path(os.environ["SystemRoot"]) / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe"
    command = [str(powershell), "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", str(SCRIPT),
               "-In", str(expanded), "-Out", str(out), "-Size", "640", "-Engine", engine]
    if engine == "libreoffice":
        command += ["-Soffice", str(soffice), "-Profile", str(profile)]
    try:
        done = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace", timeout=900)
    except subprocess.TimeoutExpired:
        return -1, "took more than 15 minutes"
    pictures = len(list(pathlib.Path(out).glob("slide*.png")))
    return pictures, done.stderr.strip().splitlines()[-1] if done.returncode != 0 and done.stderr.strip() else ""


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--render", choices=("powerpoint", "libreoffice", "both"))
    ap.add_argument("--soffice", help="LibreOffice's soffice.exe (for --render libreoffice)")
    ap.add_argument("--limit", type=int, default=0, help="stop after this many files")
    ap.add_argument("--only-steps", action="store_true", help="draw only the files that got more stills than slides")
    ap.add_argument("--every", type=int, default=1, help="of the other files, draw only every Nth")
    ap.add_argument("--keep", help="leave the rewritten files and their pictures here")
    ap.add_argument("--per-file", action="store_true", help="name the files that failed (shows file names)")
    ap.add_argument("--qt", default=str(pathlib.Path.home() / "Qt"), help="aqt install folder (default ~/Qt)")
    args = ap.parse_args()
    if not TOOL.is_file():
        sys.exit(f"Build the step tool first: qmake and mingw32-make in {TOOL.parent.parent.parent}")
    engines = {"both": ["powerpoint", "libreoffice"], None: []}.get(args.render, [args.render])
    if "libreoffice" in engines and not (args.soffice and pathlib.Path(args.soffice).is_file()):
        sys.exit("--render libreoffice needs --soffice <soffice.exe>")

    env = dict(os.environ)
    qt = pathlib.Path(args.qt)
    env["PATH"] = os.pathsep.join([str(qt / "6.8.3" / "mingw_64" / "bin"), str(qt / "Tools" / "mingw1310_64" / "bin"),
                                   env.get("PATH", "")])
    stats = collections.Counter()
    problems = []
    slowest = (0.0, 0)
    work = pathlib.Path(args.keep) if args.keep else pathlib.Path(tempfile.mkdtemp(prefix="cgf-corpus-"))
    work.mkdir(parents=True, exist_ok=True)
    profile = work / "libreoffice-profile"
    done = 0
    try:
        for path in survey.collect(args.paths):
            ext = os.path.splitext(path)[1].lower()
            if ext in survey.OLD:
                stats["old-format files, not opened"] += 1
                continue
            if ext not in survey.NEW:
                continue
            if survey.cloud_only(path):
                stats["skipped: only in the cloud"] += 1
                continue
            if args.limit and done >= args.limit:
                break
            done += 1
            folder = work / ("deck%04d" % done)
            shutil.rmtree(folder, ignore_errors=True)
            folder.mkdir()
            expanded = folder / "steps.pptx"
            started = time.time()
            try:
                run = subprocess.run([str(TOOL), path, str(expanded), str(folder / "media")], capture_output=True,
                                     text=True, encoding="utf-8", errors="replace", env=env, timeout=600)
            except subprocess.TimeoutExpired:
                stats["splitter: took more than 10 minutes"] += 1
                problems.append((path, "splitter timed out"))
                continue
            took = time.time() - started
            if run.returncode != 0:
                reason = run.stderr.strip() or "exit code %d" % run.returncode
                stats["splitter refused: " + reason[:60]] += 1
                problems.append((path, "splitter: " + reason))
                continue
            steps = [json.loads(line[len("cgf-step:"):]) for line in run.stdout.splitlines() if line.startswith("cgf-step:")]
            if took > slowest[0]:
                slowest = (took, os.path.getsize(path))
            stats["decks split"] += 1
            stats["slides"] += sum(1 for s in steps if s.get("i", 0) == 0)
            stats["stills"] += len(steps)
            stats["decks with steps beyond one per slide"] += any(s.get("i", 0) > 0 for s in steps)
            for s in steps:
                kind = "transition" if s.get("i", 0) == 0 else "leaves" if s.get("x") else "appears"
                stats["%s: %s%s" % (kind, s.get("e", "none (the usual fade)"), " " + s["r"] if s.get("r") else "")] += 1
                if s.get("a"):
                    stats["steps that follow by themselves"] += 1
                for m in s.get("m", []):
                    if m["k"] != "gif" or s.get("i", 0) == 0:
                        stats["media started: " + m["k"]] += 1
            plain = not any(s.get("i", 0) > 0 for s in steps)
            stats["decks without steps"] += plain
            if plain and (args.only_steps or stats["decks without steps"] % args.every):
                if not args.keep:
                    shutil.rmtree(folder, ignore_errors=True)
                continue
            for engine in engines:
                out = folder / engine
                out.mkdir()
                pictures, message = render(engine, expanded, out, args.soffice, profile)
                if pictures == len(steps):
                    stats[engine + ": drew every still"] += 1
                else:
                    stats[engine + ": WRONG number of pictures"] += 1
                    problems.append((path, "%s drew %d pictures for %d stills. %s" % (engine, pictures, len(steps), message)))
            if not args.keep:
                shutil.rmtree(folder, ignore_errors=True)
    finally:
        if not args.keep:
            shutil.rmtree(work, ignore_errors=True)

    for key, count in sorted(stats.items(), key=lambda kv: (-kv[1], kv[0])):
        print("  %6d  %s" % (count, key))
    print("  slowest split: %.1f s for a file of %.0f MB" % (slowest[0], slowest[1] / 1e6))
    print("  problems: %d" % len(problems))
    if args.per_file:
        for path, why in problems:
            print("    %s: %s" % (path, why))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())

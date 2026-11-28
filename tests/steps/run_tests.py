#!/usr/bin/env python3
"""Build and run the presentation-step checks (BUILD.md, "Presentation steps").

    python tests/steps/run_tests.py --db <CGF database> [--video <short video>]
                                    [--soffice <LibreOffice's soffice.exe>]

PowerPoint on this computer makes the deck (make_deck.ps1) and draws its
stills. With --video the deck gets a slide with that video (any short .mp4);
with --soffice the import is also run through LibreOffice. The database (for
example CGF Lyrics' library/exports/publish/spData-cgf-v3.sqlite) is copied
into tests/steps/out/bin first; the original is only read. The main window
runs on Qt's offscreen platform; sounds and the video really play, muted.
It leaves frame-*.png beside the test exe: the projector in the middle of a
push, a wipe and a fly-in. Uses the Qt 6.8.3 / MinGW toolchain from BUILD.md.
"""

import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
OUT = HERE / "out"


def project_lists():
    """The app's own file lists from src/softProjector.pro, minus main.cpp."""
    pro = (REPO / "src" / "softProjector.pro").read_text(encoding="utf-8")
    pro = re.sub(r"\\\s*\n", " ", pro)
    lists = {key: [] for key in ("QT", "SOURCES", "HEADERS", "FORMS", "RESOURCES", "LIBS")}
    for line in pro.splitlines():
        m = re.match(r"\s*(?:win32:\s*)?(QT|SOURCES|HEADERS|FORMS|RESOURCES|LIBS)\s*\+=\s*(.*)", line)
        if m:
            lists[m.group(1)] += m.group(2).split()
    lists["SOURCES"].remove("sources/main.cpp")
    return lists


def write_pro():
    # Relative paths: qmake mishandles absolute ones with spaces ("AI Projects").
    src = "../../../src"
    lists = project_lists()
    lines = [
        "QT += gui-private " + " ".join(lists.pop("QT")),
        "LIBS += " + " ".join(lists.pop("LIBS")),
        "TARGET = steps_tests",
        "TEMPLATE = app",
        "CONFIG += console",
        f"INCLUDEPATH += {src}/headers",
        "DESTDIR = bin",
        "OBJECTS_DIR = obj",
        "MOC_DIR = moc",
        "UI_DIR = ui",
        "RCC_DIR = rcc",
        "SOURCES += ../test_steps.cpp",
    ]
    for key, files in lists.items():
        lines += [f"{key} += {src}/{f}" for f in files]
    (OUT / "steps_tests.pro").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", required=True, help="CGF database to copy and test against")
    ap.add_argument("--video", help="a short video for the deck's video slide")
    ap.add_argument("--soffice", help="LibreOffice's soffice.exe, to run the import through it too")
    ap.add_argument("--qt", default=str(pathlib.Path.home() / "Qt"), help="aqt install folder (default ~/Qt)")
    args = ap.parse_args()

    qt = pathlib.Path(args.qt)
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join([str(qt / "Tools" / "mingw1310_64" / "bin"),
                                   str(qt / "6.8.3" / "mingw_64" / "bin"), env.get("PATH", "")])
    qmake = shutil.which("qmake", path=env["PATH"])
    make = shutil.which("mingw32-make", path=env["PATH"])
    if not qmake or not make:
        sys.exit(f"Qt or MinGW not found under {qt} (see BUILD.md); pass --qt")
    (OUT / "bin").mkdir(parents=True, exist_ok=True)
    write_pro()
    subprocess.run([qmake, "steps_tests.pro", "-spec", "win32-g++"], cwd=OUT, env=env, check=True)
    subprocess.run([make, "-f", "Makefile.Release", f"-j{os.cpu_count() or 4}"],
                   cwd=OUT, env=env, check=True, stdout=subprocess.DEVNULL)
    shutil.copyfile(args.db, OUT / "bin" / "spData.sqlite")

    # The deck, made by PowerPoint.
    bin_dir = OUT / "bin"
    windows = pathlib.Path(os.environ["SystemRoot"])
    powershell = windows / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe"
    command = [str(powershell), "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(HERE / "make_deck.ps1"),
               "-Out", str(bin_dir / "steps-deck.pptx"), "-Old", str(bin_dir / "steps-deck.ppt"),
               "-Picture", str(REPO / "src" / "icons" / "splash.png"), "-Sound", str(windows / "Media" / "chimes.wav"),
               "-Gif", str(bin_dir / "two-frames.gif")]
    if args.video:
        command += ["-Video", str(pathlib.Path(args.video).resolve())]
    deck = subprocess.run(command, capture_output=True, text=True)
    if deck.returncode != 0 or not (bin_dir / "steps-deck.pptx").is_file():
        sys.exit("PowerPoint could not make the deck: " + deck.stdout + deck.stderr)

    # One 1080p screen. The path must be relative (a "C:" in it ends the
    # platform argument), so the checks run in OUT.
    config = {"screens": [{"name": "Laptop", "x": 0, "y": 0, "width": 1920, "height": 1080,
                           "logicalDpi": 96, "logicalBaseDpi": 96, "dpr": 1}]}
    (OUT / "screens.json").write_text(json.dumps(config), encoding="utf-8")
    env["QT_QPA_PLATFORM"] = "offscreen:configfile=screens.json"
    env["QT_QPA_FONTDIR"] = "C:/Windows/Fonts"
    env["QT_FORCE_STDERR_LOGGING"] = "1"
    env["CGF_TEST_HAS_VIDEO"] = "1" if args.video else "0"
    for var in ("CGF_LIBREOFFICE", "CGF_IMPORT_ENGINE", "CGF_IMPORT_STEPS"):
        env.pop(var, None)
    if args.soffice:
        env["CGF_LIBREOFFICE"] = str(pathlib.Path(args.soffice).resolve())
    exe = OUT / "bin" / "steps_tests.exe"
    result = subprocess.run([str(exe)], cwd=OUT, env=env, capture_output=True,
                            text=True, encoding="utf-8", errors="replace")
    for line in result.stderr.splitlines():
        if line.startswith(("ok  :", "FAIL:")) or " checks, " in line:
            print(line)
    if result.returncode not in (0, 1):
        print(result.stderr[-3000:])
    sys.exit(result.returncode)


if __name__ == "__main__":
    main()

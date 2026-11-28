#!/usr/bin/env python3
"""Build and run the PowerPoint-import checks (BUILD.md, "LibreOffice for
PowerPoint files").

    python tests/presentations/run_tests.py --db <CGF database> --msi <LibreOffice msi>

--msi is the LibreOffice installer the companion is built for (the version
and SHA-256 in src/companion/fetchoffice.cpp), downloaded once from
download.documentfoundation.org. The checks serve it from this computer, so
the companion's download, its SHA-256 test and the unpacking run for real
without the internet. The database (for example CGF Lyrics'
library/exports/publish/spData-cgf-v3.sqlite) is copied into
tests/presentations/out/bin; the original is only read.

Needs PowerPoint on this computer (it makes the test deck, and the PowerPoint
path is checked too), no LibreOffice installed for everyone (the checks start
as a computer without one), the built companion (src/win32_build/bin, see
BUILD.md) and the Qt 6.8.3 / MinGW toolchain. Takes about three minutes; the
unpacked LibreOffice (about 1.1 GB) stays in out/bin until the next run.

It runs from any checkout. From one deep in the file system (a worktree with
a long name) LibreOffice's settings go to the temporary folder, as they do in
the program; the checks say so in the line about the settings.
"""

import argparse
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import time
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
OUT = HERE / "out"
PORT = 8791


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
        "TARGET = presentations_tests",
        "TEMPLATE = app",
        "CONFIG += console",
        f"INCLUDEPATH += {src}/headers",
        "DESTDIR = bin",
        "OBJECTS_DIR = obj",
        "MOC_DIR = moc",
        "UI_DIR = ui",
        "RCC_DIR = rcc",
        "SOURCES += ../test_presentations.cpp",
    ]
    for key, files in lists.items():
        lines += [f"{key} += {src}/{f}" for f in files]
    (OUT / "presentations_tests.pro").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", required=True, help="CGF database to copy and test against")
    ap.add_argument("--msi", required=True, help="the LibreOffice installer the companion expects")
    ap.add_argument("--qt", default=str(pathlib.Path.home() / "Qt"), help="aqt install folder (default ~/Qt)")
    args = ap.parse_args()

    msi = pathlib.Path(args.msi)
    if not msi.is_file():
        sys.exit(f"No LibreOffice installer at {msi}")
    companion = REPO / "src" / "win32_build" / "bin" / "cgf-companion.exe"
    if not companion.is_file():
        sys.exit(f"No {companion}: build src/companion first (BUILD.md)")
    for var in ("ProgramFiles", "ProgramFiles(x86)"):
        if os.environ.get(var) and (pathlib.Path(os.environ[var]) / "LibreOffice" / "program" / "soffice.exe").is_file():
            sys.exit("LibreOffice is installed on this computer; these checks need one without it")

    qt = pathlib.Path(args.qt)
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join([str(qt / "Tools" / "mingw1310_64" / "bin"),
                                   str(qt / "6.8.3" / "mingw_64" / "bin"), env.get("PATH", "")])
    qmake = shutil.which("qmake", path=env["PATH"])
    make = shutil.which("mingw32-make", path=env["PATH"])
    if not qmake or not make:
        sys.exit(f"Qt or MinGW not found under {qt} (see BUILD.md); pass --qt")
    bin_dir = OUT / "bin"
    bin_dir.mkdir(parents=True, exist_ok=True)
    write_pro()
    subprocess.run([qmake, "presentations_tests.pro", "-spec", "win32-g++"], cwd=OUT, env=env, check=True)
    subprocess.run([make, "-f", "Makefile.Release", f"-j{os.cpu_count() or 4}"],
                   cwd=OUT, env=env, check=True, stdout=subprocess.DEVNULL)

    # A laptop that has never had LibreOffice.
    for name in ("libreoffice", "libreoffice.new", "libreoffice-download", "libreoffice-profile"):
        shutil.rmtree(bin_dir / name, ignore_errors=True)
    shutil.copyfile(args.db, bin_dir / "spData.sqlite")
    shutil.copyfile(companion, bin_dir / "cgf-companion.exe")

    # The test deck and its PDF, made by PowerPoint.
    powershell = pathlib.Path(os.environ["SystemRoot"]) / "System32" / "WindowsPowerShell" / "v1.0" / "powershell.exe"
    deck = subprocess.run([str(powershell), "-NoProfile", "-ExecutionPolicy", "Bypass", "-File", str(HERE / "make_deck.ps1"),
                           "-Out", str(bin_dir / "test-deck.pptx"), "-Pdf", str(bin_dir / "test-deck.pdf"),
                           "-Picture", str(REPO / "src" / "icons" / "splash.png")],
                          capture_output=True, text=True)
    if deck.returncode != 0 or not (bin_dir / "test-deck.pptx").is_file():
        sys.exit("PowerPoint could not make the test deck:\n" + deck.stdout + deck.stderr)

    # What the companion downloads, served from this computer: the real
    # installer, and a file that is not it.
    served = OUT / "served"
    shutil.rmtree(served, ignore_errors=True)
    served.mkdir()
    try:
        os.link(msi, served / "LibreOffice.msi")
    except OSError:
        shutil.copyfile(msi, served / "LibreOffice.msi")
    (served / "not-libreoffice.msi").write_bytes(b"not LibreOffice " * 4096)
    server = subprocess.Popen([sys.executable, "-m", "http.server", str(PORT), "--bind", "127.0.0.1",
                               "--directory", str(served)], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(50):
            try:
                urllib.request.urlopen(f"http://127.0.0.1:{PORT}/not-libreoffice.msi", timeout=2).close()
                break
            except OSError:
                time.sleep(0.2)
        else:
            sys.exit(f"the download server did not start on port {PORT}")

        # One 1080p screen. The path must be relative (a "C:" in it ends the
        # platform argument), so the checks run in OUT.
        config = {"screens": [{"name": "Laptop", "x": 0, "y": 0, "width": 1920, "height": 1080,
                               "logicalDpi": 96, "logicalBaseDpi": 96, "dpr": 1}]}
        (OUT / "screens.json").write_text(json.dumps(config), encoding="utf-8")
        env["QT_QPA_PLATFORM"] = "offscreen:configfile=screens.json"
        env["QT_QPA_FONTDIR"] = "C:/Windows/Fonts"
        env["QT_FORCE_STDERR_LOGGING"] = "1"
        env["CGF_TEST_OFFICE_URL"] = f"http://127.0.0.1:{PORT}/LibreOffice.msi"
        for var in ("CGF_LIBREOFFICE", "CGF_LIBREOFFICE_URL", "CGF_IMPORT_ENGINE"):
            env.pop(var, None)
        exe = bin_dir / "presentations_tests.exe"
        result = subprocess.run([str(exe)], cwd=OUT, env=env, capture_output=True,
                                text=True, encoding="utf-8", errors="replace")
    finally:
        server.kill()
    for line in result.stderr.splitlines():
        if line.startswith(("ok  :", "FAIL:")) or " checks, " in line:
            print(line)
    if result.returncode not in (0, 1):
        print(result.stderr[-3000:])
    sys.exit(result.returncode)


if __name__ == "__main__":
    main()

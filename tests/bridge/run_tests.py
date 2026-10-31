#!/usr/bin/env python3
"""Build and run the network-bridge checks against a copy of a CGF database.

    python tests/bridge/run_tests.py --db C:/Users/<you>/CGF/CGFProjector/spData.sqlite
    python tests/bridge/run_tests.py --db ... --serve     # keep serving for a browser

The database is copied into tests/bridge/out/bin first; the original is only
read. Uses the Qt 6.8.3 / MinGW toolchain from BUILD.md. The bridge listens on
this computer only (CGF_BRIDGE_LOCAL), port 8779, so no firewall prompt.
"""

import argparse
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
    lists = {key: [] for key in ("QT", "SOURCES", "HEADERS", "FORMS", "RESOURCES")}
    for line in pro.splitlines():
        m = re.match(r"\s*(QT|SOURCES|HEADERS|FORMS|RESOURCES)\s*\+=\s*(.*)", line)
        if m:
            lists[m.group(1)] += m.group(2).split()
    lists["SOURCES"].remove("sources/main.cpp")
    return lists


def write_pro():
    # Relative paths: qmake mishandles absolute ones with spaces ("AI Projects").
    src = "../../../src"
    lists = project_lists()
    lines = [
        "QT += " + " ".join(lists.pop("QT")),
        "TARGET = bridge_tests",
        "TEMPLATE = app",
        "CONFIG += console",
        f"INCLUDEPATH += {src}/headers",
        "DESTDIR = bin",
        "OBJECTS_DIR = obj",
        "MOC_DIR = moc",
        "UI_DIR = ui",
        "RCC_DIR = rcc",
        "SOURCES += ../test_bridge.cpp",
    ]
    for key, files in lists.items():
        lines += [f"{key} += {src}/{f}" for f in files]
    (OUT / "bridge_tests.pro").write_text("\n".join(lines) + "\n", encoding="utf-8")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", required=True, help="CGF database to copy and test against")
    ap.add_argument("--qt", default=str(pathlib.Path.home() / "Qt"), help="aqt install folder (default ~/Qt)")
    ap.add_argument("--serve", action="store_true", help="keep serving instead of running the checks")
    args = ap.parse_args()

    qt = pathlib.Path(args.qt)
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join([str(qt / "Tools" / "mingw1310_64" / "bin"),
                                   str(qt / "6.8.3" / "mingw_64" / "bin"), env.get("PATH", "")])
    # Windows finds a program on the caller's PATH, not env's: look it up here.
    qmake = shutil.which("qmake", path=env["PATH"])
    make = shutil.which("mingw32-make", path=env["PATH"])
    if not qmake or not make:
        sys.exit(f"Qt or MinGW not found under {qt} (see BUILD.md); pass --qt")
    (OUT / "bin").mkdir(parents=True, exist_ok=True)
    write_pro()
    subprocess.run([qmake, "bridge_tests.pro", "-spec", "win32-g++"], cwd=OUT, env=env, check=True)
    subprocess.run([make, "-f", "Makefile.Release", f"-j{os.cpu_count() or 4}"],
                   cwd=OUT, env=env, check=True, stdout=subprocess.DEVNULL)
    shutil.copyfile(args.db, OUT / "bin" / "spData.sqlite")

    env["QT_FORCE_STDERR_LOGGING"] = "1"
    exe = OUT / "bin" / "bridge_tests.exe"
    if args.serve:
        sys.exit(subprocess.run([str(exe), "--serve"], cwd=OUT, env=env).returncode)
    result = subprocess.run([str(exe)], cwd=OUT, env=env, capture_output=True,
                            text=True, encoding="utf-8", errors="replace")
    for line in result.stderr.splitlines():
        if line.startswith(("ok  :", "FAIL:")) or " checks, " in line:
            print(line)
    sys.exit(result.returncode)


if __name__ == "__main__":
    main()

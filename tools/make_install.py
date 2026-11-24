"""Make the CGF Projector install package for a church laptop (docs/song-push.md).

  python tools/make_install.py --version 0.2.0 --out <folder>
        [--build-dir src/win32_build/bin] [--launcher src/win32_build/launcher/CGFProjector.exe]
        [--snapshot <spData-cgf-vN.sqlite>] [--device-file <cgf-cloud.json>] [--seq N] [--notes TEXT]

Writes <out>/CGFProjector-<version>.zip with install.cmd, install.ps1 and a
CGFProjector folder: the launcher, app-<version> (the deployed build),
cgf-versions.txt and, when given, the song library and the laptop's
cgf-cloud.json (from push_release.py add-device). The zip then holds that
laptop's device token: keep it private, and remove the device
(push_release.py remove-device) if the zip goes astray.

--seq is the app release number of this version on the push server
(push_release.py app-release prints it), so the laptop does not download the
same version again. Without it the laptop takes the server's current release.
"""
import argparse
import json
import re
import shutil
import sqlite3
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import push_release as pr  # noqa: E402

REPO = Path(__file__).resolve().parent.parent


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--version", required=True)
    ap.add_argument("--out", required=True, help="folder for the zip (keep it outside the repository)")
    ap.add_argument("--build-dir", default=str(REPO / "src" / "win32_build" / "bin"))
    ap.add_argument("--launcher", default=str(REPO / "src" / "win32_build" / "launcher" / "CGFProjector.exe"))
    ap.add_argument("--snapshot", help="published library snapshot to start the laptop with")
    ap.add_argument("--device-file", help="the laptop's cgf-cloud.json from push_release.py add-device")
    ap.add_argument("--seq", type=int, default=0, help="this version's app release number on the server")
    ap.add_argument("--notes", default="")
    ap.add_argument("--keep-folder", action="store_true", help="also leave the unzipped package (tests)")
    args = ap.parse_args(argv)

    folder = f"app-{args.version}"
    if not pr.APP_FOLDER.match(folder):
        raise SystemExit("A version may hold letters, digits, dots, dashes and underscores only")
    build = Path(args.build_dir)
    for r in pr.REQUIRED:
        if not (build / r).exists():
            raise SystemExit(f"{build} has no {r}: build and deploy first (BUILD.md)")
    if not Path(args.launcher).exists():
        raise SystemExit(f"No launcher at {args.launcher}: build src/launcher (BUILD.md)")

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(dir=out) as tmp:
        pkg = Path(tmp) / f"CGFProjector-{args.version}"
        root = pkg / "CGFProjector"
        app = root / folder
        app.mkdir(parents=True)
        for rel, path in pr.app_files(build):
            dest = app / rel
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, dest)
        (app / "cgf-release.json").write_text(json.dumps(
            {"version": args.version, "folder": folder, "seq": args.seq, "notes": args.notes}, indent=2), "utf-8")
        shutil.copy2(args.launcher, root / "CGFProjector.exe")
        (root / "cgf-versions.txt").write_text(
            f"current={folder}\nprevious=\nseq={args.seq}\ntrial=0\nstarts=0\nbad=\n", "utf-8")

        if args.snapshot:
            snap = Path(args.snapshot)
            m = re.search(r"-v(\d+)\.sqlite$", snap.name)
            if not m:
                raise SystemExit("--snapshot must be a published snapshot (spData-cgf-v<N>.sqlite)")
            shutil.copy2(snap, root / "spData.sqlite")
            (root / "version.txt").write_text(m.group(1) + "\n", "utf-8")
            con = sqlite3.connect(root / "spData.sqlite")
            try:
                if con.execute("PRAGMA integrity_check").fetchone()[0] != "ok":
                    raise SystemExit("The snapshot fails SQLite's integrity check")
            finally:
                con.close()
        if args.device_file:
            cloud = json.loads(Path(args.device_file).read_text("utf-8"))
            if not cloud.get("server") or not cloud.get("token") or not cloud.get("publisherKeys"):
                raise SystemExit("--device-file is not a cgf-cloud.json from push_release.py add-device")
            shutil.copy2(args.device_file, root / "cgf-cloud.json")

        for name in ("install.ps1", "install.cmd"):
            shutil.copy2(REPO / "tools" / "install" / name, pkg / name)

        zip_base = out / f"CGFProjector-{args.version}"
        archive = shutil.make_archive(str(zip_base), "zip", root_dir=pkg.parent, base_dir=pkg.name)
        if args.device_file:
            pr.restrict_to_user(Path(archive))
        if args.keep_folder:
            kept = out / pkg.name
            shutil.rmtree(kept, ignore_errors=True)
            shutil.copytree(pkg, kept)
    size = Path(archive).stat().st_size / 1e6
    print(f"PACKAGE OK: {archive} ({size:.0f} MB)"
          + ("; it holds the laptop's device token" if args.device_file else ""))


if __name__ == "__main__":
    main()

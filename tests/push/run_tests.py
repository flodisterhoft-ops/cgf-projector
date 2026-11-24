#!/usr/bin/env python3
"""Build and run the library-push checks (docs/song-push.md).

    python tests/push/run_tests.py --db <CGF snapshot>          # app + bridge checks
    python tests/push/run_tests.py --db <CGF snapshot> --e2e    # also through a local
                                                                # push server and the companion

--db is a published snapshot (for example CGF Lyrics' library/exports/publish/
spData-cgf-v3.sqlite). It is only read: the checks work on copies in
tests/push/out/ (git-ignored) with made-up edits and a throwaway signing key.
--e2e also needs Node (wrangler dev in push-server/, `npm install` done) and a
built companion (src/companion, see BUILD.md).
"""

import argparse
import json
import os
import pathlib
import re
import secrets
import shutil
import sqlite3
import subprocess
import sys
import threading
import time
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
OUT = HERE / "out"
BIN = OUT / "bin"
sys.path.insert(0, str(REPO / "tools"))
import push_release as pr  # noqa: E402
from cryptography.hazmat.primitives.asymmetric import ec  # noqa: E402

CHURCH = "push-test"
SERVER_PORT = 8788


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


def build(env):
    src = "../../../src"  # relative: qmake mishandles absolute paths with spaces
    lists = project_lists()
    lines = ["QT += " + " ".join(lists.pop("QT")), "LIBS += " + " ".join(lists.pop("LIBS")),
             "TARGET = push_tests", "TEMPLATE = app", "CONFIG += console",
             f"INCLUDEPATH += {src}/headers", "DESTDIR = bin", "OBJECTS_DIR = obj", "MOC_DIR = moc",
             "UI_DIR = ui", "RCC_DIR = rcc", "SOURCES += ../test_push.cpp"]
    for key, files in lists.items():
        lines += [f"{key} += {src}/{f}" for f in files]
    (OUT / "push_tests.pro").write_text("\n".join(lines) + "\n", encoding="utf-8")
    qmake = shutil.which("qmake", path=env["PATH"])
    make = shutil.which("mingw32-make", path=env["PATH"])
    if not qmake or not make:
        sys.exit("Qt or MinGW not found (see BUILD.md); pass --qt")
    subprocess.run([qmake, "push_tests.pro", "-spec", "win32-g++"], cwd=OUT, env=env, check=True)
    subprocess.run([make, "-f", "Makefile.Release", f"-j{os.cpu_count() or 4}"],
                   cwd=OUT, env=env, check=True, stdout=subprocess.DEVNULL)


def make_versions(base, publish):
    """v3 = the given snapshot; v4 edits two songs, removes one and adds one;
    v5 adds a songbook with a song in it. All edits are made-up test text."""
    publish.mkdir(parents=True, exist_ok=True)
    v3, v4, v5 = (publish / f"spData-cgf-v{n}.sqlite" for n in (3, 4, 5))
    shutil.copyfile(base, v3)
    con = sqlite3.connect(v3)
    ids = [r[0] for r in con.execute("SELECT id FROM Songs WHERE id <= 500000 ORDER BY id LIMIT 3")]
    max_id = con.execute("SELECT MAX(id) FROM Songs WHERE id <= 500000").fetchone()[0]
    book, max_book = con.execute("SELECT MIN(id), MAX(id) FROM Songbooks").fetchone()
    old_title0 = con.execute("SELECT title FROM Songs WHERE id = ?", (ids[0],)).fetchone()[0]
    con.close()

    new_text0 = "Verse 1\nPush test line one\nPush test line two\n\nVerse 2\nPush test line three"
    shutil.copyfile(v3, v4)
    con = sqlite3.connect(v4)
    con.execute("UPDATE Songs SET title = title || ' (push test)', song_text = ? WHERE id = ?", (new_text0, ids[0]))
    con.execute("UPDATE Songs SET song_text = song_text || ? WHERE id = ?", ("\nPush test extra line", ids[1]))
    con.execute("DELETE FROM Songs WHERE id = ?", (ids[2],))
    con.execute("INSERT INTO Songs (id, songbook_id, number, title, song_text, count) VALUES (?,?,?,?,?,0)",
                (max_id + 1, book, 9999, "Push test new song", "Verse 1\nA new song"))
    con.commit()
    con.close()

    shutil.copyfile(v4, v5)
    con = sqlite3.connect(v5)
    con.execute("INSERT INTO Songbooks (id, name, info) VALUES (?, 'Push test book', '')", (max_book + 1,))
    con.execute("INSERT INTO Songs (id, songbook_id, number, title, song_text, count) VALUES (?,?,1,?,?,0)",
                (max_id + 2, max_book + 1, "Push test song in a new book", "Verse 1\nIn a new book"))
    con.commit()
    con.close()
    expect = {"edited0": ids[0], "edited1": ids[1], "deleted": ids[2], "added": max_id + 1,
              "addedTitle": "Push test new song", "newTitle0": old_title0 + " (push test)",
              "newText0": new_text0, "newBook": "Push test book", "addedInBook": max_id + 2}
    return v3, v4, v5, expect


def fresh_app_db(v3, expect):
    """The laptop: v3 plus its own quick-add and a local usage count."""
    db = BIN / "spData.sqlite"
    for p in (db, BIN / "spData.sqlite-journal", BIN / "cgf-companion.log"):
        p.unlink(missing_ok=True)
    shutil.copyfile(v3, db)
    con = sqlite3.connect(db)
    con.execute("INSERT INTO Songs (id, songbook_id, number, title, song_text) VALUES (500001, 1, 1, "
                "'Laptop quick-add', 'Verse 1\nlocal')")
    con.execute("UPDATE Songs SET count = 42 WHERE id = ?", (expect["edited0"],))
    con.commit()
    con.close()
    (BIN / "version.txt").write_text("3\n", encoding="utf-8")


def make_releases(v3, v4, v5, expect, key):
    saved = pr.PART_BUDGET
    pr.PART_BUDGET = 1  # one song per part, to exercise multi-part releases
    try:
        r34 = pr.build_parts(pr.compute_delta(v3, v4), CHURCH, 3, 4, v4.name, "test", key)
    finally:
        pr.PART_BUDGET = saved
    r45 = pr.build_parts(pr.compute_delta(v4, v5), CHURCH, 4, 5, v5.name, "test", key)
    empty = {"songbooks": [], "songbooks_deleted": [], "songs_deleted": [], "other_changes": False,
             "summary": {}, "songs": []}
    bad = dict(empty, songs=[{"id": 500002, "title": "Not allowed", "song_text": "x"}])
    tampered = dict(r34[0])
    tampered["payload"] = tampered["payload"].replace("Push test", "Forged")
    return {"expect": expect, "r34": r34, "r45": r45, "tampered": tampered,
            "bad56": pr.build_parts(bad, CHURCH, 5, 6, "x", "x", key),
            "gap78": pr.build_parts(empty, CHURCH, 7, 8, "x", "x", key)}


def run_harness(args, env, mode_args):
    exe = BIN / "push_tests.exe"
    p = subprocess.run([str(exe), *mode_args], cwd=OUT, env=env, capture_output=True,
                       text=True, encoding="utf-8", errors="replace", timeout=180)
    show(p.stderr)
    return p.returncode


def show(stderr):
    for line in stderr.splitlines():
        if line.startswith(("ok  :", "FAIL:")) or " checks, " in line or line.startswith("Library release"):
            print(line)


def e2e(env, v3, v4, expect):
    print("--- end to end: wrangler dev + companion")
    companion = REPO / "src" / "win32_build" / "bin" / "cgf-companion.exe"
    if not companion.exists():
        sys.exit(f"Build the companion first ({companion} is missing; see BUILD.md)")
    shutil.copyfile(companion, BIN / "cgf-companion.exe")

    cfg_dir = OUT / "publish-config"
    shutil.rmtree(cfg_dir, ignore_errors=True)
    env = dict(env, CGF_PUBLISH_DIR_CONFIG=str(cfg_dir))
    py = [sys.executable, str(REPO / "tools" / "push_release.py")]
    subprocess.run(py + ["init", "--server", f"http://127.0.0.1:{SERVER_PORT}", "--church", CHURCH],
                   env=env, check=True, stdout=subprocess.DEVNULL)
    token = (cfg_dir / "publish-token.txt").read_text("utf-8").strip()

    publish = OUT / "publish-e2e"
    shutil.rmtree(publish, ignore_errors=True)
    publish.mkdir()
    shutil.copyfile(v3, publish / v3.name)
    shutil.copyfile(v4, publish / v4.name)
    (publish / "manifest.json").write_text(json.dumps({
        "content_version": 4, "file": v4.name, "sha256": pr.file_sha256(v4),
        "bytes": v4.stat().st_size, "published_at": "test"}), "utf-8")

    state = OUT / "wrangler-state"
    shutil.rmtree(state, ignore_errors=True)
    npx = shutil.which("npx.cmd" if os.name == "nt" else "npx")
    server = subprocess.Popen([npx, "wrangler", "dev", "--port", str(SERVER_PORT), "--ip", "127.0.0.1",
                               "--persist-to", str(state), "--var", f"PUBLISH_TOKEN:{token}"],
                              cwd=REPO / "push-server", env=env, stdout=subprocess.DEVNULL,
                              stderr=subprocess.DEVNULL)
    harness = None
    try:
        if not wait_for(lambda: http_ok(f"http://127.0.0.1:{SERVER_PORT}/"), 60):
            sys.exit("wrangler dev did not start")
        subprocess.run(py + ["add-device", "Push test laptop", "--out", str(BIN / "cgf-cloud.json")],
                       env=env, check=True, stdout=subprocess.DEVNULL)
        fresh_app_db(v3, expect)
        harness = subprocess.Popen([str(BIN / "push_tests.exe"), "--e2e", "4"], cwd=OUT, env=env,
                                   stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace")
        lines = []
        threading.Thread(target=lambda: lines.extend(harness.stderr), daemon=True).start()

        # The companion connects once the app's bridge is up; wait for it.
        if not wait_for(lambda: device_connected(token), 60):
            print("FAIL: the companion never connected to the push server")
        r = subprocess.run(py + ["push", "--publish-dir", str(publish)], env=env, capture_output=True,
                           text=True, encoding="utf-8")
        print(r.stdout.strip() or r.stderr.strip())
        code = harness.wait(timeout=120)
        show("".join(lines))
        status = server_status(token)
        dev = next((d for d in status["devices"] if d["name"] == "Push test laptop"), {})
        ok = dev.get("last_version") == 4
        print(("ok  :" if ok else "FAIL:") + f" the server records the laptop at v{dev.get('last_version')}")
        log = (BIN / "cgf-companion.log").read_text("utf-8") if (BIN / "cgf-companion.log").exists() else ""
        logged = "v4 is in the projector's library" in log
        print(("ok  :" if logged else "FAIL:") + " the companion log records the release")
        return 0 if code == 0 and ok and logged else 1
    finally:
        if harness and harness.poll() is None:
            harness.kill()
        if os.name == "nt":
            subprocess.run(["taskkill", "/T", "/F", "/PID", str(server.pid)], capture_output=True)
        else:
            server.terminate()


def server_status(token):
    req = urllib.request.Request(f"http://127.0.0.1:{SERVER_PORT}/v1/{CHURCH}/status",
                                 headers={"Authorization": f"Bearer {token}"})
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.loads(r.read())


def device_connected(token):
    try:
        return any(d["connected"] for d in server_status(token)["devices"])
    except Exception:
        return False


def http_ok(url):
    try:
        with urllib.request.urlopen(url, timeout=2) as r:
            return r.status == 200
    except Exception:
        return False


def wait_for(cond, seconds):
    end = time.time() + seconds
    while time.time() < end:
        if cond():
            return True
        time.sleep(0.5)
    return False


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", required=True, help="a published CGF snapshot to copy (only read)")
    ap.add_argument("--qt", default=str(pathlib.Path.home() / "Qt"), help="aqt install folder (default ~/Qt)")
    ap.add_argument("--e2e", action="store_true", help="also run through wrangler dev and the companion")
    args = ap.parse_args()

    qt = pathlib.Path(args.qt)
    env = dict(os.environ)
    env["PATH"] = os.pathsep.join([str(qt / "Tools" / "mingw1310_64" / "bin"),
                                   str(qt / "6.8.3" / "mingw_64" / "bin"), env.get("PATH", "")])
    env["QT_FORCE_STDERR_LOGGING"] = "1"
    BIN.mkdir(parents=True, exist_ok=True)
    build(env)

    v3, v4, v5, expect = make_versions(pathlib.Path(args.db), OUT / "publish")
    key = ec.generate_private_key(ec.SECP256R1())
    (BIN / "releases.json").write_text(json.dumps(make_releases(v3, v4, v5, expect, key)), "utf-8")

    print("--- app and bridge")
    fresh_app_db(v3, expect)
    (BIN / "cgf-cloud.json").write_text(json.dumps({"church": CHURCH, "publisherKeys": [pr.public_key_b64(key)]}),
                                        "utf-8")
    (BIN / "cgf-companion.exe").unlink(missing_ok=True)
    code = run_harness(args, dict(env, CGF_NO_COMPANION="1"), ["--local"])
    if args.e2e:
        code = e2e(env, v3, v4, expect) or code
    sys.exit(code)


if __name__ == "__main__":
    main()

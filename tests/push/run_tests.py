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
    v5 adds a songbook with a song in it; v6 replaces the library's German
    Bible (the snapshot's last Bible id, OLD) by a new Bible OLD+1 whose
    Psalm 23:1 is made-up test text, and records OLD -> OLD+1 in
    CgfBibleReplaces. All edits are made-up test text."""
    publish.mkdir(parents=True, exist_ok=True)
    v3, v4, v5, v6 = (publish / f"spData-cgf-v{n}.sqlite" for n in (3, 4, 5, 6))
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
    shutil.copyfile(v5, v6)
    con = sqlite3.connect(v6)
    old = con.execute("SELECT MAX(id) FROM BibleVersions").fetchone()[0]
    new = old + 1
    con.execute("INSERT INTO BibleVersions (id, bible_name, abbreviation, information, right_to_left) "
                "SELECT ?, 'Push test Bible (v9)', abbreviation, information, right_to_left FROM BibleVersions WHERE id = ?",
                (new, old))
    con.execute("INSERT INTO BibleBooks SELECT ?, id, book_name, chapter_count FROM BibleBooks WHERE bible_id = ? "
                "ORDER BY rowid", (new, old))
    con.execute("INSERT INTO BibleVerse SELECT verse_id, ?, book, chapter, verse, verse_text FROM BibleVerse "
                "WHERE CAST(bible_id AS INTEGER) = ? ORDER BY rowid", (str(new), old))
    con.execute("UPDATE BibleVerse SET verse_text = 'Push test psalm text' WHERE CAST(bible_id AS INTEGER) = ? "
                "AND verse_id = 'B019C023V001'", (new,))
    verses = con.execute("SELECT COUNT(*) FROM BibleVerse WHERE CAST(bible_id AS INTEGER) = ?", (new,)).fetchone()[0]
    for t in ("BibleVerse", "BibleBooks"):
        con.execute(f"DELETE FROM {t} WHERE CAST(bible_id AS INTEGER) = ?", (old,))
    con.execute("DELETE FROM BibleVersions WHERE id = ?", (old,))
    con.execute("CREATE TABLE CgfBibleReplaces (old_id TEXT PRIMARY KEY, new_id TEXT NOT NULL)")
    con.execute("INSERT INTO CgfBibleReplaces VALUES (?, ?)", (str(old), str(new)))
    library = [r[0] for r in con.execute("SELECT id FROM BibleVersions ORDER BY id")]
    con.commit()
    con.close()
    expect = {"edited0": ids[0], "edited1": ids[1], "deleted": ids[2], "added": max_id + 1,
              "addedTitle": "Push test new song", "newTitle0": old_title0 + " (push test)",
              "newText0": new_text0, "newBook": "Push test book", "addedInBook": max_id + 2,
              "oldBible": str(old), "newBible": str(new), "newBibleName": "Push test Bible (v9)",
              "newBibleVerses": verses, "library": [str(i) for i in library]}
    return v3, v4, v5, v6, expect


def fresh_app_db(v3, expect):
    """The laptop: v3 plus its own quick-add, a local usage count and a Bible
    imported by hand under the id the library's new Bible will get (as on the
    church laptop in November 2026), which is the second Bible on the screen;
    display 2 shows the library's old German Bible."""
    db = BIN / "spData.sqlite"
    for p in (db, BIN / "spData.sqlite-journal", BIN / "cgf-companion.log"):
        p.unlink(missing_ok=True)
    shutil.copyfile(v3, db)
    con = sqlite3.connect(db)
    con.execute("INSERT INTO Songs (id, songbook_id, number, title, song_text) VALUES (500001, 1, 1, "
                "'Laptop quick-add', 'Verse 1\nlocal')")
    con.execute("UPDATE Songs SET count = 42 WHERE id = ?", (expect["edited0"],))
    old, new = expect["oldBible"], expect["newBible"]
    con.execute("INSERT INTO BibleVersions (id, bible_name, abbreviation, information, right_to_left) "
                "VALUES (?, 'Hand import', '', '', 0)", (int(new),))
    con.execute("INSERT INTO BibleBooks SELECT ?, id, book_name, chapter_count FROM BibleBooks WHERE bible_id = ?",
                (int(new), int(old)))
    con.execute("INSERT INTO BibleVerse SELECT verse_id, ?, book, chapter, verse, 'hand import text' FROM BibleVerse "
                "WHERE CAST(bible_id AS INTEGER) = ? LIMIT 50", (new, int(old)))
    con.execute("UPDATE Settings SET sets = ? WHERE type = 'bible1'",
                (f"primary = 2\nsecondary = {new}\ntrinary = 3\noperator = same",))
    con.execute("UPDATE Settings SET sets = ? WHERE type = 'bible2'",
                (f"primary = {old}\nsecondary = none\ntrinary = none",))
    con.commit()
    con.close()
    (BIN / "version.txt").write_text("3\n", encoding="utf-8")


def make_releases(v3, v4, v5, v6, expect, key):
    saved = pr.PART_BUDGET
    pr.PART_BUDGET = 1  # one song per part, to exercise multi-part releases
    try:
        r34 = pr.build_parts(pr.compute_delta(v3, v4), CHURCH, 3, 4, v4.name, "test", key)
    finally:
        pr.PART_BUDGET = saved
    r45 = pr.build_parts(pr.compute_delta(v4, v5), CHURCH, 4, 5, v5.name, "test", key)
    pr.PART_BUDGET = 300 * 1024  # a Bible in several parts
    try:
        r56 = pr.build_parts(pr.compute_delta(v5, v6), CHURCH, 5, 6, v6.name, "test", key)
    finally:
        pr.PART_BUDGET = saved
    # A release asking for something no app version understands.
    future = json.dumps({"kind": "cgf-release", "v": 1, "church": CHURCH, "from": 6, "to": 7, "part": 1,
                         "parts": 1, "needs": ["teleport"], "songs": []}, separators=(",", ":"))
    future_part = {"type": "release", "payload": future, "sig": pr.sign(key, future), "key": pr.key_id(key)}
    empty = {"songbooks": [], "songbooks_deleted": [], "songs_deleted": [], "other_changes": False,
             "summary": {}, "songs": []}
    bad = dict(empty, songs=[{"id": 500002, "title": "Not allowed", "song_text": "x"}])
    tampered = dict(r34[0])
    tampered["payload"] = tampered["payload"].replace("Push test", "Forged")
    return {"expect": expect, "r34": r34, "r45": r45, "r56": r56, "future67": future_part, "tampered": tampered,
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


def e2e(env, versions, expect):
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

    # v3 -> v6: two song releases, then a Bible release at the publisher's
    # real part size, which the app holds while a passage is on the projector.
    publish = OUT / "publish-e2e"
    shutil.rmtree(publish, ignore_errors=True)
    publish.mkdir()
    for v in versions:
        shutil.copyfile(v, publish / v.name)
    last = versions[-1]
    (publish / "manifest.json").write_text(json.dumps({
        "content_version": 6, "file": last.name, "sha256": pr.file_sha256(last),
        "bytes": last.stat().st_size, "published_at": "test"}), "utf-8")

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
        fresh_app_db(versions[0], expect)
        # As on an installed laptop: the server sends a Bible release only to a
        # laptop whose app is new enough (cgf-versions.txt, docs/song-push.md).
        (BIN / "cgf-versions.txt").write_text("current=app-0.2.7" + chr(10), encoding="utf-8")
        harness = subprocess.Popen([str(BIN / "push_tests.exe"), "--e2e", "6"], cwd=OUT, env=env,
                                   stderr=subprocess.PIPE, text=True, encoding="utf-8", errors="replace")
        lines = []
        threading.Thread(target=lambda: lines.extend(harness.stderr), daemon=True).start()

        # The companion connects once the app's bridge is up; wait for it.
        if not wait_for(lambda: device_connected(token), 60):
            print("FAIL: the companion never connected to the push server")
        r = subprocess.run(py + ["push", "--publish-dir", str(publish)], env=env, capture_output=True,
                           text=True, encoding="utf-8")
        print(r.stdout.strip() or r.stderr.strip())
        code = harness.wait(timeout=240)
        show("".join(lines))
        status = server_status(token)
        dev = next((d for d in status["devices"] if d["name"] == "Push test laptop"), {})
        ok = dev.get("last_version") == 6
        print(("ok  :" if ok else "FAIL:") + f" the server records the laptop at v{dev.get('last_version')}")
        log = (BIN / "cgf-companion.log").read_text("utf-8") if (BIN / "cgf-companion.log").exists() else ""
        logged = all(f"v{v} is in the projector's library" in log for v in (4, 5, 6))
        print(("ok  :" if logged else "FAIL:") + " the companion log records v4, v5 and v6")
        held = "v6 changes Bibles" in log
        print(("ok  :" if held else "FAIL:") + " the companion log records that the projector held the Bible release")
        logged = logged and held
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

    v3, v4, v5, v6, expect = make_versions(pathlib.Path(args.db), OUT / "publish")
    key = ec.generate_private_key(ec.SECP256R1())
    (BIN / "releases.json").write_text(json.dumps(make_releases(v3, v4, v5, v6, expect, key)), "utf-8")

    print("--- app and bridge")
    fresh_app_db(v3, expect)
    (BIN / "cgf-cloud.json").write_text(json.dumps({"church": CHURCH, "publisherKeys": [pr.public_key_b64(key)]}),
                                        "utf-8")
    (BIN / "cgf-companion.exe").unlink(missing_ok=True)
    code = run_harness(args, dict(env, CGF_NO_COMPANION="1"), ["--local"])
    if args.e2e:
        code = e2e(env, [v3, v4, v5, v6], expect) or code
    sys.exit(code)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""App updates end to end (docs/song-push.md, "App updates").

    python tests/update/run_tests.py --db <published snapshot, e.g. spData-cgf-v3.sqlite>

Needs the deployed build (src/win32_build/bin with cgf-companion.exe), the
launcher (src/win32_build/launcher) and push-server/ with `npm install` done.
Everything happens in tests/update/out/ (git-ignored) against `wrangler dev`
on port 8789 with a throwaway publisher; the snapshot is only read.

It installs the package into a test "laptop" folder, then:
  t2: one new file      the companion downloads just that, copies the rest, switches
  app via the launcher  runs offscreen, marks itself healthy; a song push still lands
  t3 while the app runs the app is told a restart is ready; app-t1 is tidied away
  t4 that crashes       the launcher goes back to t3 by itself and never retries t4
The installed app runs without Qt on PATH, so the package must be complete.
"""

import argparse
import importlib.util
import json
import os
import pathlib
import shutil
import sqlite3
import subprocess
import sys
import time
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
OUT = HERE / "out"
BUILD = REPO / "src" / "win32_build" / "bin"
LAUNCHER = REPO / "src" / "win32_build" / "launcher" / "CGFProjector.exe"
LAPTOP = OUT / "laptop"
PORT = 8789
BRIDGE_PORT = 8776
CHURCH = "update-test"
PY = [sys.executable, str(REPO / "tools" / "push_release.py")]

checks = failures = 0


def check(cond, msg):
    global checks, failures
    checks += 1
    if not cond:
        failures += 1
    print(("ok  : " if cond else "FAIL: ") + msg)
    return cond


def wait_for(cond, seconds, step=0.5):
    end = time.time() + seconds
    while time.time() < end:
        try:
            if cond():
                return True
        except Exception:
            pass
        time.sleep(step)
    return False


def state():
    out = {}
    p = LAPTOP / "cgf-versions.txt"
    if p.exists():
        for line in p.read_text("utf-8").splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                out[k.strip()] = v.strip()
    return out


def text(name):
    p = LAPTOP / name
    return p.read_text("utf-8", errors="replace") if p.exists() else ""


def server_status(token):
    req = urllib.request.Request(f"http://127.0.0.1:{PORT}/v1/{CHURCH}/status",
                                 headers={"Authorization": f"Bearer {token}"})
    with urllib.request.urlopen(req, timeout=5) as r:
        return json.loads(r.read())


def processes():
    """CGF Projector programs running from the test laptop folder: [(name, path, pid)]."""
    ps = ("Get-Process CGFProjector, cgf-companion -ErrorAction SilentlyContinue | "
          "ForEach-Object { \"$($_.Name)|$($_.Path)|$($_.Id)\" }")
    r = subprocess.run(["powershell", "-NoProfile", "-Command", ps], capture_output=True, text=True)
    out = []
    for line in r.stdout.splitlines():
        parts = line.strip().split("|")
        if len(parts) == 3 and parts[1].lower().startswith(str(LAPTOP).lower()):
            out.append((parts[0], parts[1], int(parts[2])))
    return out


def kill(name=None):
    for n, _, pid in processes():
        if name is None or n == name:
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(pid)], capture_output=True)


def release(env, build_dir, version, notes):
    r = subprocess.run(PY + ["app-release", "--build-dir", str(build_dir), "--version", version,
                             "--notes", notes, "--launcher", str(LAUNCHER)],
                       env=env, capture_output=True, text=True, encoding="utf-8")
    print("   " + (r.stdout.strip().splitlines() or [r.stderr.strip()])[-1])
    if r.returncode != 0:
        print(r.stderr)
    return r.returncode == 0


def derived_build(base, name, extra_file=None, replace_exe=None, offscreen=None):
    dest = OUT / f"build-{name}"
    shutil.rmtree(dest, ignore_errors=True)
    shutil.copytree(base, dest, ignore=shutil.ignore_patterns("*.log", "spData*", "*.sqlite*"))
    if offscreen:
        # Test builds only: the offscreen platform these checks run the app on.
        # The real package carries just the Windows one.
        shutil.copyfile(offscreen, dest / "platforms" / "qoffscreen.dll")
    if extra_file:
        (dest / extra_file).write_text(f"update test {name}\n", "utf-8")
    if replace_exe:
        shutil.copyfile(replace_exe, dest / "CGFProjector.exe")
    return dest


def crasher(env):
    """A CGFProjector.exe that stops with code 3 at once, for the rollback test."""
    src = OUT / "crash.cpp"
    src.write_text("int main() { return 3; }\n", "utf-8")
    exe = OUT / "crash.exe"
    gpp = shutil.which("g++", path=env["PATH"])
    subprocess.run([gpp, "-O2", "-static", "-o", str(exe), str(src)], check=True)
    return exe


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--db", required=True)
    ap.add_argument("--qt", default=str(pathlib.Path.home() / "Qt"))
    args = ap.parse_args()
    for p in (BUILD / "CGFProjector.exe", BUILD / "cgf-companion.exe", LAUNCHER):
        if not p.exists():
            sys.exit(f"{p} is missing: build the app, companion and launcher first (BUILD.md)")

    kill()
    shutil.rmtree(OUT, ignore_errors=True)
    OUT.mkdir(parents=True)
    base_env = dict(os.environ, CGF_PUBLISH_DIR_CONFIG=str(OUT / "publish-config"), PYTHONDONTWRITEBYTECODE="1")
    tool_env = dict(base_env)
    qt = pathlib.Path(args.qt)
    tool_env["PATH"] = os.pathsep.join([str(qt / "Tools" / "mingw1310_64" / "bin"), base_env.get("PATH", "")])
    tool_env["QT_PLUGINS"] = str(qt / "6.8.3" / "mingw_64" / "plugins")
    # The installed app gets no Qt on PATH: the package must carry everything.
    app_env = dict(base_env, QT_QPA_PLATFORM="offscreen", QT_QUICK_BACKEND="software", CGF_BRIDGE_LOCAL="1",
                   CGF_HEALTHY_MS="3000", QT_QPA_FONTDIR="C:/Windows/Fonts")

    subprocess.run(PY + ["init", "--server", f"http://127.0.0.1:{PORT}", "--church", CHURCH],
                   env=base_env, check=True, stdout=subprocess.DEVNULL)
    token = (OUT / "publish-config" / "publish-token.txt").read_text("utf-8").strip()
    npx = shutil.which("npx.cmd" if os.name == "nt" else "npx")
    server = subprocess.Popen([npx, "wrangler", "dev", "--port", str(PORT), "--ip", "127.0.0.1",
                               "--persist-to", str(OUT / "wrangler-state"), "--var", f"PUBLISH_TOKEN:{token}"],
                              cwd=REPO / "push-server", stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        if not wait_for(lambda: urllib.request.urlopen(f"http://127.0.0.1:{PORT}/", timeout=2).status == 200, 60):
            sys.exit("wrangler dev did not start")
        run(base_env, tool_env, app_env, token, pathlib.Path(args.db))
    finally:
        kill()
        subprocess.run(["taskkill", "/T", "/F", "/PID", str(server.pid)], capture_output=True)
    print(f"{checks} checks, {failures} failed")
    sys.exit(1 if failures else 0)


def run(env, tool_env, app_env, token, db):
    print("--- install package")
    subprocess.run(PY + ["add-device", "Update test laptop", "--out", str(OUT / "cloud.json")],
                   env=env, check=True, stdout=subprocess.DEVNULL)
    check(release(env, BUILD, "t1", "First test version"), "release t1 (every file uploaded)")
    spec = importlib.util.spec_from_file_location("push_checks", REPO / "tests" / "push" / "run_tests.py")
    push_checks = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(push_checks)
    publish = OUT / "publish"
    v3, v4, _v5, expect = push_checks.make_versions(db, publish)
    r = subprocess.run([sys.executable, str(REPO / "tools" / "make_install.py"), "--version", "t1", "--seq", "1",
                        "--snapshot", str(v3), "--device-file", str(OUT / "cloud.json"), "--notes", "First test version",
                        "--out", str(OUT / "pkg"), "--keep-folder"], env=env, capture_output=True, text=True)
    check(r.returncode == 0, "the install package is made: " + r.stdout.strip()[-80:])
    r = subprocess.run(["powershell", "-NoProfile", "-ExecutionPolicy", "Bypass", "-File",
                        str(OUT / "pkg" / "CGFProjector-t1" / "install.ps1"), "-Target", str(LAPTOP),
                        "-NoShortcuts", "-NoStart", "-NoFirewall"], capture_output=True, text=True)
    check(r.returncode == 0, "install.ps1 installs into the laptop folder " + (r.stderr.strip()[:200] or ""))
    (LAPTOP / "cgf-network.json").write_text(json.dumps({"port": BRIDGE_PORT}), "utf-8")
    s = state()
    check(s.get("current") == "app-t1" and s.get("seq") == "1", f"cgf-versions.txt names app-t1, release 1 ({s})")
    check((LAPTOP / "CGFProjector.exe").exists() and (LAPTOP / "app-t1" / "cgf-companion.exe").exists()
          and text("version.txt").strip() == "3", "launcher, app-t1, the v3 library and version.txt are in place")

    print("--- t2 with the app closed")
    subprocess.Popen([str(LAPTOP / "CGFProjector.exe"), "--companion"], env=app_env)
    check(wait_for(lambda: any(d["connected"] and d["app_folder"] == "app-t1"
                               for d in server_status(token)["devices"]), 30),
          "the companion (from sign-in) connects and reports app-t1")
    qt_plugins = pathlib.Path(tool_env["QT_PLUGINS"])
    t2 = derived_build(BUILD, "t2", extra_file="update-test-t2.txt", offscreen=qt_plugins / "platforms" / "qoffscreen.dll")
    release(env, t2, "t2", "Second test version")
    check(wait_for(lambda: state().get("current") == "app-t2", 120), "the laptop switches to app-t2")
    s = state()
    check(s.get("previous") == "app-t1" and s.get("trial") == "1", f"app-t1 stays as the fallback, app-t2 on trial ({s})")
    check("(2 files downloaded" in text("cgf-companion.log"), "only the 2 new files were downloaded; the rest copied")
    check((LAPTOP / "app-t2" / "update-test-t2.txt").exists() and (LAPTOP / "app-t2" / "cgf-release.json").exists(),
          "app-t2 has the new file and its release notes")
    check(wait_for(lambda: any(n == "cgf-companion" and "app-t2" in p for n, p, _ in processes()), 30),
          "the companion restarted itself from app-t2")

    print("--- the app through the launcher")
    subprocess.Popen([str(LAPTOP / "CGFProjector.exe")], env=app_env)
    check(wait_for(lambda: text("cgf-healthy.txt").strip() == "app-t2", 45), "app-t2 runs and marks itself healthy")
    (publish / "manifest.json").write_text(json.dumps({"content_version": 4, "file": v4.name,
                                                       "sha256": push_checks.pr.file_sha256(v4)}), "utf-8")
    for p in publish.glob("spData-cgf-v5.sqlite"):
        p.unlink()
    r = subprocess.run(PY + ["push", "--publish-dir", str(publish)], env=env, capture_output=True, text=True)
    def library_version():
        con = sqlite3.connect(f"file:{LAPTOP / 'spData.sqlite'}?mode=ro", uri=True)
        try:
            return int(con.execute("SELECT value FROM CgfSync WHERE key = 'content_version'").fetchone()[0])
        finally:
            con.close()
    check(wait_for(lambda: library_version() == 4, 30), "a song push still lands in this layout (library v4)")

    print("--- t3 while the app runs")
    t3 = derived_build(t2, "t3", extra_file="update-test-t3.txt")
    release(env, t3, "t3", "Third test version")
    check(wait_for(lambda: state().get("current") == "app-t3", 120), "the laptop switches to app-t3")
    check(wait_for(lambda: "App update t3 is ready" in text("CGFProjector.log"), 30), "the open app offers the restart")
    check("told the projector that app-t3 is ready" in text("cgf-companion.log"), "the companion told the app")
    check(not (LAPTOP / "app-t1").exists() and (LAPTOP / "app-t2").exists(),
          "app-t1 is tidied away; app-t2 (still running) stays")
    kill("CGFProjector")

    print("--- t4 that crashes")
    t4 = derived_build(t3, "t4", replace_exe=crasher(tool_env))
    release(env, t4, "t4", "A broken test version")
    check(wait_for(lambda: state().get("current") == "app-t4", 120), "the laptop installs app-t4")
    subprocess.Popen([str(LAPTOP / "CGFProjector.exe")], env=app_env)
    check(wait_for(lambda: state().get("current") == "app-t3", 40), "the launcher goes back to app-t3 by itself")
    s = state()
    check("app-t4" in s.get("bad", ""), f"app-t4 is marked bad ({s.get('bad')})")
    check("stopped with code 3" in text("cgf-launcher.log"), "cgf-launcher.log says why")
    check(wait_for(lambda: any(n == "CGFProjector" and "app-t3" in p for n, p, _ in processes()), 20),
          "app-t3 was started instead")
    time.sleep(5)
    check(state().get("current") == "app-t3", "the companion does not install app-t4 again")


if __name__ == "__main__":
    main()

"""Push a published library release to the church laptops (docs/song-push.md).

publish_snapshot.py (CGF Lyrics) writes versioned snapshots and manifest.json
to the publish folder. This tool compares each new snapshot with the one
before it, signs the changed songs and songbooks, and sends them to the push
server, which passes them to every connected laptop at once. Laptops that are
offline get them when they reconnect.

Commands:
  init --server URL [--church ID]   one-time: signing key, publish token, config
  server-secret --wrangler-dir DIR  set PUBLISH_TOKEN on the server (via wrangler)
  add-device NAME --out FILE        register a laptop; writes its cgf-cloud.json
  remove-device NAME
  push --publish-dir DIR            push every version the server lacks
  status                            the server's latest version and laptops
  diff OLD.sqlite NEW.sqlite        what a release would contain (nothing sent)

Settings live in %USERPROFILE%\\.cgf-publish\\ (push.json, signing-key.pem,
publish-token.txt), outside every repository. Secrets are never printed.

Requires the `cryptography` package.
"""
import argparse
import base64
import datetime
import hashlib
import json
import os
import re
import secrets
import sqlite3
import subprocess
import sys
import urllib.error
import urllib.request
from pathlib import Path

from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import decode_dss_signature

if hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8")

CONFIG_DIR = Path(os.environ.get("CGF_PUBLISH_DIR_CONFIG", Path.home() / ".cgf-publish"))
LOCAL_BAND = 500000            # song ids above this are a laptop's own quick-adds
PART_BUDGET = 900 * 1024       # payload characters per part, far below the server's limit
MAX_ROW = 3 * 1024 * 1024      # a larger song (a huge background) waits for the snapshot
# Usage counters: each laptop keeps its own; they are not song content.
SONG_LOCAL_COLUMNS = {"count", "date"}
SNAPSHOT = re.compile(r"spData-cgf-v(\d+)\.sqlite$")


# ---------------------------------------------------------------------------
# Settings and secrets

def config_path(name):
    return CONFIG_DIR / name


def load_config():
    p = config_path("push.json")
    if not p.exists():
        raise SystemExit(f"No push settings at {p}. Run: push_release.py init --server <url>")
    cfg = json.loads(p.read_text("utf-8"))
    cfg["token"] = config_path("publish-token.txt").read_text("utf-8").strip()
    return cfg


def restrict_to_user(path):
    """Only the current Windows user may read the file."""
    if os.name == "nt":
        user = os.environ.get("USERNAME", "")
        subprocess.run(["icacls", str(path), "/inheritance:r", "/grant:r", f"{user}:F"],
                       check=True, capture_output=True)


def write_secret(path, text):
    path.write_text(text, "utf-8")
    restrict_to_user(path)


def load_key():
    pem = config_path("signing-key.pem").read_bytes()
    return serialization.load_pem_private_key(pem, password=None)


def public_key_b64(key):
    """The public key as the app reads it: X || Y, 64 bytes, base64."""
    point = key.public_key().public_bytes(serialization.Encoding.X962,
                                          serialization.PublicFormat.UncompressedPoint)
    return base64.b64encode(point[1:]).decode()


def key_id(key):
    raw = base64.b64decode(public_key_b64(key))
    return hashlib.sha256(raw).hexdigest()[:16]


def sign(key, payload):
    """ECDSA P-256 / SHA-256 over the payload's UTF-8 bytes; r || s, base64."""
    der = key.sign(payload.encode("utf-8"), ec.ECDSA(hashes.SHA256()))
    r, s = decode_dss_signature(der)
    return base64.b64encode(r.to_bytes(32, "big") + s.to_bytes(32, "big")).decode()


# ---------------------------------------------------------------------------
# Server calls

def request(cfg, method, path, body=None, raw=None):
    url = cfg["server"].rstrip("/") + f"/v1/{cfg['church']}{path}"
    data = raw if raw is not None else (None if body is None else json.dumps(body, ensure_ascii=False).encode("utf-8"))
    req = urllib.request.Request(url, data=data, method=method, headers={
        "Authorization": f"Bearer {cfg['token']}",
        "Content-Type": "application/octet-stream" if raw is not None else "application/json",
        "User-Agent": "cgf-push/1",
    })
    try:
        with urllib.request.urlopen(req, timeout=300) as r:
            return json.loads(r.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")[:300]
        raise SystemExit(f"Server refused {method} {path}: HTTP {e.code} {detail}")
    except urllib.error.URLError as e:
        raise SystemExit(f"Cannot reach the push server at {cfg['server']}: {e.reason}")


def device_url(cfg):
    base = cfg["server"].rstrip("/")
    base = re.sub(r"^http(s?)://", lambda m: "ws" + m.group(1) + "://", base)
    return f"{base}/v1/{cfg['church']}/ws"


# ---------------------------------------------------------------------------
# Deltas between two snapshots

def open_ro(path):
    uri = Path(path).resolve().as_uri() + "?mode=ro"
    con = sqlite3.connect(uri, uri=True)
    con.row_factory = sqlite3.Row
    return con


def encode(value):
    if isinstance(value, bytes):
        return {"$b64": base64.b64encode(value).decode()}
    return value


def rows_by_id(con, table, where=""):
    cols = [r[1] for r in con.execute(f"PRAGMA table_info('{table}')")]
    out = {}
    for row in con.execute(f"SELECT * FROM '{table}' {where}"):
        out[row["id"]] = {c: encode(row[c]) for c in cols}
    return out


def table_digest(con, table):
    h = hashlib.sha256()
    cols = [r[1] for r in con.execute(f"PRAGMA table_info('{table}')")]
    h.update(json.dumps(cols).encode())
    for row in con.execute(f"SELECT * FROM '{table}' ORDER BY rowid"):
        h.update(json.dumps([encode(v) for v in row], ensure_ascii=False).encode("utf-8"))
    return h.hexdigest()


def compute_delta(old_path, new_path):
    """Songs and songbooks that differ between two snapshots, plus whether
    anything else (Bibles, themes, ...) changed; that waits for the snapshot."""
    old, new = open_ro(old_path), open_ro(new_path)
    try:
        band = f"WHERE id <= {LOCAL_BAND}"
        s_old, s_new = rows_by_id(old, "Songs", band), rows_by_id(new, "Songs", band)
        b_old, b_new = rows_by_id(old, "Songbooks"), rows_by_id(new, "Songbooks")

        def content(row):
            return {k: v for k, v in row.items() if k not in SONG_LOCAL_COLUMNS}

        songs = [s_new[i] for i in sorted(s_new) if i not in s_old or content(s_new[i]) != content(s_old[i])]
        songs_added = sum(1 for r in songs if r["id"] not in s_old)
        songs_deleted = sorted(i for i in s_old if i not in s_new)
        songbooks = [b_new[i] for i in sorted(b_new) if b_new[i] != b_old.get(i)]
        songbooks_deleted = sorted(i for i in b_old if i not in b_new)

        skipped = [r["id"] for r in songs if len(json.dumps(r, ensure_ascii=False)) > MAX_ROW]
        songs = [r for r in songs if r["id"] not in skipped]

        tables = lambda con: {r[0] for r in con.execute(
            "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%'")}
        others = (tables(old) | tables(new)) - {"Songs", "Songbooks"}
        other_changes = bool(skipped) or any(
            t not in tables(old) or t not in tables(new) or table_digest(old, t) != table_digest(new, t)
            for t in sorted(others))
        return {
            "songbooks": songbooks,
            "songbooks_deleted": songbooks_deleted,
            "songs": songs,
            "songs_deleted": songs_deleted,
            "other_changes": other_changes,
            "summary": {
                "songs_changed": len(songs) - songs_added,
                "songs_added": songs_added,
                "songs_deleted": len(songs_deleted),
                "songbooks_changed": len(songbooks) + len(songbooks_deleted),
                "skipped_large": skipped,
            },
        }
    finally:
        old.close()
        new.close()


def build_parts(delta, church, from_v, to_v, snapshot_name, snapshot_sha, key, published_at=None):
    """Split a delta into signed parts. Songbooks and deletions ride in part 1
    so a song never arrives before its songbook."""
    header = {
        "kind": "cgf-release", "v": 1, "church": church, "from": from_v, "to": to_v,
        "published_at": published_at or datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "snapshot": snapshot_name, "snapshot_sha256": snapshot_sha,
        "other_changes": delta["other_changes"], "summary": delta["summary"],
    }
    groups, current, size = [], [], 0
    for row in delta["songs"]:
        n = len(json.dumps(row, ensure_ascii=False))
        if current and size + n > PART_BUDGET:
            groups.append(current)
            current, size = [], 0
        current.append(row)
        size += n
    groups.append(current)

    kid = key_id(key)
    parts = []
    for i, songs in enumerate(groups, start=1):
        body = dict(header, part=i, parts=len(groups), songs=songs,
                    songbooks=delta["songbooks"] if i == 1 else [],
                    songbooks_deleted=delta["songbooks_deleted"] if i == 1 else [],
                    songs_deleted=delta["songs_deleted"] if i == 1 else [])
        payload = json.dumps(body, ensure_ascii=False, separators=(",", ":"))
        parts.append({"type": "release", "payload": payload, "sig": sign(key, payload), "key": kid})
    return parts


def snapshot_files(publish_dir):
    found = {}
    for p in Path(publish_dir).glob("spData-cgf-v*.sqlite"):
        m = SNAPSHOT.search(p.name)
        if m:
            found[int(m.group(1))] = p
    return found


def file_sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


# ---------------------------------------------------------------------------
# Commands

def cmd_init(args):
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    key_file = config_path("signing-key.pem")
    if key_file.exists():
        print(f"Keeping the existing signing key ({key_file}).")
    else:
        key = ec.generate_private_key(ec.SECP256R1())
        pem = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                serialization.NoEncryption())
        key_file.write_bytes(pem)
        restrict_to_user(key_file)
        print(f"Made a signing key: {key_file}")
    token_file = config_path("publish-token.txt")
    if token_file.exists():
        print("Keeping the existing publish token.")
    else:
        write_secret(token_file, secrets.token_urlsafe(32))
        print(f"Made a publish token: {token_file} (not shown)")
    # push_tool: CGF Lyrics' publish_snapshot.py runs this file after each publish.
    cfg = {"server": args.server.rstrip("/"), "church": args.church, "push_tool": str(Path(__file__).resolve())}
    config_path("push.json").write_text(json.dumps(cfg, indent=2), "utf-8")
    key = load_key()
    print(f"Settings: {config_path('push.json')}")
    print(f"Public key (goes into each laptop's cgf-cloud.json; add-device does this): {public_key_b64(key)}")
    print(f"Key id: {key_id(key)}")


def cmd_server_secret(args):
    """Hands the publish token to `wrangler secret put` on stdin; it is never shown."""
    cfg = load_config()
    wrangler_dir = Path(args.wrangler_dir)
    cmd = ["npx.cmd" if os.name == "nt" else "npx", "wrangler", "secret", "put", "PUBLISH_TOKEN"]
    r = subprocess.run(cmd, input=cfg["token"], text=True, cwd=wrangler_dir)
    raise SystemExit(r.returncode)


def cmd_add_device(args):
    cfg = load_config()
    r = request(cfg, "POST", "/devices", {"name": args.name})
    cloud = {
        "server": device_url(cfg),
        "church": cfg["church"],
        "device": r["name"],
        "token": r["token"],
        "publisherKeys": [public_key_b64(load_key())],
    }
    out = Path(args.out)
    out.parent.mkdir(parents=True, exist_ok=True)
    write_secret(out, json.dumps(cloud, indent=2))
    print(f"Registered '{r['name']}'. Its settings are in {out} (token not shown).")
    print("Copy that file next to CGFProjector.exe on the laptop.")


def cmd_remove_device(args):
    cfg = load_config()
    from urllib.parse import quote
    r = request(cfg, "DELETE", "/devices/" + quote(args.name, safe=""))
    print("Removed." if r.get("deleted") else "No device with that name.")


def cmd_status(args):
    cfg = load_config()
    s = request(cfg, "GET", "/status")
    if s["versions"]:
        kept = f"latest release v{s['latest']}, {s['versions']} kept (oldest from v{s['oldest_from']})"
    else:
        kept = "no releases yet"
    print(f"Server {cfg['server']} - church {s['church']}: {kept}")
    if not s["devices"]:
        print("  no laptops registered")
    for d in s["devices"]:
        state = "connected" if d["connected"] else "offline"
        version = f"v{d['last_version']}" if d["last_version"] is not None else "no release yet"
        print(f"  {d['name']}: {state}, has {version}, last seen {d['last_seen_at']}")


def cmd_diff(args):
    d = compute_delta(args.old, args.new)
    print(json.dumps(d["summary"], indent=2))
    print(f"other tables changed: {d['other_changes']}")
    for row in d["songs"][:20]:
        print(f"  song {row['id']}: {row['title']}")


def plan_pushes(available, server_latest, current):
    """Consecutive (from, to) pairs to push. When the server is behind by more
    than the snapshots kept here, only the newest steps go; laptops that are
    further behind take the snapshot."""
    pairs = []
    v = current
    while v > server_latest and (v - 1) in available and v in available:
        pairs.append((v - 1, v))
        v -= 1
    return list(reversed(pairs))


def cmd_push(args):
    cfg = load_config()
    publish_dir = Path(args.publish_dir)
    manifest = json.loads((publish_dir / "manifest.json").read_text("utf-8"))
    current = int(manifest["content_version"])
    status = request(cfg, "GET", "/status")
    server_latest = int(status["latest"])
    if server_latest >= current:
        print(f"Server already has v{server_latest}; nothing to push.")
        return
    files = snapshot_files(publish_dir)
    pairs = plan_pushes(files, server_latest, current)
    if not pairs:
        print(f"No consecutive snapshots to compare for v{current}; laptops take the snapshot.")
        return
    key = load_key()
    parts = []
    for from_v, to_v in pairs:
        delta = compute_delta(files[from_v], files[to_v])
        sha = manifest["sha256"] if to_v == current else file_sha256(files[to_v])
        release = build_parts(delta, cfg["church"], from_v, to_v, files[to_v].name, sha, key)
        parts.extend(release)
        s = delta["summary"]
        print(f"v{from_v} -> v{to_v}: {s['songs_changed']} changed, {s['songs_added']} added, "
              f"{s['songs_deleted']} removed songs; {s['songbooks_changed']} songbook changes; "
              f"{len(release)} part(s)" + ("; other tables wait for the snapshot" if delta["other_changes"] else ""))
    r = request(cfg, "POST", "/releases", {"parts": parts})
    print(f"PUSH OK: server at v{r['latest']}, sent to {r['connected']} connected laptop(s)")


# ---------------------------------------------------------------------------
# App releases (docs/song-push.md, "App updates")

# Files in a build folder that belong to a laptop, never to a release.
LOCAL_FILE = re.compile(r"(?i)^(spData.*|.*\.sqlite.*|version\.txt|cgf-(cloud|network|versions|healthy)\..*|"
                        r".*\.log|releases\.json|.*\.tmp)$")
LOCAL_DIRS = {"staging"}
APP_FOLDER = re.compile(r"^app-[A-Za-z0-9._-]{1,40}$")
REQUIRED = ["CGFProjector.exe", "cgf-companion.exe", "Qt6Core.dll", "tls/qschannelbackend.dll"]


def app_files(build_dir):
    """Every program file of a deployed build (windeployqt output), as
    (relative path with '/', absolute path)."""
    build_dir = Path(build_dir)
    out = []
    for p in sorted(build_dir.rglob("*")):
        if not p.is_file():
            continue
        rel = p.relative_to(build_dir).as_posix()
        parts = rel.split("/")
        if parts[0] in LOCAL_DIRS or (len(parts) == 1 and LOCAL_FILE.match(parts[0])):
            continue
        out.append((rel, p))
    return out


def qt_compress(data):
    """The format Qt's qUncompress reads: original size (4 bytes, big-endian) + zlib."""
    import struct
    import zlib
    return struct.pack(">I", len(data)) + zlib.compress(data, 9)


def build_app_release(files, church, seq, version, notes, key, launcher=None):
    payload = {
        "kind": "cgf-app", "v": 1, "church": church, "seq": seq, "version": version,
        "folder": f"app-{version}", "notes": notes,
        "published_at": datetime.datetime.now(datetime.timezone.utc).isoformat(timespec="seconds"),
        "files": [{"path": rel, "sha256": sha, "bytes": size} for rel, sha, size in files],
    }
    if launcher:
        payload["launcher"] = {"sha256": launcher[0], "bytes": launcher[1]}
    text = json.dumps(payload, ensure_ascii=False, separators=(",", ":"))
    return {"type": "app", "payload": text, "sig": sign(key, text), "key": key_id(key)}


def cmd_app_release(args):
    cfg = load_config()
    build = Path(args.build_dir)
    for r in REQUIRED:
        if not (build / r).exists():
            raise SystemExit(f"{build} has no {r}: build and deploy first (BUILD.md)")
    if not APP_FOLDER.match(f"app-{args.version}"):
        raise SystemExit("A version may hold letters, digits, dots, dashes and underscores only")
    notes = Path(args.notes_file).read_text("utf-8").strip() if args.notes_file else (args.notes or "")
    files = []
    blobs = {}
    for rel, path in app_files(build):
        data = path.read_bytes()
        sha = hashlib.sha256(data).hexdigest()
        files.append((rel, sha, len(data)))
        blobs[sha] = path
    launcher = None
    if args.launcher:
        data = Path(args.launcher).read_bytes()
        sha = hashlib.sha256(data).hexdigest()
        launcher = (sha, len(data))
        blobs[sha] = Path(args.launcher)

    status = request(cfg, "GET", "/status")
    seq = ((status.get("app") or {}).get("seq") or 0) + 1
    missing = request(cfg, "POST", "/app/files/missing", {"sha256": sorted(blobs)})["missing"]
    total = sum(blobs[s].stat().st_size for s in missing)
    print(f"app-{args.version}: {len(files)} files, {len(missing)} to upload ({total / 1e6:.1f} MB before compression)")
    for i, sha in enumerate(missing, 1):
        request(cfg, "PUT", f"/app/files/{sha}", raw=qt_compress(blobs[sha].read_bytes()))
        if i % 20 == 0 or i == len(missing):
            print(f"  uploaded {i}/{len(missing)}")
    release = build_app_release(files, cfg["church"], seq, args.version, notes, load_key(), launcher)
    r = request(cfg, "POST", "/app/releases", {"release": release})
    print(f"APP RELEASE OK: app-{args.version} is release {r['seq']}, sent to {r['connected']} connected laptop(s)")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    p = sub.add_parser("init"); p.add_argument("--server", required=True); p.add_argument("--church", default="cgf-vernon")
    p.set_defaults(fn=cmd_init)
    p = sub.add_parser("server-secret"); p.add_argument("--wrangler-dir", required=True); p.set_defaults(fn=cmd_server_secret)
    p = sub.add_parser("add-device"); p.add_argument("name"); p.add_argument("--out", required=True)
    p.set_defaults(fn=cmd_add_device)
    p = sub.add_parser("remove-device"); p.add_argument("name"); p.set_defaults(fn=cmd_remove_device)
    p = sub.add_parser("push"); p.add_argument("--publish-dir", required=True); p.set_defaults(fn=cmd_push)
    p = sub.add_parser("status"); p.set_defaults(fn=cmd_status)
    p = sub.add_parser("diff"); p.add_argument("old"); p.add_argument("new"); p.set_defaults(fn=cmd_diff)
    p = sub.add_parser("app-release", help="publish a new app build to the laptops")
    p.add_argument("--build-dir", required=True, help="the deployed build (src/win32_build/bin)")
    p.add_argument("--version", required=True, help="for example 0.2.1; the laptop folder becomes app-<version>")
    p.add_argument("--notes", help="what changed, shown in the app")
    p.add_argument("--notes-file", help="the same, from a text file")
    p.add_argument("--launcher", help="the launcher exe (src/win32_build/launcher/CGFProjector.exe)")
    p.set_defaults(fn=cmd_app_release)
    args = ap.parse_args(argv)
    args.fn(args)


if __name__ == "__main__":
    main()

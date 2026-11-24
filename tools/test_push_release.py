"""Checks for push_release.py. Builds two tiny throwaway snapshots at run time
(made-up songs, no church data) and checks the delta, the parts and the
signatures.

  python tools/test_push_release.py
"""
import base64
import json
import sqlite3
import sys
import tempfile
import unittest
from pathlib import Path

from cryptography.exceptions import InvalidSignature
from cryptography.hazmat.primitives import hashes
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.hazmat.primitives.asymmetric.utils import encode_dss_signature

sys.path.insert(0, str(Path(__file__).resolve().parent))
import push_release as pr  # noqa: E402

SCHEMA = """
CREATE TABLE 'Songbooks' ('id' INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, 'name' TEXT, 'info' TEXT);
CREATE TABLE 'Songs' ('id' INTEGER PRIMARY KEY AUTOINCREMENT NOT NULL, 'songbook_id' INTEGER, 'number' INTEGER,
  'title' TEXT, 'category' INTEGER DEFAULT 0, 'tune' TEXT, 'words' TEXT, 'music' TEXT, 'song_text' TEXT,
  'notes' TEXT, 'use_private' BOOL, 'alignment_v' INTEGER, 'alignment_h' INTEGER, 'color' INTEGER, 'font' TEXT,
  'info_color' INTEGER, 'info_font' TEXT, 'ending_color' INTEGER, 'ending_font' TEXT, 'use_background' BOOL,
  'background_name' TEXT, 'background' BLOB, 'count' INTEGER DEFAULT 0, 'date' TEXT);
CREATE TABLE 'BibleVerse' ('verse_id' TEXT, 'bible_id' TEXT, 'verse_text' TEXT);
"""


def make_db(path, songbooks, songs, verses=()):
    con = sqlite3.connect(path)
    con.executescript(SCHEMA)
    con.executemany("INSERT INTO Songbooks (id, name, info) VALUES (?,?,'')", songbooks)
    for s in songs:
        con.execute("INSERT INTO Songs (id, songbook_id, number, title, song_text, color, background, count, date) "
                    "VALUES (?,?,?,?,?,?,?,?,?)", s)
    con.executemany("INSERT INTO BibleVerse VALUES (?,?,?)", verses)
    con.commit()
    con.close()


def verify(public_b64, payload, sig_b64):
    raw = base64.b64decode(public_b64)
    pub = ec.EllipticCurvePublicNumbers(int.from_bytes(raw[:32], "big"), int.from_bytes(raw[32:], "big"),
                                        ec.SECP256R1()).public_key()
    sig = base64.b64decode(sig_b64)
    der = encode_dss_signature(int.from_bytes(sig[:32], "big"), int.from_bytes(sig[32:], "big"))
    pub.verify(der, payload.encode("utf-8"), ec.ECDSA(hashes.SHA256()))


class DeltaTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        d = Path(self.tmp.name)
        self.old, self.new = d / "spData-cgf-v1.sqlite", d / "spData-cgf-v2.sqlite"
        books = [(1, "Test Book A"), (2, "Test Book B")]
        make_db(self.old, books, [
            (1, 1, 1, "Test song one", "Verse 1\nla la", 16777215, None, 5, "2026-01-01"),
            (2, 1, 2, "Test song two", "Verse 1\nlo lo", None, None, 0, None),
            (3, 2, 1, "Test song three", "Verse 1\nli li", None, None, 0, None),
            (500001, 1, 9, "A laptop's own song", "x", None, None, 0, None),
        ], [("B1", "1", "In the beginning")])
        make_db(self.new, books + [(3, "Test Book C")], [
            (1, 1, 1, "Test song one", "Verse 1\nla la", 16777215, None, 9, "2026-02-02"),  # only usage changed
            (2, 1, 2, "Test song two (edited)", "Verse 1\nlo lo lo", None, None, 0, None),
            (4, 3, 1, "Test song four", "Verse 1\nnew", None, b"\x89PNG", 0, None),
        ], [("B1", "1", "In the beginning")])
        self.key = ec.generate_private_key(ec.SECP256R1())

    def tearDown(self):
        self.tmp.cleanup()

    def test_delta_lists_changed_added_and_removed_songs(self):
        d = pr.compute_delta(self.old, self.new)
        self.assertEqual([r["id"] for r in d["songs"]], [2, 4])
        self.assertEqual(d["songs_deleted"], [3])
        self.assertEqual([b["id"] for b in d["songbooks"]], [3])
        self.assertEqual(d["summary"]["songs_added"], 1)
        self.assertEqual(d["summary"]["songs_changed"], 1)
        self.assertFalse(d["other_changes"])
        self.assertEqual(d["songs"][1]["background"], {"$b64": base64.b64encode(b"\x89PNG").decode()})

    def test_local_songs_and_usage_counts_never_travel(self):
        d = pr.compute_delta(self.old, self.new)
        ids = [r["id"] for r in d["songs"]] + d["songs_deleted"]
        self.assertNotIn(500001, ids)
        self.assertNotIn(1, ids)

    def test_other_tables_are_flagged(self):
        con = sqlite3.connect(self.new)
        con.execute("UPDATE BibleVerse SET verse_text = 'changed'")
        con.commit()
        con.close()
        self.assertTrue(pr.compute_delta(self.old, self.new)["other_changes"])

    def test_parts_are_signed_and_split(self):
        d = pr.compute_delta(self.old, self.new)
        saved = pr.PART_BUDGET
        pr.PART_BUDGET = 10  # one song per part
        try:
            parts = pr.build_parts(d, "test-church", 1, 2, "spData-cgf-v2.sqlite", "abc", self.key)
        finally:
            pr.PART_BUDGET = saved
        self.assertEqual(len(parts), 2)
        public = pr.public_key_b64(self.key)
        for i, p in enumerate(parts, start=1):
            verify(public, p["payload"], p["sig"])
            body = json.loads(p["payload"])
            self.assertEqual((body["from"], body["to"], body["part"], body["parts"]), (1, 2, i, 2))
            self.assertEqual(p["key"], pr.key_id(self.key))
        first, second = (json.loads(p["payload"]) for p in parts)
        self.assertEqual(first["songs_deleted"], [3])
        self.assertEqual(second["songs_deleted"], [])
        self.assertEqual([b["id"] for b in first["songbooks"]], [3])

    def test_a_changed_payload_fails_the_signature(self):
        parts = pr.build_parts(pr.compute_delta(self.old, self.new), "c", 1, 2, "f", "s", self.key)
        tampered = parts[0]["payload"].replace("edited", "forged")
        with self.assertRaises(InvalidSignature):
            verify(pr.public_key_b64(self.key), tampered, parts[0]["sig"])


class AppReleaseTests(unittest.TestCase):
    def test_app_files_leave_out_the_laptops_data(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            for name in ["CGFProjector.exe", "Qt6Core.dll", "spData.sqlite", "spData.sqlite.prev-1", "version.txt",
                         "cgf-cloud.json", "cgf-network.json", "CGFProjector.log", "platforms/qwindows.dll",
                         "staging/manifest.json", "translations/qt_de.qm"]:
                (d / name).parent.mkdir(parents=True, exist_ok=True)
                (d / name).write_bytes(b"x")
            self.assertEqual([r for r, _ in pr.app_files(d)],
                             ["CGFProjector.exe", "platforms/qwindows.dll", "Qt6Core.dll", "translations/qt_de.qm"])

    def test_files_travel_as_qt_compress(self):
        import struct
        import zlib
        data = b"program bytes " * 100
        packed = pr.qt_compress(data)
        self.assertEqual(struct.unpack(">I", packed[:4])[0], len(data))
        self.assertEqual(zlib.decompress(packed[4:]), data)

    def test_an_app_release_is_signed(self):
        key = ec.generate_private_key(ec.SECP256R1())
        r = pr.build_app_release([("CGFProjector.exe", "a" * 64, 10)], "c", 3, "0.2.1", "notes", key, ("b" * 64, 5))
        verify(pr.public_key_b64(key), r["payload"], r["sig"])
        body = json.loads(r["payload"])
        self.assertEqual((body["seq"], body["folder"], body["launcher"]["sha256"]), (3, "app-0.2.1", "b" * 64))


class PlanTests(unittest.TestCase):
    def test_plans_consecutive_steps_the_server_lacks(self):
        files = {1: "a", 2: "b", 3: "c", 4: "d"}
        self.assertEqual(pr.plan_pushes(files, 2, 4), [(2, 3), (3, 4)])
        self.assertEqual(pr.plan_pushes(files, 0, 4), [(1, 2), (2, 3), (3, 4)])
        self.assertEqual(pr.plan_pushes(files, 4, 4), [])

    def test_stops_at_a_missing_snapshot(self):
        self.assertEqual(pr.plan_pushes({3: "c", 5: "e", 6: "f"}, 2, 6), [(5, 6)])

    def test_device_address(self):
        self.assertEqual(pr.device_url({"server": "https://push.example/", "church": "c"}),
                         "wss://push.example/v1/c/ws")
        self.assertEqual(pr.device_url({"server": "http://127.0.0.1:8787", "church": "c"}),
                         "ws://127.0.0.1:8787/v1/c/ws")


if __name__ == "__main__":
    unittest.main()

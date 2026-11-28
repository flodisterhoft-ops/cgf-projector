"""Tests for pull_proposals.py: which proposals need work, and pulling them.

    python tools/test_pull_proposals.py
"""
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import pull_proposals as pp  # noqa: E402


def row(uuid, state, undo=False):
    return {"uuid": uuid, "state": state, "undo": undo, "kind": "edit", "song_id": 7, "device": "Booth",
            "received_at": "2026-11-28T10:00:00Z", "proposal": {"proposed": {"title": "T"}}}


class FakeServer:
    def __init__(self, rows):
        self.rows = {r["uuid"]: r for r in rows}
        self.posts = []

    def request(self, cfg, method, path, body=None):
        if method == "GET":
            return {"proposals": list(self.rows.values())}
        uuid = path.rsplit("/", 1)[1]
        self.posts.append((uuid, body))
        self.rows[uuid] = {**self.rows[uuid], **body}
        return self.rows[uuid]


class PullTests(unittest.TestCase):
    def test_needs_work(self):
        self.assertTrue(pp.needs_work(row("a", "new")))
        self.assertTrue(pp.needs_work(row("a", "received", undo=True)))  # skip it, then mark undone
        self.assertTrue(pp.needs_work(row("a", "applied", undo=True)))  # the master must go back
        self.assertTrue(pp.needs_work(row("a", "undone", undo=False)))  # kept after all
        self.assertFalse(pp.needs_work(row("a", "applied")))
        self.assertFalse(pp.needs_work(row("a", "undone", undo=True)))
        self.assertFalse(pp.needs_work(row("a", "replaced")))

    def test_pull_writes_files_and_marks_new_ones_received(self):
        server = FakeServer([row("n1", "new"), row("a1", "applied"), row("a2", "applied", undo=True)])
        pp.request = server.request
        with tempfile.TemporaryDirectory() as d:
            got = pp.pull({}, d)
            self.assertEqual(sorted(p["uuid"] for p in got), ["a2", "n1"])
            self.assertEqual(server.posts, [("n1", {"state": "received"})])
            self.assertEqual(json.loads((Path(d) / "n1.json").read_text("utf-8"))["state"], "received")
            self.assertTrue((Path(d) / "a2.json").exists())
            self.assertFalse((Path(d) / "a1.json").exists())
            self.assertEqual([p.name for p in Path(d).glob("*.partial")], [])
            # A second pull finds n1 received and writes it again, without marking.
            pp.pull({}, d)
            self.assertEqual(len(server.posts), 1)


if __name__ == "__main__":
    unittest.main()

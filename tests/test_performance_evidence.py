"""Reject incomplete or unsuccessful transaction evidence before publication."""
import csv
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
from benchmark import summarize
from summarize_performance import verified_profile, radio_comparison


class PerformanceEvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "session-test").mkdir()
        self.csv = self.root / "session-test/performance-raw.csv"
        cases = ["ping", "read_16", "read_64", "read_128", "read_240", "identical_write_240"]
        self.rows = [dict(case=case, round="0", iteration=str(i), status="ok", latency_ms="2.0")
                     for case in cases for i in range(2)]
        self.profile = dict(passed=True, restore_verified=True, rollback_session="session-test",
            label="candidate_1000000", hex_sha256="test-image", baudrate=1000000,
            measurements=dict(rounds=1, samples_per_round=2, revision=20261114, scratch_sha256="test-data",
                cases={case: [dict(duration_seconds=.004)] for case in cases},
                aggregate={case: summarize([r for r in self.rows if r["case"] == case], .004) for case in cases}))

    def write_rows(self, rows):
        with self.csv.open("w", newline="", encoding="utf-8") as stream:
            writer = csv.DictWriter(stream, fieldnames=["case", "round", "iteration", "status", "latency_ms"])
            writer.writeheader()
            writer.writerows(rows)

    def test_complete_evidence_is_recomputed(self):
        self.write_rows(self.rows)
        result = verified_profile(self.profile, self.root)
        self.assertEqual(result["measured_transactions"], 12)
        self.assertEqual(result["cases"]["read_240"]["latency_ms"]["median"], 2)

    def test_missing_and_duplicate_observations_are_rejected(self):
        for rows in [self.rows[:-1], self.rows + self.rows[:1], self.rows[:-1] + self.rows[:1]]:
            with self.subTest(rows=len(rows)):
                self.write_rows(rows)
                with self.assertRaises(ValueError):
                    verified_profile(self.profile, self.root)

    def test_failed_or_changed_observation_cannot_reuse_summary(self):
        for field, value in [("status", "timeout"), ("latency_ms", "200")]:
            rows = [dict(r) for r in self.rows]
            rows[0][field] = value
            self.write_rows(rows)
            with self.assertRaises(ValueError):
                verified_profile(self.profile, self.root)

    def test_unverified_rollback_is_rejected(self):
        self.write_rows(self.rows)
        self.profile["restore_verified"] = False
        with self.assertRaises(ValueError):
            verified_profile(self.profile, self.root)

    def test_radio_evidence_from_another_image_is_rejected(self):
        proof = dict(radio={}, experimental_hex_sha256="other-image", baudrate=115200)
        path = self.root / "radio.json"
        path.write_text(json.dumps(proof), encoding="utf-8")
        with self.assertRaises(ValueError):
            radio_comparison(path, path, [self.profile])


if __name__ == "__main__": unittest.main()

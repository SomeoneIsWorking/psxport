#!/usr/bin/env python3
"""Hermetic tests for the override-differential gate: every failure class must fail, and a clean
report must pass. The passing report is the control that makes each failing case mean something."""

from __future__ import annotations

import copy
import json
import shutil
import tempfile
import unittest
from pathlib import Path
from typing import Any

from override_differential_gate import SCHEMA, ReportError, evaluate, load_report, main


def key(name: str = "sum", address: str = "0x80010100", *, seen: int = 20, match: int = 17,
        mismatch: int = 0, incomparable: int = 0, reasons: dict[str, int] | None = None) -> dict[str, Any]:
    return {
        "name": name,
        "address": address,
        "image_id": 1,
        "image_generation": 1,
        "calls_seen": seen,
        "sampled": match + mismatch + incomparable,
        "match": match,
        "mismatch": mismatch,
        "incomparable": incomparable,
        "incomparable_by_reason": reasons or {},
        "dead_stack_bytes_ignored": 0,
        "restored_ranges": 0,
        "first_mismatch": None if not mismatch else {
            "call": 3, "what": "register v0", "original": "0x00000001", "native": "0x00000002",
        },
    }


def report(keys: list[dict[str, Any]], selectors: list[dict[str, Any]] | None = None,
           complete: bool = True) -> dict[str, Any]:
    if selectors is None:
        selectors = [{"selector": "sum", "address": None, "keys_matched": 1,
                      "sampled": sum(k["sampled"] for k in keys if k["name"] == "sum")}]
    return {"schema": SCHEMA, "complete": complete, "selectors": selectors, "keys": keys, "failures": []}


class GateTests(unittest.TestCase):
    def verdict(self, data: dict[str, Any], **options: Any) -> list[str]:
        return evaluate(data, required=options.get("required", []),
                        allow_incomplete=options.get("allow_incomplete", False)).failures

    def test_a_clean_report_passes(self) -> None:
        self.assertEqual(self.verdict(report([key()])), [])

    def test_a_mismatch_fails_and_names_the_first_difference(self) -> None:
        failures = self.verdict(report([key(match=16, mismatch=1)]))
        self.assertEqual(len(failures), 1)
        self.assertIn("register v0 original 0x00000001 native 0x00000002", failures[0])

    def test_zero_sampled_calls_fail(self) -> None:
        data = report([], selectors=[{"selector": "sum", "address": None, "keys_matched": 0, "sampled": 0}])
        self.assertTrue(any("sampled 0 calls" in failure for failure in self.verdict(data)))

    def test_all_incomparable_samples_fail(self) -> None:
        data = report([key(match=0, incomparable=4, reasons={"original path performed syscall": 4})])
        self.assertTrue(any("none could be compared" in failure for failure in self.verdict(data)))

    def test_some_incomparable_samples_with_matches_pass(self) -> None:
        data = report([key(match=3, incomparable=4, reasons={"original path performed syscall": 4})])
        self.assertEqual(self.verdict(data), [])

    def test_an_incomplete_report_fails_unless_allowed(self) -> None:
        data = report([key()], complete=False)
        self.assertTrue(any("incomplete" in failure for failure in self.verdict(data)))
        self.assertEqual(self.verdict(data, allow_incomplete=True), [])

    def test_a_required_selector_missing_from_the_report_fails(self) -> None:
        failures = self.verdict(report([key()]), required=["other"])
        self.assertTrue(any("'other' is not in this report" in failure for failure in failures))

    def test_an_address_selector_counts_keys_at_that_address(self) -> None:
        data = report([key(name="sum", address="0x80010100")],
                      selectors=[{"selector": "0x80010100", "address": "0x80010100", "keys_matched": 1,
                                  "sampled": 17}])
        self.assertEqual(self.verdict(data), [])

    def test_the_gate_recomputes_rather_than_trusting_the_report(self) -> None:
        # The selector claims samples the keys do not have; the report's own empty failure list must not
        # be believed.
        data = report([key(match=0)], selectors=[{"selector": "sum", "address": None, "keys_matched": 1,
                                                  "sampled": 5}])
        failures = self.verdict(data)
        self.assertTrue(any("keys sum to 0" in failure for failure in failures))
        self.assertTrue(any("sampled 0 calls" in failure for failure in failures))

    def test_inconsistent_key_counts_fail(self) -> None:
        bad = key()
        bad["sampled"] = 99
        data = report([bad], selectors=[{"selector": "sum", "address": None, "keys_matched": 1, "sampled": 99}])
        self.assertTrue(any("inconsistent counts" in failure for failure in self.verdict(data)))

    def test_a_report_with_no_selectors_fails(self) -> None:
        self.assertTrue(any("gates nothing" in f for f in self.verdict(report([], selectors=[]))))

    def test_malformed_reports_are_refused(self) -> None:
        broken = copy.deepcopy(report([key()]))
        broken["keys"][0]["match"] = -1
        with self.assertRaises(ReportError):
            evaluate(broken, required=[], allow_incomplete=False)


class CliTests(unittest.TestCase):
    def setUp(self) -> None:
        directory = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, directory, ignore_errors=True)
        self.directory = directory

    def write(self, data: object) -> Path:
        path = self.directory / "report.json"
        path.write_text(json.dumps(data))
        return path

    def test_exit_codes(self) -> None:
        self.assertEqual(main([str(self.write(report([key()])))]), 0)
        self.assertEqual(main([str(self.write(report([key(match=1, mismatch=1)])))]), 1)
        self.assertEqual(main([str(self.directory / "absent.json")]), 2)
        self.assertEqual(main([str(self.write({"schema": "other"}))]), 2)

    def test_load_report_refuses_non_json(self) -> None:
        path = self.directory / "report.json"
        path.write_text("not json")
        with self.assertRaises(ReportError):
            load_report(path)


if __name__ == "__main__":
    unittest.main()

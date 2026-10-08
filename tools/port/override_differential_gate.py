#!/usr/bin/env python3
"""Gate a native override on the override differential's JSON report.

The report is written by ``runtime/cpu/override_differential.h`` during a real run armed with
``PSXPORT_OVERRIDE_DIFF``. This gate re-derives every verdict from the raw per-key counts instead of
trusting the report's own ``failures`` list, and FAILS when:

* any shadowed call mismatched;
* a requested selector sampled zero calls, or sampled calls none of which could be compared (every one
  incomparable) -- zero evidence is a failure, never a pass;
* a selector passed with ``--require`` is absent from the report (a report for a different run);
* the report is incomplete (the run did not shut down cleanly) unless ``--allow-incomplete``;
* the report's counts are internally inconsistent.

Exit status: 0 pass, 1 gate failure, 2 unreadable or malformed report.

    uv run --frozen python tools/port/override_differential_gate.py scratch/override_differential.json \\
        --require myOverrideName
"""

from __future__ import annotations

import argparse
import json
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

SCHEMA = "psxport.override-differential/1"


class ReportError(Exception):
    """The report cannot be read as an override-differential report."""


@dataclass(frozen=True)
class KeyCounts:
    name: str
    address: int
    calls_seen: int
    sampled: int
    match: int
    mismatch: int
    incomparable: int
    incomparable_by_reason: dict[str, int]
    first_mismatch: dict[str, Any] | None


@dataclass(frozen=True)
class Selector:
    text: str
    address: int | None
    reported_sampled: int


@dataclass
class GateResult:
    failures: list[str] = field(default_factory=list)
    lines: list[str] = field(default_factory=list)

    @property
    def passed(self) -> bool:
        return not self.failures


def _int(value: Any, what: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool) or value < 0:
        raise ReportError(f"{what} is not a non-negative integer: {value!r}")
    return value


def _address(value: Any, what: str) -> int:
    if not isinstance(value, str) or not value.lower().startswith("0x"):
        raise ReportError(f"{what} is not a 0x-prefixed address: {value!r}")
    try:
        return int(value, 16)
    except ValueError as error:
        raise ReportError(f"{what} is not a hex address: {value!r}") from error


def parse_key(raw: Any) -> KeyCounts:
    if not isinstance(raw, dict):
        raise ReportError(f"key entry is not an object: {raw!r}")
    reasons = raw.get("incomparable_by_reason")
    if not isinstance(reasons, dict):
        raise ReportError(f"key {raw.get('name')!r} has no incomparable_by_reason object")
    name = raw.get("name")
    if not isinstance(name, str):
        raise ReportError(f"key entry has no name: {raw!r}")
    return KeyCounts(
        name=name,
        address=_address(raw.get("address"), f"key {name!r} address"),
        calls_seen=_int(raw.get("calls_seen"), f"key {name!r} calls_seen"),
        sampled=_int(raw.get("sampled"), f"key {name!r} sampled"),
        match=_int(raw.get("match"), f"key {name!r} match"),
        mismatch=_int(raw.get("mismatch"), f"key {name!r} mismatch"),
        incomparable=_int(raw.get("incomparable"), f"key {name!r} incomparable"),
        incomparable_by_reason={str(k): _int(v, f"key {name!r} reason count") for k, v in reasons.items()},
        first_mismatch=raw.get("first_mismatch"),
    )


def parse_selector(raw: Any) -> Selector:
    if not isinstance(raw, dict) or not isinstance(raw.get("selector"), str):
        raise ReportError(f"selector entry is malformed: {raw!r}")
    address = raw.get("address")
    return Selector(
        text=raw["selector"],
        address=None if address is None else _address(address, f"selector {raw['selector']!r} address"),
        reported_sampled=_int(raw.get("sampled"), f"selector {raw['selector']!r} sampled"),
    )


def load_report(path: Path) -> dict[str, Any]:
    try:
        report = json.loads(path.read_text())
    except FileNotFoundError as error:
        raise ReportError(f"no report at {path}: the run was not armed or never wrote one") from error
    except json.JSONDecodeError as error:
        raise ReportError(f"{path} is not JSON: {error}") from error
    if not isinstance(report, dict):
        raise ReportError(f"{path} is not a JSON object")
    if report.get("schema") != SCHEMA:
        raise ReportError(f"{path} has schema {report.get('schema')!r}, expected {SCHEMA!r}")
    return report


def _selects(selector: Selector, key: KeyCounts) -> bool:
    return key.address == selector.address if selector.address is not None else key.name == selector.text


def evaluate(report: dict[str, Any], *, required: list[str], allow_incomplete: bool) -> GateResult:
    """Re-derive the gate verdict from the report's raw counts."""

    raw_keys = report.get("keys")
    raw_selectors = report.get("selectors")
    if not isinstance(raw_keys, list) or not isinstance(raw_selectors, list):
        raise ReportError("report has no keys/selectors arrays")
    keys = [parse_key(raw) for raw in raw_keys]
    selectors = [parse_selector(raw) for raw in raw_selectors]
    result = GateResult()

    if report.get("complete") is not True and not allow_incomplete:
        result.failures.append("report is incomplete: the run did not shut down and write its final report")
    if not selectors:
        result.failures.append("report requests no selectors, so it gates nothing")
    for name in required:
        if not any(selector.text == name for selector in selectors):
            result.failures.append(f"required selector {name!r} is not in this report")

    for key in keys:
        verdicts = key.match + key.mismatch + key.incomparable
        if verdicts != key.sampled or key.sampled > key.calls_seen:
            result.failures.append(
                f"{key.name} @0x{key.address:08X}: inconsistent counts (seen {key.calls_seen}, sampled "
                f"{key.sampled}, verdicts {verdicts})"
            )
        reasons = "; ".join(f"{reason} x{count}" for reason, count in sorted(key.incomparable_by_reason.items()))
        result.lines.append(
            f"{key.name} @0x{key.address:08X}: {key.calls_seen} seen, {key.sampled} sampled: {key.match} match, "
            f"{key.mismatch} mismatch, {key.incomparable} incomparable" + (f" ({reasons})" if reasons else "")
        )
        if key.mismatch:
            first = key.first_mismatch or {}
            result.failures.append(
                f"{key.name} @0x{key.address:08X}: {key.mismatch} of {key.sampled} sampled call(s) mismatched; "
                f"first at call {first.get('call')}: {first.get('what')} original {first.get('original')} "
                f"native {first.get('native')}"
            )

    for selector in selectors:
        matched = [key for key in keys if _selects(selector, key)]
        sampled = sum(key.sampled for key in matched)
        compared = sum(key.match + key.mismatch for key in matched)
        if sampled != selector.reported_sampled:
            result.failures.append(
                f"selector {selector.text!r}: report says {selector.reported_sampled} sampled, keys sum to {sampled}"
            )
        if sampled == 0:
            result.failures.append(
                f"selector {selector.text!r} sampled 0 calls ({len(matched)} matching override key(s) called): "
                "no evidence, which is a failure"
            )
        elif compared == 0:
            result.failures.append(
                f"selector {selector.text!r} sampled {sampled} call(s) and none could be compared (all incomparable)"
            )
    return result


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("report", type=Path, help="the JSON report the armed run wrote")
    parser.add_argument("--require", action="append", default=[], metavar="SELECTOR",
                        help="a selector (override name or 0x address) that must be in the report; repeatable")
    parser.add_argument("--allow-incomplete", action="store_true",
                        help="accept a report written mid-run (the final shutdown write never happened)")
    args = parser.parse_args(argv)
    try:
        result = evaluate(load_report(args.report), required=args.require, allow_incomplete=args.allow_incomplete)
    except ReportError as error:
        print(f"override-differential gate: REFUSED: {error}", file=sys.stderr)
        return 2
    for line in result.lines:
        print(line)
    for failure in result.failures:
        print(f"FAIL: {failure}", file=sys.stderr)
    print(f"override-differential gate: {'PASS' if result.passed else 'FAIL'} "
          f"({len(result.failures)} failure(s), {len(result.lines)} key(s))")
    return 0 if result.passed else 1


if __name__ == "__main__":
    raise SystemExit(main())

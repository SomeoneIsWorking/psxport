#!/usr/bin/env python3
"""Both-answer regression for oracle_trace call capture and explicit modeled return."""

from __future__ import annotations

import argparse
import pathlib
import re
import struct
import subprocess
import sys


LOAD_ADDRESS = 0x80010000
FUNCTION_ONE = LOAD_ADDRESS + 0x40
FUNCTION_TWO = LOAD_ADDRESS + 0x50
BIOS_THUNK = LOAD_ADDRESS + 0x60
POST_MODEL_CALL = LOAD_ADDRESS + 0x70


def jal(target: int) -> int:
    return (0x03 << 26) | ((target >> 2) & 0x03FFFFFF)


def addiu(rt: int, rs: int, immediate: int) -> int:
    return (0x09 << 26) | (rs << 21) | (rt << 16) | (immediate & 0xFFFF)


def jr(rs: int) -> int:
    return (rs << 21) | 0x08


def make_executable(path: pathlib.Path) -> None:
    words = [0] * 32
    words[0] = jal(FUNCTION_ONE)
    words[1] = 0
    words[2] = jal(FUNCTION_TWO)
    words[3] = addiu(8, 0, 0x2222)
    words[16] = jr(31)
    words[17] = 0
    words[20] = addiu(9, 0, 0x3333)

    payload = b"".join(struct.pack("<I", word) for word in words)
    header = bytearray(0x800)
    header[:8] = b"PS-X EXE"
    struct.pack_into("<I", header, 0x10, LOAD_ADDRESS)
    struct.pack_into("<I", header, 0x18, LOAD_ADDRESS)
    struct.pack_into("<I", header, 0x1C, len(payload))
    struct.pack_into("<I", header, 0x30, 0x801FFF00)
    path.write_bytes(header + payload)


def make_modeled_return_executable(path: pathlib.Path) -> None:
    words = [0] * 40
    words[0] = jal(BIOS_THUNK)
    words[1] = 0
    words[2] = jal(POST_MODEL_CALL)
    words[3] = 0
    words[24] = addiu(10, 0, 0xA0)
    words[25] = jr(10)
    words[26] = addiu(9, 0, 0x39)
    words[28] = addiu(8, 0, 0x4444)

    payload = b"".join(struct.pack("<I", word) for word in words)
    header = bytearray(0x800)
    header[:8] = b"PS-X EXE"
    struct.pack_into("<I", header, 0x10, LOAD_ADDRESS)
    struct.pack_into("<I", header, 0x18, LOAD_ADDRESS)
    struct.pack_into("<I", header, 0x1C, len(payload))
    struct.pack_into("<I", header, 0x30, 0x801FFF00)
    path.write_bytes(header + payload)


def run_capture(tracer: pathlib.Path, executable: pathlib.Path, trace: pathlib.Path, ordinal: int) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            str(tracer),
            str(executable),
            "--steps",
            "64",
            "--capture-call",
            str(ordinal),
            "--summary-only",
            "--out",
            str(trace),
        ],
        capture_output=True,
        text=True,
        check=False,
        timeout=20,
    )


def run_first_call_alias(
    tracer: pathlib.Path, executable: pathlib.Path, trace: pathlib.Path
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            str(tracer),
            str(executable),
            "--steps",
            "64",
            "--capture-first-call",
            "--summary-only",
            "--out",
            str(trace),
        ],
        capture_output=True,
        text=True,
        check=False,
        timeout=20,
    )


def captured_target(trace: pathlib.Path) -> int | None:
    match = re.search(r"^# CAPTURED-CALL target=0x([0-9A-Fa-f]+)\b", trace.read_text(encoding="utf-8"), re.MULTILINE)
    return int(match.group(1), 16) if match else None


def run_modeled_return(
    tracer: pathlib.Path,
    executable: pathlib.Path,
    trace: pathlib.Path,
    function: int,
) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [
            str(tracer),
            str(executable),
            "--steps",
            "64",
            "--capture-call",
            "1",
            "--model-bios-return",
            f"A:0x{function:02X}:0",
            "--summary-only",
            "--out",
            str(trace),
        ],
        capture_output=True,
        text=True,
        check=False,
        timeout=20,
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("tracer", type=pathlib.Path)
    parser.add_argument("--scratch", required=True, type=pathlib.Path)
    arguments = parser.parse_args()

    if not arguments.tracer.is_file():
        print(f"oracle_trace selftest: REFUSING — tracer does not exist: {arguments.tracer}", file=sys.stderr)
        return 2
    arguments.scratch.mkdir(parents=True, exist_ok=True)
    executable = arguments.scratch / "ordinal-capture-fixture.exe"
    make_executable(executable)

    first_trace = arguments.scratch / "ordinal-1.trace"
    alias_trace = arguments.scratch / "first-call-alias.trace"
    second_trace = arguments.scratch / "ordinal-2.trace"
    missing_trace = arguments.scratch / "ordinal-3.trace"
    first = run_capture(arguments.tracer, executable, first_trace, 1)
    alias = run_first_call_alias(arguments.tracer, executable, alias_trace)
    second = run_capture(arguments.tracer, executable, second_trace, 2)
    missing = run_capture(arguments.tracer, executable, missing_trace, 3)

    modeled_executable = arguments.scratch / "modeled-return-fixture.exe"
    make_modeled_return_executable(modeled_executable)
    modeled_trace = arguments.scratch / "modeled-return.trace"
    wrong_model_trace = arguments.scratch / "wrong-modeled-return.trace"
    modeled = run_modeled_return(arguments.tracer, modeled_executable, modeled_trace, 0x39)
    wrong_model = run_modeled_return(arguments.tracer, modeled_executable, wrong_model_trace, 0x38)

    first_target = captured_target(first_trace)
    alias_target = captured_target(alias_trace)
    second_target = captured_target(second_trace)
    missing_text = missing_trace.read_text(encoding="utf-8") if missing_trace.is_file() else ""
    modeled_text = modeled_trace.read_text(encoding="utf-8") if modeled_trace.is_file() else ""
    wrong_model_text = wrong_model_trace.read_text(encoding="utf-8") if wrong_model_trace.is_file() else ""
    checks = [
        ("ordinal 1 captures the first call", first.returncode == 0 and first_target == FUNCTION_ONE),
        (
            "the first-call compatibility alias selects ordinal 1",
            alias.returncode == 0 and alias_target == first_target,
        ),
        ("ordinal 2 captures the second call", second.returncode == 0 and second_target == FUNCTION_TWO),
        ("the two ordinal answers are observably different", first_target != second_target),
        (
            "a captured boundary carries the canonical 33-register block",
            len(re.findall(r"^# CALL-BOUNDARY-REG ", first_trace.read_text(encoding="utf-8"), re.MULTILINE))
            == 33,
        ),
        (
            "insufficient window refuses with its denominator and no boundary block",
            missing.returncode == 2
            and "reached 2 of 3 requested executed jal" in missing.stderr
            and "# CALL-BOUNDARY-REGS" not in missing_text,
        ),
        (
            "explicit BIOS model resumes to the first subsequent call",
            modeled.returncode == 0
            and "# MODELED-BIOS-RETURN table=A function=0x39" in modeled_text
            and f"# POST-RETURN-CAPTURED-CALL target=0x{POST_MODEL_CALL:08X}" in modeled_text,
        ),
        (
            "wrong BIOS function refuses without modeled/post evidence",
            wrong_model.returncode == 2
            and "not requested function 0x38" in wrong_model.stderr
            and "# MODELED-BIOS-RETURN" not in wrong_model_text
            and "# POST-RETURN-CAPTURED-CALL" not in wrong_model_text,
        ),
    ]
    for label, passed in checks:
        print(f"  {'PASS' if passed else 'FAIL'} {label}")
    failed = sum(not passed for _, passed in checks)
    print(f"oracle_trace selftest: {len(checks) - failed}/{len(checks)} passed")
    if failed:
        print("--- ordinal 1 stderr ---", file=sys.stderr)
        print(first.stderr, file=sys.stderr)
        print("--- first-call alias stderr ---", file=sys.stderr)
        print(alias.stderr, file=sys.stderr)
        print("--- ordinal 2 stderr ---", file=sys.stderr)
        print(second.stderr, file=sys.stderr)
        print("--- ordinal 3 stderr ---", file=sys.stderr)
        print(missing.stderr, file=sys.stderr)
        print("--- modeled return stderr ---", file=sys.stderr)
        print(modeled.stderr, file=sys.stderr)
        print("--- wrong modeled return stderr ---", file=sys.stderr)
        print(wrong_model.stderr, file=sys.stderr)
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

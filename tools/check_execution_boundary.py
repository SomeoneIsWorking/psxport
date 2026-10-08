#!/usr/bin/env python3
"""Reject offline guest-source and explicit interpreter-mode ownership in a product target."""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

FORBIDDEN_SOURCE_PARTS = (
    "/generated/",
    "/tools/" + "recomp/",
    "/runtime/psx/interp.cpp",
    "/runtime/psx/selftest.cpp",
    "/runtime/psx/sbs.cpp",
    "/runtime/psx/dualcore.cpp",
    "/runtime/psx/recomp_iface.cpp",
    "/runtime/psx/override_registry.cpp",
    "/runtime/psx/overlay_router.cpp",
    "/runtime/psx/generic_whole_program.cpp",
)

# These are repository ownership violations even when CMake happens not to reference them. Keeping
# a dormant generator beside a dynarec runtime leaves two apparent methodologies and invites the
# static path to be resurrected by the next consumer. A backend-owned bounded interpreter fallback
# is allowed; a standalone product interpreter or engine selector is not.
FORBIDDEN_REPOSITORY_PATHS = (
    "generated",
    "tools/" + "recomp",
    "tools/abi_extract.py",
    "tools/frame_audit.py",
    "tools/interp_dump.py",
    "tools/layout_move.py",
    "tools/port_check.py",
    "tools/port_gen.py",
    "tools/producer_class.py",
    "tools/prof_report.py",
    "tools/psx_port_scaffold.py",
    "runtime/psx/interp.cpp",
    "runtime/psx/interp_diag.h",
    "runtime/psx/interp_diagnostics.cpp",
    "runtime/psx/interp_diagnostics.h",
    "runtime/psx/selftest.cpp",
    "runtime/psx/sbs.cpp",
    "runtime/psx/sbs.h",
    "runtime/psx/dualcore.cpp",
    "runtime/psx/dualcore.h",
    "runtime/psx/verify_harness.cpp",
    "runtime/psx/verify_harness.h",
    "runtime/psx/recomp_iface.cpp",
    "runtime/psx/recomp_iface.h",
    "runtime/psx/override_registry.cpp",
    "runtime/psx/override_registry.h",
    "runtime/psx/overlay_router.cpp",
    "runtime/psx/overlay_router.h",
    "runtime/psx/generic_whole_program.cpp",
    "runtime/psx/generic_whole_program.h",
)

FORBIDDEN_PRODUCT_SYMBOLS = (
    re.compile(r"(^|\W)gen_func_[0-9a-fA-F]+($|\W)"),
    re.compile(r"(^|\W)main_dispatch($|\W)"),
    re.compile(r"(^|\W)rec_interp($|\W)"),
)


def cmake_product_sources(path: Path) -> list[str]:
    if not path.is_file():
        raise ValueError(f"CMake product manifest does not exist: {path}")
    result: list[str] = []
    for raw_line in path.read_text(encoding="utf-8").splitlines():
        line = raw_line.split("#", 1)[0].strip().lower()
        if not line:
            continue
        result.extend(part.replace("\\", "/") for part in line.split())
    return result


def forbidden_sources(path: Path) -> list[str]:
    return sorted(
        {
            source
            for source in cmake_product_sources(path)
            if any(part in f"/{source.rstrip(')')}" for part in FORBIDDEN_SOURCE_PARTS)
        }
    )


def forbidden_repository_paths(root: Path) -> list[str]:
    if not root.is_dir():
        raise ValueError(f"repository root does not exist: {root}")
    findings = [relative for relative in FORBIDDEN_REPOSITORY_PATHS if (root / relative).exists()]
    for directory in (root / "tools", root / "scripts"):
        if directory.is_dir():
            findings.extend(path.relative_to(root).as_posix() for path in directory.rglob("*.sh"))
    return sorted(set(findings))


def defined_symbols(binary: Path, nm: str) -> str:
    if not binary.is_file():
        raise ValueError(f"product binary does not exist: {binary}")
    result = subprocess.run(
        [nm, "-C", "--defined-only", str(binary)],
        check=False,
        text=True,
        capture_output=True,
    )
    if result.returncode:
        raise ValueError(f"{nm} could not inspect {binary}: {result.stderr.strip()}")
    return result.stdout


def forbidden_symbols(symbol_table: str) -> list[str]:
    return sorted(
        line
        for line in symbol_table.splitlines()
        if any(pattern.search(line) for pattern in FORBIDDEN_PRODUCT_SYMBOLS)
    )


def selftest() -> int:
    root = Path(__file__).resolve().parent.parent / "build/tool-selftests/execution-boundary"
    shutil.rmtree(root, ignore_errors=True)
    try:
        root.mkdir(parents=True)
        clean = root / "clean.cmake"
        clean.write_text("set(SOURCES runtime/cpu/lightrec_executor.cpp)\n", encoding="utf-8")
        dirty = root / "dirty.cmake"
        dirty.write_text(
            "set(SOURCES\n runtime/cpu/lightrec_executor.cpp\n generated/shard_0.c\n "
            "runtime/psx/interp.cpp)\n",
            encoding="utf-8",
        )
        assert forbidden_sources(clean) == []
        findings = forbidden_sources(dirty)
        assert len(findings) == 2
        repository = root / "repo"
        deleted_tool_dir = "tools/" + "recomp"
        (repository / deleted_tool_dir).mkdir(parents=True)
        (repository / "tools/abi_extract.py").write_text("# forbidden\n", encoding="utf-8")
        (repository / "scripts").mkdir()
        (repository / "scripts/build.sh").write_text("#!/bin/sh\n", encoding="utf-8")
        repository_findings = forbidden_repository_paths(repository)
        assert repository_findings == ["scripts/build.sh", "tools/abi_extract.py", deleted_tool_dir]
        assert forbidden_symbols("000 T normal_symbol\n") == []
        assert len(forbidden_symbols("000 T gen_func_80010000\n000 T rec_interp\n")) == 2
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print("execution boundary checker: PASS (clean + repository-negative + source-negative + symbol-negative)")
    return 0


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cmake", type=Path, help="CMake product source manifest to inspect")
    parser.add_argument("--binary", type=Path, help="linked product or library to inspect")
    parser.add_argument("--root", type=Path, help="repository tree to inspect for retired surfaces")
    parser.add_argument("--nm", default="nm", help="nm-compatible executable")
    parser.add_argument("--selftest", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.selftest:
        return selftest()
    if not args.cmake and not args.binary and not args.root:
        print("execution boundary checker: REFUSED: pass --root, --cmake, and/or --binary", file=sys.stderr)
        return 2
    try:
        repository_findings = forbidden_repository_paths(args.root) if args.root else []
        source_findings = forbidden_sources(args.cmake) if args.cmake else []
        symbol_findings = forbidden_symbols(defined_symbols(args.binary, args.nm)) if args.binary else []
    except ValueError as error:
        print(f"execution boundary checker: REFUSED: {error}", file=sys.stderr)
        return 2
    if repository_findings or source_findings or symbol_findings:
        print(
            "execution boundary checker: FAIL: "
            f"{len(repository_findings)} forbidden repository surface(s), "
            f"{len(source_findings)} forbidden product source(s), "
            f"{len(symbol_findings)} forbidden linked symbol(s)",
            file=sys.stderr,
        )
        for finding in repository_findings + source_findings + symbol_findings:
            print(f"  {finding}", file=sys.stderr)
        return 1
    print("execution boundary checker: PASS: product owns no offline guest source or explicit interpreter-mode symbols")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

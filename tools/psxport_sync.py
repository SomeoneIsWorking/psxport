#!/usr/bin/env python3
"""Resolve a consumer's psxport checkout and verify its recorded build provenance.

This is the canonical resolver used by psxport consumers after their minimal fresh-clone bootstrap.
It never replaces an existing private checkout, never records a dirty framework as a pin, and checks
the CMake-written ``build/psxport_resolved.txt`` against the consumer's immutable ``psxport.pin``.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import tempfile
from pathlib import Path


class Refusal(RuntimeError):
    pass


def run(command: list[str], *, cwd: Path) -> str:
    completed = subprocess.run(command, cwd=cwd, capture_output=True, text=True, check=False)
    if completed.returncode:
        raise Refusal(completed.stderr.strip() or "command failed: " + " ".join(command))
    return completed.stdout.strip()


def pin(consumer: Path) -> tuple[str, str]:
    values: dict[str, str] = {}
    for line in (consumer / "psxport.pin").read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("=")
        if separator:
            values[key.strip()] = value.strip()
    if not values.get("url") or not values.get("commit"):
        raise Refusal(f"{consumer / 'psxport.pin'} must provide url and immutable commit")
    return values["url"], values["commit"]


def valid(path: Path) -> bool:
    return (path / "cmake" / "psxport.cmake").is_file()


def framework(consumer: Path) -> Path:
    return consumer / "external" / "psxport"


def candidates(consumer: Path) -> list[Path]:
    roots = []
    if value := os.environ.get("PSX"):
        roots.append(Path(value) / "psxport")
    roots.append(consumer.parent / "psxport")
    return roots


def auto(consumer: Path) -> None:
    link = framework(consumer)
    if valid(link):
        print(f"[psxport] ready: {link.resolve()}")
        return
    if link.exists() or link.is_symlink():
        raise Refusal(f"{link} exists but is not a psxport checkout; refusing to replace it")
    for shared in candidates(consumer):
        if valid(shared):
            link.parent.mkdir(parents=True, exist_ok=True)
            link.symlink_to(os.path.relpath(shared, link.parent))
            print(f"[psxport] linked shared checkout: {link} -> {shared}")
            return
    url, commit = pin(consumer)
    link.parent.mkdir(parents=True, exist_ok=True)
    run(["git", "clone", url, str(link)], cwd=consumer)
    run(["git", "checkout", commit], cwd=link)
    run(["git", "submodule", "update", "--init", "vendor/beetle-psx", "vendor/lucent"], cwd=link)
    run(["git", "submodule", "update", "--init", "deps/libchdr"], cwd=link / "vendor" / "beetle-psx")
    print(f"[psxport] cloned pinned checkout: {link}")


def resolved_file(consumer: Path, supplied: Path | None) -> Path:
    return supplied if supplied is not None else consumer / "build" / "psxport_resolved.txt"


def read_resolved(path: Path) -> str | None:
    if not path.is_file():
        return None
    for line in path.read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("=")
        if separator and key.strip() == "commit":
            return value.strip()
    raise Refusal(f"{path} does not contain a commit provenance field")


def check(consumer: Path, supplied: Path | None) -> None:
    _, expected = pin(consumer)
    path = resolved_file(consumer, supplied)
    actual = read_resolved(path)
    if actual is None:
        print(f"[psxport] no CMake provenance at {path}; nothing built yet")
        return
    if actual != expected:
        raise Refusal(f"build used {actual}, but {consumer / 'psxport.pin'} records {expected}")
    print(f"[psxport] provenance OK: build and pin both use {actual}")


def selftest() -> None:
    """Exercise the shipping resolver against a self-contained shared checkout."""
    scratch = Path(__file__).resolve().parents[1] / "scratch"
    scratch.mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="psxport_sync_", dir=scratch) as temporary:
        root = Path(temporary)
        shared = root / "psxport"
        (shared / "cmake").mkdir(parents=True)
        (shared / "cmake" / "psxport.cmake").write_text("# fixture\n", encoding="utf-8")
        consumer = root / "consumer"
        consumer.mkdir()
        expected = "a" * 40
        (consumer / "psxport.pin").write_text(
            "url = https://example.invalid/psxport.git\ncommit = " + expected + "\n",
            encoding="utf-8",
        )

        old_psx = os.environ.pop("PSX", None)
        try:
            auto(consumer)
        finally:
            if old_psx is not None:
                os.environ["PSX"] = old_psx
        link = framework(consumer)
        assert link.is_symlink() and link.resolve() == shared.resolve()

        provenance = root / "provenance.txt"
        provenance.write_text("commit = " + expected + "\n", encoding="utf-8")
        check(consumer, provenance)
        provenance.write_text("commit = " + "b" * 40 + "\n", encoding="utf-8")
        try:
            check(consumer, provenance)
        except Refusal as error:
            assert "build used" in str(error)
        else:
            raise AssertionError("mismatched provenance was accepted")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--consumer", type=Path, help="consumer repository root")
    group = parser.add_mutually_exclusive_group(required=False)
    group.add_argument("--auto", action="store_true", help="resolve the framework without replacing an existing checkout")
    group.add_argument("--check", action="store_true", help="compare CMake provenance to psxport.pin")
    parser.add_argument("--resolved", type=Path, help="override CMake provenance path for --check")
    parser.add_argument("--selftest", action="store_true", help="exercise resolver/provenance behavior hermetically")
    args = parser.parse_args(argv)
    if args.selftest:
        selftest()
        print("psxport_sync selftest: PASS — shared resolution and provenance mismatch refusal")
        return 0
    if not (args.auto or args.check):
        parser.error("one of --auto or --check is required")
    if args.consumer is None:
        parser.error("--consumer is required unless --selftest is used")
    consumer = args.consumer.resolve()
    if not (consumer / "psxport.pin").is_file():
        raise Refusal(f"{consumer} is not a psxport consumer: psxport.pin is missing")
    if args.auto:
        auto(consumer)
    else:
        check(consumer, args.resolved)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Refusal as error:
        print(f"[psxport] REFUSED: {error}", file=sys.stderr)
        raise SystemExit(2)

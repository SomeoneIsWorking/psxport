#!/usr/bin/env python3
"""Build psxport's fast-boot OpenBIOS image from the pinned source workflow."""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path


def checked(arguments: list[str | Path], *, cwd: Path) -> None:
    command = [str(argument) for argument in arguments]
    result = subprocess.run(command, cwd=cwd, check=False)
    if result.returncode:
        raise RuntimeError(f"command failed ({result.returncode}): {' '.join(command)}")


def main() -> int:
    root = Path(__file__).resolve().parent.parent
    source = root / "vendor/openbios-src"
    try:
        for program in ("git", "make", "mips64-linux-gnu-gcc"):
            if shutil.which(program) is None:
                raise RuntimeError(f"required program not found: {program}")
        if not source.is_dir():
            checked(
                [
                    "git",
                    "clone",
                    "--depth",
                    "1",
                    "--filter=blob:none",
                    "--sparse",
                    "https://github.com/grumpycoders/pcsx-redux.git",
                    source,
                ],
                cwd=root,
            )
            checked(["git", "sparse-checkout", "set", "src/mips", "third_party/EASTL/include"], cwd=source)
            checked(["git", "submodule", "update", "--init", "--depth", "1", "third_party/uC-sdk"], cwd=source)
            checked(
                ["git", "apply", root / "patches/openbios/0001-psxport-boot-logs.patch"],
                cwd=source,
            )
        checked(
            [
                "make",
                "-C",
                source / "src/mips/openbios",
                "FASTBOOT=1",
                "PREFIX=mips64-linux-gnu",
                "FORMAT=elf32-tradlittlemips",
                f"-j{max(1, os.cpu_count() or 1)}",
            ],
            cwd=root,
        )
        destination = root / "bios/openbios-fast.bin"
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source / "src/mips/openbios/openbios.bin", destination)
    except (OSError, RuntimeError) as error:
        print(f"build_openbios: REFUSED: {error}", file=sys.stderr)
        return 1
    print(f"built: {destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

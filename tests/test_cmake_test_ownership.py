"""Exercise psxport's standalone-versus-embedded CTest ownership in real CMake configures."""

from __future__ import annotations

import re
import shutil
import subprocess
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRATCH = ROOT / "scratch" / "cmake-test-ownership"


class Checks:
    def __init__(self) -> None:
        self.count = 0

    def equal(self, actual: object, expected: object) -> None:
        self.count += 1
        if actual != expected:
            raise AssertionError(f"check {self.count}: {actual!r} != {expected!r}")

    def true(self, value: object, detail: str) -> None:
        self.count += 1
        if not value:
            raise AssertionError(f"check {self.count}: {detail}")


def run(*args: str, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
    return subprocess.run(args, cwd=cwd, check=True, text=True, capture_output=True)


def test_names(build: Path) -> list[str]:
    listing = run("ctest", "--test-dir", str(build), "-N")
    return re.findall(r"Test\s+#\d+:\s+(\S+)", listing.stdout)


def write_fixture(source: Path) -> None:
    source.mkdir(parents=True)
    (source / "probe.cpp").write_text("int main() { return 0; }\n", encoding="utf-8")
    (source / "CMakeLists.txt").write_text(
        f'''cmake_minimum_required(VERSION 3.24)
project(psxport_consumer_fixture LANGUAGES C CXX)
enable_testing()
set(PSXPORT_BUILD_SMOKE ON CACHE BOOL "" FORCE)
add_subdirectory("{ROOT}" psxport_build)
add_executable(consumer_probe probe.cpp)
add_test(NAME consumer_probe COMMAND consumer_probe)
''',
        encoding="utf-8",
    )


def configure(source: Path, build: Path, framework_tests: bool | None = None) -> None:
    args = [
        "cmake",
        "-S",
        str(source),
        "-B",
        str(build),
        "-DCMAKE_C_COMPILER=clang",
        "-DCMAKE_CXX_COMPILER=clang++",
    ]
    if framework_tests is not None:
        args.append(f"-DPSXPORT_BUILD_TESTS={'ON' if framework_tests else 'OFF'}")
    run(*args)


def main() -> int:
    checks = Checks()
    SCRATCH.mkdir(parents=True, exist_ok=True)
    work = Path(tempfile.mkdtemp(prefix="run-", dir=SCRATCH))
    try:
        source = work / "source"
        write_fixture(source)

        default_build = work / "default"
        configure(source, default_build)
        names = test_names(default_build)
        checks.equal(names, ["consumer_probe"])
        cache = (default_build / "CMakeCache.txt").read_text(encoding="utf-8")
        checks.true("PSXPORT_BUILD_TESTS:BOOL=OFF" in cache, "embedded default must be OFF")
        run("cmake", "--build", str(default_build), "--target", "consumer_probe", "-j2")
        run("ctest", "--test-dir", str(default_build), "-R", "^consumer_probe$", "--output-on-failure")

        opted_in_build = work / "opted-in"
        configure(source, opted_in_build, framework_tests=True)
        opted_in_names = set(test_names(opted_in_build))
        for expected in ("consumer_probe", "oracle_spike", "test_fmv_watchdog", "cpp_style", "psx_exe_reader"):
            checks.true(expected in opted_in_names, f"explicit opt-in did not register {expected}")
        checks.true(len(opted_in_names) >= 68, "explicit opt-in registered too few framework tests")
    finally:
        shutil.rmtree(work)

    print(f"cmake test ownership: PASS ({checks.count} checks; embedded default + explicit opt-in)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

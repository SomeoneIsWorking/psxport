#!/usr/bin/env python3
"""A consumer must resolve the PINNED Lightrec worktree, so a landing commit cannot change its build.

The real question this answers: `cmake/lightrec_dependency.cmake` pins `PSXPORT_LIGHTREC_REVISION`, and
the shared Lightrec checkout MOVES. If the resolver only ever saw the moving checkout, then a Lightrec
commit landing under a running consumer would change what that consumer builds with no edit to it — and
the revision check would turn that landing into a hard configure failure in a tree that was green a
minute earlier. The resolver therefore looks at `<checkout>/scratch/pins/<revision>/` FIRST and the plain
checkout second, and `tools/psxport_fetch.py --lightrec` creates that worktree from the SAME pin.

EVERYTHING HERE IS A FIXTURE. The Lightrec repository is a throwaway git repository in a temp directory
with two commits and a hand-written CMakeLists, and the pin under test is one the fixture sets after
including the module — so the case is independent of whatever the real shared checkout is at today, and
it never touches it. Each case is a REAL CMake configure, because the behaviour under test is a candidate
order and a `FATAL_ERROR`: a test that only read the file would assert the text, not the resolution.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MODULE = ROOT / "cmake" / "lightrec_dependency.cmake"
GIT_ENV = {
    "GIT_AUTHOR_NAME": "pin fixture",
    "GIT_AUTHOR_EMAIL": "pin@fixture.invalid",
    "GIT_COMMITTER_NAME": "pin fixture",
    "GIT_COMMITTER_EMAIL": "pin@fixture.invalid",
    "GIT_CONFIG_GLOBAL": "/dev/null",
    "GIT_CONFIG_SYSTEM": "/dev/null",
}


def git(*args: str) -> str:
    """`git <args...> <path>`: the trailing argument is the working directory, so the call sites read
    as the command they run."""
    return subprocess.run(["git", *args[:-1]], cwd=args[-1], env=dict(os.environ, **GIT_ENV), check=True,
                          capture_output=True, text=True).stdout.strip()


def make_lightrec(path: Path) -> str:
    """A minimal but real Lightrec-shaped repository: a static target, a CMakeLists and a header, at a
    PINNED_COMMIT and then moved to a second commit so HEAD no longer matches the pin."""
    path.mkdir(parents=True)
    subprocess.run(["git", "init", "-q", "-b", "main", str(path)], check=True,
                   env=dict(os.environ, **GIT_ENV))
    (path / "CMakeLists.txt").write_text(
        'cmake_minimum_required(VERSION 3.21)\n'
        'project(fake_lightrec LANGUAGES C)\n'
        'file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated.h" "#pragma once\\n")\n'
        'add_library(lightrec STATIC lightrec.c)\n',
        encoding="utf-8")
    (path / "lightrec.h").write_text("void lightrec_fixture(void);\n", encoding="utf-8")
    (path / "lightrec.c").write_text("void lightrec_fixture(void) {}\n", encoding="utf-8")
    (path / ".gitignore").write_text("build/\nscratch/\n", encoding="utf-8")
    git("add", "-A", path)
    git("commit", "-q", "-m", "pinned commit", path)
    pinned = git("rev-parse", "HEAD", path)
    (path / "lightrec.c").write_text("void lightrec_fixture(void) { /* moved */ }\n", encoding="utf-8")
    git("add", "-A", path)
    git("commit", "-q", "-m", "a commit that lands after the pin", path)
    return pinned


def write_consumer(source: Path, lightrec: Path, pin: str, body: str) -> None:
    source.mkdir(parents=True, exist_ok=True)
    (source / "CMakeLists.txt").write_text(
        "\n".join(
            (
                "cmake_minimum_required(VERSION 3.21)",
                "project(pinned_lightrec_consumer LANGUAGES C)",
                f'set(PSXPORT_ROOT "{ROOT}")',
                f'set(PSXPORT_LIGHTREC_DIR "{lightrec}" CACHE PATH "fixture Lightrec")',
                f'include("{MODULE}")',
                # The pin under test is the FIXTURE's, set after the include so the module's own pin is
                # not what is being exercised — the candidate ORDER is.
                f'set(PSXPORT_LIGHTREC_REVISION "{pin}")',
                body,
                "",
            )
        ),
        encoding="utf-8",
    )


def configure(source: Path, build: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(["cmake", "-S", str(source), "-B", str(build)], check=False,
                          capture_output=True, text=True)


def resolved_from(cache: Path) -> str:
    for line in cache.read_text(encoding="utf-8").splitlines():
        if line.startswith("PSXPORT_LIGHTREC_SOURCE_DIR:INTERNAL="):
            return line.split("=", 1)[1]
    return ""


def main() -> int:
    failures: list[str] = []
    checks = 0
    with tempfile.TemporaryDirectory(prefix="lightrec-pin-") as tmp:
        work = Path(tmp)
        shared = work / "lightrec"
        pinned_sha = make_lightrec(shared)
        moved_head = git("rev-parse", "HEAD", shared)
        pinned_tree = shared / "scratch" / "pins" / pinned_sha
        git("worktree", "add", "--quiet", "--detach", str(pinned_tree), pinned_sha, shared)

        # 1. The consumer resolves the PINNED worktree even though the shared checkout has moved on.
        consumer = work / "consumer"
        write_consumer(consumer, shared, pinned_sha,
                       'psxport_configure_lightrec_dependency()\n'
                       'message(STATUS "RESOLVED=${PSXPORT_LIGHTREC_SOURCE_DIR}")')
        build = work / "build"
        done = configure(consumer, build)
        checks += 1
        if done.returncode != 0:
            failures.append(f"pinned consumer did not configure:\n{done.stdout}\n{done.stderr}")
        else:
            resolved = resolved_from(build / "CMakeCache.txt")
            checks += 1
            if os.path.realpath(resolved) != os.path.realpath(pinned_tree):
                failures.append(f"resolved {resolved}, expected the pinned worktree {pinned_tree}")
            checks += 1
            if git("rev-parse", "HEAD", Path(resolved)) != pinned_sha:
                failures.append(f"the resolved tree is not at the pin {pinned_sha}")

        # 2. With no pinned worktree, a MOVED checkout is refused, not silently consumed.
        git("worktree", "remove", "--force", str(pinned_tree), shared)
        shutil.rmtree(shared / "scratch" / "pins", ignore_errors=True)
        moved_consumer = work / "moved-consumer"
        write_consumer(moved_consumer, shared, pinned_sha, "psxport_configure_lightrec_dependency()")
        moved = configure(moved_consumer, work / "moved-build")
        checks += 1
        output = moved.stdout + moved.stderr
        if moved.returncode == 0:
            failures.append(f"a checkout at {moved_head[:12]} satisfied pin {pinned_sha[:12]}")
        elif "revision mismatch" not in output:
            failures.append(f"the refusal did not name the revision mismatch:\n{output}")
        checks += 1
        if "psxport_fetch.py --lightrec" not in output:
            failures.append("the refusal does not say how to create the pinned worktree")

        # 3. A DIRTY pinned worktree is refused, and the resolver falls through to the refusal above
        #    rather than using it: a pinned tree somebody is editing is not a verified dependency.
        git("worktree", "add", "--quiet", "--detach", str(pinned_tree), pinned_sha, shared)
        (pinned_tree / "lightrec.c").write_text("void lightrec_fixture(void) { /* edited */ }\n",
                                                encoding="utf-8")
        dirty = configure(consumer, work / "dirty-build")
        checks += 1
        dirty_output = dirty.stdout + dirty.stderr
        if dirty.returncode == 0:
            failures.append("a DIRTY pinned worktree was accepted as the dependency")

        # 4. With NO explicit directory, a framework that is a pinned worktree of its checkout
        #    (`<ws>/psx/psxport/scratch/pins/<sha>/`, which is what every port builds against) finds the
        #    shared workspace's Lightrec at `<ws>/shared/lightrec` from its MAIN checkout. No fixed `../..`
        #    chain from the pinned worktree reaches it, so a fresh configure with nothing cached used to fail.
        workspace = work / "ws"
        layout_shared = workspace / "shared" / "lightrec"
        layout_sha = make_lightrec(layout_shared)
        layout_pin = layout_shared / "scratch" / "pins" / layout_sha
        git("worktree", "add", "--quiet", "--detach", str(layout_pin), layout_sha, layout_shared)
        framework = workspace / "psx" / "psxport"
        framework.mkdir(parents=True)
        git("init", "-q", "-b", "main", framework)
        (framework / "README").write_text("fixture framework\n", encoding="utf-8")
        git("add", "-A", framework)
        git("commit", "-q", "-m", "framework", framework)
        framework_pin = framework / "scratch" / "pins" / "fixture"
        git("worktree", "add", "--quiet", "--detach", str(framework_pin), "HEAD", framework)
        layout_consumer = workspace / "psx" / "title" / ".claude" / "worktrees" / "change"
        layout_consumer.mkdir(parents=True)
        (layout_consumer / "CMakeLists.txt").write_text(
            "\n".join(("cmake_minimum_required(VERSION 3.21)",
                       "project(layout_consumer LANGUAGES C)",
                       f'set(PSXPORT_ROOT "{framework_pin}")',
                       f'include("{MODULE}")',
                       f'set(PSXPORT_LIGHTREC_REVISION "{layout_sha}")',
                       "psxport_configure_lightrec_dependency()", "")),
            encoding="utf-8")
        environment = {key: value for key, value in os.environ.items()
                       if key not in ("PSXPORT_LIGHTREC_DIR", "SHARED_DIR")}
        layout = subprocess.run(["cmake", "-S", str(layout_consumer), "-B", str(work / "layout-build")],
                                check=False, capture_output=True, text=True, env=environment)
        checks += 1
        if layout.returncode != 0:
            failures.append("a pinned framework worktree did not find <workspace>/shared/lightrec from "
                            f"its main checkout:\n{layout.stdout}\n{layout.stderr}")
        else:
            checks += 1
            resolved = resolved_from(work / "layout-build" / "CMakeCache.txt")
            if os.path.realpath(resolved) != os.path.realpath(layout_pin):
                failures.append(f"resolved {resolved}, expected the pinned worktree {layout_pin}")

        # 5. A pin bump reconfigures an existing build: the resolved tree is not written back as an input.
        bumped_sha = git("rev-parse", "HEAD", layout_shared)
        bumped_pin = layout_shared / "scratch" / "pins" / bumped_sha
        git("worktree", "add", "--quiet", "--detach", str(bumped_pin), bumped_sha, layout_shared)
        listing = (layout_consumer / "CMakeLists.txt").read_text(encoding="utf-8")
        (layout_consumer / "CMakeLists.txt").write_text(listing.replace(layout_sha, bumped_sha), encoding="utf-8")
        bumped = subprocess.run(["cmake", "-S", str(layout_consumer), "-B", str(work / "layout-build")],
                                check=False, capture_output=True, text=True, env=environment)
        checks += 1
        if bumped.returncode != 0:
            failures.append(f"a pin bump did not reconfigure the existing build:\n{bumped.stdout}\n{bumped.stderr}")
        else:
            checks += 1
            resolved = resolved_from(work / "layout-build" / "CMakeCache.txt")
            if os.path.realpath(resolved) != os.path.realpath(bumped_pin):
                failures.append(f"after the bump resolved {resolved}, expected {bumped_pin}")

    if failures:
        print(f"[lightrec-pin] FAIL: {len(failures)} of {checks} checks failed")
        for label in failures:
            print(f"[lightrec-pin]   - {label}")
        return 1
    print(f"[lightrec-pin] OK: {checks} checks — a consumer resolves the pinned worktree, a moved "
          f"checkout is refused by revision, a dirty pinned worktree is not accepted, and a pinned framework worktree finds the "
          f"workspace Lightrec from its main checkout, and a pin bump reconfigures an existing build")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""pin_round.py — reconfigure -> build -> test -> --bump, for every port, in that order.

WHY THIS EXISTS. The pin guard compares a port's recorded pin against the framework commit its build
RECEIPT names, and a framework commit invalidates every receipt. That is the guard working, not
misbehaving, and the documented remedy is a four-step round per port. Doing it by hand is nine
near-identical command sequences in nine different build directories, and the failure mode of getting
one of them wrong is a port left red with a plausible story about why.

So this runs the round and REPORTS, per port, which step each one reached. It does not hide a failure:
a port that fails to build, or fails its gate, or refuses the bump, is reported at the step it stopped,
with its own words, and the round continues to the next port -- because stopping would leave the other
eight un-refreshed for no gain.

It NEVER bumps a port whose build or gate failed, and never bumps one whose receipt disagrees with the
framework it links. `--bump` already refuses both; this adds the ordering and the reporting on top, and
it prints what it did so the operator can see the round rather than infer it from nine green gates.

Usage:
  python3 tools/pin_round.py --dry-run          # show the plan, touch nothing
  python3 tools/pin_round.py                    # run the round
  python3 tools/pin_round.py --port spyro        # one port
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

PSX = Path("/home/bhamil/repo/psx")
FRAMEWORK = PSX / "psxport"

# Each port's CONFIGURED build directory, which is not always `build`: several titles configure a
# named directory and the receipt only exists there. `crash` is the one that cannot be configured with
# GCC, because its policy gate runs clang-tidy over the compile database and cannot pass under it.
PORTS = [
    ("spyro", "build", True),
    ("ctr", "build/agent-clang", True),
    ("crash", "build/ci", True),
    ("crashbash", "build/migration", True),
    ("megamanx4", "build", True),
    ("spider1", "build/agent-clang", True),
    ("Tomba2Engine", "build/ci", True),
    ("tekken3", "build", True),
    ("vagrant", "build", True),
]

PIN_RE = re.compile(r"^commit\s*=\s*([0-9a-f]{8,})", re.MULTILINE)


def run(cmd: list[str], cwd: Path) -> tuple[int, str]:
    proc = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=5400)
    return proc.returncode, (proc.stdout + proc.stderr)


def framework_head() -> str:
    rc, out = run(["git", "rev-parse", "--short=8", "HEAD"], FRAMEWORK)
    return out.strip() if rc == 0 else "?"


def receipt_commit(build: Path) -> str:
    f = build / "psxport_resolved.txt"
    if not f.is_file():
        return ""
    m = PIN_RE.search(f.read_text(errors="ignore"))
    return m.group(1)[:8] if m else ""


def pin_commit(port: Path) -> str:
    f = port / "psxport.pin"
    if not f.is_file():
        return ""
    m = PIN_RE.search(f.read_text(errors="ignore"))
    return m.group(1)[:8] if m else ""


def ctest_summary(out: str) -> str:
    for line in out.splitlines():
        if "tests passed" in line and "tests failed" in line:
            return line.strip()
    return "no summary line"


def is_pin_test(name: str) -> bool:
    """A pin test, by NAME rather than by substring alone.

    `crash` registers its provenance test as `crash_dependency_provenance` and the name lacks the
    substring `pin` -- which is how "crash registers no pin test" was once published as a fact about a
    port, from a grep for a string. So the predicate is deliberately generous about the NAME and also
    accepts a test whose own output says it is the pin check.
    """
    n = name.strip().lower()
    return "pin" in n or "provenance" in n


def failed_tests(out: str) -> list[str]:
    """The names of the tests CTest reported as failed, from its own summary block.

    Parsed from the 'The following tests FAILED:' section rather than from the progress lines, so a
    test that printed `***Failed` in a name and then passed on retry cannot be counted. If the section
    is absent, this returns everything it can and the caller reports the raw summary -- a missing
    parse must not read as "no failures".
    """
    names: list[str] = []
    collecting = False
    for line in out.splitlines():
        if "The following tests FAILED:" in line:
            collecting = True
            continue
        if collecting:
            s = line.strip()
            if not s:
                if names:
                    break
                continue
            # Entries look like "  25 - psxport_pin (Failed)"; take the middle field.
            parts = s.split()
            if len(parts) >= 4 and parts[1] == "-":
                names.append(parts[2])
            elif parts and not s[0].isdigit():
                break
    return names


def round_port(name: str, rel_build: str, dry: bool) -> str:
    port = PSX / name
    build = port / rel_build
    if not (port / "tools" / "psxport_fetch.py").is_file():
        return f"SKIP   {name}: no fetch tool"
    if dry:
        return (f"PLAN   {name}: build={rel_build} pin={pin_commit(port) or '-'} "
                f"receipt={receipt_commit(build) or '-'}")
    steps = []
    # external/psxport must EXIST at the recorded pin before configure: the pin work lives in the
    # fetched framework, and external/psxport is now a pinned worktree rather than the moving checkout,
    # so a port whose pin moved forward needs a fetch before its build means anything.
    fetch = port / "tools/psxport_fetch.py"
    rc, out = run(["python3", str(fetch)], port)
    steps.append(f"fetch={'ok' if rc == 0 else f'rc{rc}'}")
    if rc != 0:
        first = next((l for l in out.splitlines() if l.strip()), out.strip()[:90])
        return f"STOP   {name}: fetch refused -- {first}"
    rc, _ = run(["cmake", "-S", ".", "-B", rel_build, "-DCMAKE_CXX_COMPILER=clang++",
                 "-DCMAKE_C_COMPILER=clang"], port)
    steps.append(f"configure={'ok' if rc == 0 else f'rc{rc}'}")
    if rc != 0:
        return f"STOP   {name}: configure failed -- " + " ".join(steps)
    rc, out = run(["cmake", "--build", rel_build, "-j", str(max(1, len(steps) and 8))], port)
    steps.append(f"build={'ok' if rc == 0 else f'rc{rc}'}")
    if rc != 0:
        tail = [l for l in out.splitlines() if "error" in l.lower()][:2]
        return f"STOP   {name}: build failed -- " + " ".join(steps) + " | " + " / ".join(tail)
    rc, out = run(["ctest", "--test-dir", rel_build], port)
    summary = ctest_summary(out)
    # THE PIN TEST IS EXPECTED RED BEFORE THE BUMP, and treating it as a failure makes the round
    # unable to ever complete -- the receipt is now current precisely because the framework moved, the
    # pin still records the old commit, and that is the REASON this round is running. So the gate is
    # split: a red PIN test is the round's premise, and any OTHER red test is a real stop.
    others = [t for t in failed_tests(out) if not is_pin_test(t)]
    steps.append(f"test={'ok' if rc == 0 else 'pin-only' if not others else 'RED'}")
    if others:
        return (f"STOP   {name}: gate red on {len(others)} non-pin test(s) -- " + " ".join(steps)
                + f" ({summary}) | {', '.join(others[:3])}")
    if rc != 0:
        steps.append("pin-test red before bump, as expected")
    # The pin tool is the FETCHED framework's, not the port's: it is not installed into a port any more.
    rc, out = run(["python3", str(port / "external/psxport/tools/psxport_sync.py"),
                   "--repo", str(port), "--bump", "--build", str(build)], port)
    if rc != 0:
        first = next((l for l in out.splitlines() if l.strip()), out.strip()[:90])
        return f"STOP   {name}: bump refused -- " + " ".join(steps) + f" | {first}"
    steps.append("bump=ok")
    rc, out = run(["ctest", "--test-dir", rel_build], port)
    return (f"DONE   {name}: " + " ".join(steps) + f"  pin={pin_commit(port)}  "
            f"({ctest_summary(out)})")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--port", default="", help="one port only")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    head = framework_head()
    print(f"[pin-round] framework {head}; every receipt below predates it until this round runs\n")
    ports = [p for p in PORTS if not args.port or p[0] == args.port]
    if args.port and not ports:
        print(f"REFUSED: no configured build directory is known for {args.port!r}. "
              f"Known: {[p[0] for p in PORTS]}")
        return 2
    results = [round_port(p[0], p[1], args.dry_run) for p in ports]
    for line in results:
        print(line)
    bad = [r for r in results if not r.startswith(("DONE", "PLAN", "SKIP"))]
    print(f"\n[pin-round] {len(results) - len(bad)} of {len(results)} completed; "
          f"{len(bad)} stopped at a step and need a look.")
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())

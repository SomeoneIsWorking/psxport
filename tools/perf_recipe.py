"""Run one deterministic cost recipe on a built title product and keep its log.

The run is the meter setup of docs/presentation.md "Cost and cadence": hidden, silent, unpaced, the judgement
ires (0) and sink (3840x2160), fps60 on, with the `perf` channel switched on only for the measured window, so the
run-end `[perf] cpu ...` lines cover just those logic frames. The steps are REPL lines separated by `;`
(`run N`, `warp a/b`, `debug perf`, any title verb). `mark` is the recipe's own step: it samples the product's
process CPU time there and again at the end, and prints the window's CPU seconds and fields, the meter for a title
whose frame driver marks no perf phases (each sample is `ps` time, 10 ms resolution). E.g. Tomba 2's

    uv run --frozen python tools/perf_recipe.py --root ../psx-tomba \\
        --product build/agent/bin/tomba2_port \\
        --steps "run 600; warp 0/0; run 300; debug perf; run 1200" --log scratch/perf/tomba2-a1.log

Compare two sets of logs with tools/perf_compare.py.
"""
from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

from port.launch_environment import agent_environment  # noqa: E402

READY = re.compile(r"\[repl\] frame=(\d+) ready")


def process_cpu_seconds(pid: int) -> float:
    """User plus system CPU time of a live process, from ps (10 ms resolution)."""
    text = subprocess.run(["ps", "-o", "time=", "-p", str(pid)], capture_output=True, text=True,
                          check=True).stdout.strip()
    seconds = 0.0
    for part in text.replace("-", ":").split(":"):
        seconds = seconds * 60 + float(part)
    return seconds


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--root", type=Path, required=True, help="the title repository the product runs from")
    parser.add_argument("--product", type=Path, required=True, help="the product binary, relative to --root")
    parser.add_argument("--image", type=Path, help="the boot executable, relative to --root (default: the product finds it)")
    parser.add_argument("--steps", required=True, help="REPL lines separated by ';'")
    parser.add_argument("--log", type=Path, required=True)
    parser.add_argument("--debug", default="", help="debug channels on from the start (perf is switched on by a step)")
    parser.add_argument("--env", action="append", default=[], help="extra KEY=VALUE for the run")
    args = parser.parse_args()

    root = args.root.resolve()
    env = agent_environment(dict(os.environ))
    env.update(PSXPORT_REPL="1", PSXPORT_WATCHDOG="0", PSXPORT_NOPACE="1", PSXPORT_IRES="0", PSXPORT_FPS60="1",
               PSXPORT_PRESENT_SINK="3840x2160")
    if args.debug:
        env["PSXPORT_DEBUG"] = args.debug
    for pair in args.env:
        key, _, value = pair.partition("=")
        env[key] = value
    log = args.log.resolve()
    log.parent.mkdir(parents=True, exist_ok=True)
    with log.open("w") as out:
        command = [str(root / args.product)] + ([str(root / args.image)] if args.image else [])
        proc = subprocess.Popen(command, cwd=root, env=env,
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
                                bufsize=1)
        assert proc.stdin is not None and proc.stdout is not None

        def await_ready() -> None:
            for line in proc.stdout:
                out.write(line)
                if READY.search(line):
                    return
            raise SystemExit(f"the product exited before its next frame boundary; see {log}")

        await_ready()
        marked: tuple[float, int] | None = None
        fields = 0
        for step in filter(None, (s.strip() for s in args.steps.split(";"))):
            if step == "mark":
                marked = (process_cpu_seconds(proc.pid), fields)
                continue
            proc.stdin.write(step + "\n")
            proc.stdin.flush()
            if step.split()[0] == "run":
                await_ready()
                fields += int(step.split()[1]) if len(step.split()) > 1 else 1
        window = None
        if marked is not None:
            window = (process_cpu_seconds(proc.pid) - marked[0], fields - marked[1])
            out.write(f"[recipe] window cpu: {window[0]:.2f} s over {window[1]} fields "
                      f"({1000 * window[0] / max(1, window[1]):.3f} cpu-ms/field), process CPU time\n")
        proc.stdin.write("end\n")
        proc.stdin.close()
        for line in proc.stdout:
            out.write(line)
        code = proc.wait()
    for line in log.read_text().splitlines():
        if "[perf] cpu" in line or "shutdown: guest:" in line or "[recipe]" in line:
            print(line)
    return code


if __name__ == "__main__":
    raise SystemExit(main())

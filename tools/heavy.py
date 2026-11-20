"""Run a heavy step (title build, product run) on whichever workspace machine has room for it.

    uv run --frozen python external/psxport/tools/heavy.py build --target spyro_port
    uv run --frozen python external/psxport/tools/heavy.py run --target spyro_port --fetch scratch/x -- \
        build/agent/bin/spyro_port scratch/assets/spyro1/SCUS_942.28

The machines are this one and the Ally (docs/workspace/WORKSPACE.md, Second machine). Each build and
run starts through that machine's shared `heavy.py run` (re-harness tools/heavy.py, ~/.local/bin), which
waits until `--cores` and `--memory` are spare; a build compiles with `--cores` jobs. Without
`--host`, the step goes where those cores are spare now, this machine first. On the Ally the caller's
working tree (uncommitted edits included) is mirrored to ~/agents/mirror first, under that mirror's
lock for the whole step; builds run in the my-distrobox box and runs on the host. The Mac
cannot be reached from here: a step for it is queued in ~/agents/queue/<job>/job.json and the Mac's
worker (which can ssh here and to the Ally) mirrors the tree, runs it, copies `fetch` back, streams
`log` and writes the exit code to `done` (97 refused, 98 worker error); it touches
~/agents/queue/mac.alive while it serves. It accepts only build/agent builds, product binaries and
checked-in tools, PSXPORT_* run keys, and runs them sandboxed (psxport tools/heavy_mac/).

A step's binary lives on the machine that built it, so a run builds with --target in the same step.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import subprocess
import sys
import time
import uuid
from pathlib import Path

ALLY = "ally"
# The shared runner on each machine; the Ally's is a copy at the same path.
ROOM_RUNNER = Path.home() / ".local/bin/heavy.py"
ALLY_ROOM_RUNNER = "$HOME/.local/bin/heavy.py"
# One step at a time writes and builds in the Ally's mirror.
ALLY_MIRROR_LOCK = "$HOME/agents/mirror/.lock"
# What a step reserves unless told otherwise: a title build, or a hidden product run.
BUILD_ROOM = (8, 8.0)
RUN_ROOM = (2, 3.0)
SPARE_CORES = re.compile(r"^([0-9.]+) of \d+ cores spare", re.MULTILINE)
ALLY_MIRROR = "agents/mirror/repo"
ALLY_BOX = "my-distrobox"
SHARED_DEPENDENCIES = ("bug-report", "setup-ui", "touch-ui", "port-assets")
MIRROR_EXCLUDES = ("/build/", "/scratch/", "/env-assets/", "/.env", "__pycache__", ".venv")
AGENT_RUN_ENV = {"PSXPORT_SHOW_WINDOW": "0", "PSXPORT_NOAUDIO": "1"}
# The Mac cannot be reached from here; its worker pulls jobs from this queue over ssh.
MAC = "mac"
MAC_QUEUE = Path.home() / "agents/queue"
MAC_HEARTBEAT = MAC_QUEUE / "mac.alive"
MAC_HEARTBEAT_LIMIT_S = 120


def repo_root(start: Path) -> Path:
    out = subprocess.run(["git", "-C", str(start), "rev-parse", "--show-toplevel"], check=True,
                         capture_output=True, text=True)
    return Path(out.stdout.strip())


def spare_cores(output: str) -> float | None:
    found = SPARE_CORES.search(output)
    return float(found.group(1)) if found else None


def local_spare() -> float | None:
    probe = subprocess.run([str(ROOM_RUNNER), "free"], capture_output=True, text=True)
    return spare_cores(probe.stdout) if probe.returncode == 0 else None


def ally_spare() -> float | None:
    probe = subprocess.run(["ssh", "-o", "ConnectTimeout=5", ALLY, f"{ALLY_ROOM_RUNNER} free"],
                           capture_output=True, text=True)
    return spare_cores(probe.stdout) if probe.returncode == 0 else None


def mac_queue() -> int | None:
    try:
        if time.time() - MAC_HEARTBEAT.stat().st_mtime > MAC_HEARTBEAT_LIMIT_S:
            return None
    except FileNotFoundError:
        return None
    return sum(1 for job in MAC_QUEUE.iterdir() if job.is_dir() and not (job / "done").exists())


UV_PYTHON = ["uv", "run", "--frozen", "python"]


def mac_accepts(args: argparse.Namespace) -> bool:
    """The Mac's worker (psxport tools/heavy_mac/) refuses anything but these shapes."""
    if args.build != "build/agent" or any(not item.startswith("PSXPORT_") for item in args.env):
        return False
    command = args.command
    if not command:
        return True
    if command[0].startswith("build/agent/bin/"):
        return True
    return command[:4] == UV_PYTHON and len(command) > 4 and command[4].startswith(
        ("tools/", "external/psxport/tools/"))


def choose_host(requested: str, args: argparse.Namespace) -> str:
    """This machine when it has the cores spare, else the Ally, else an idle Mac, else the roomiest machine."""
    if requested != "auto":
        return requested
    spare = {"local": local_spare(), ALLY: ally_spare()}
    mac = mac_queue() if mac_accepts(args) else None
    shown = " ".join(f"{name}={'off' if cores is None else cores}" for name, cores in spare.items())
    choices = [name for name in ("local", ALLY) if (spare[name] or 0.0) >= args.cores]
    if choices:
        host = choices[0]
    elif mac == 0:
        host = MAC
    else:
        host = max((name for name in spare if spare[name] is not None), key=lambda name: spare[name] or 0.0)
    print(f"[heavy] spare cores {shown} mac={'off' if mac is None else f'{mac} queued'} -> {host}", flush=True)
    return host


def room_arguments(args: argparse.Namespace, label: str, command: list[str]) -> list[str]:
    """The shared runner's arguments that start ``command`` once ``args.cores`` and ``args.memory`` are spare."""
    return ["run", "--cores", str(args.cores), "--memory", str(args.memory), "--label", label, "--",
            *command]


class AllyMirrorLock:
    """Holds the Ally mirror's lock for as long as this process keeps the ssh session's stdin open."""

    def __enter__(self) -> AllyMirrorLock:
        self._holder = subprocess.Popen(
            ["ssh", ALLY, f"mkdir -p $HOME/agents/mirror && flock {ALLY_MIRROR_LOCK} sh -c 'echo locked; cat >/dev/null'"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        if self._holder.stdout is None or self._holder.stdout.readline().strip() != "locked":
            raise SystemExit("[heavy] could not take the Ally mirror's lock")
        return self

    def __exit__(self, *exc: object) -> None:
        if self._holder.stdin is not None:
            self._holder.stdin.close()
        self._holder.wait()


def workspace_paths(repo: Path) -> tuple[Path, Path, list[Path]]:
    framework = (repo / "external/psxport").resolve() if (repo / "external/psxport").exists() else repo
    workspace = framework.parent
    shared = workspace.parent / "shared"
    return framework, workspace, [shared / name for name in SHARED_DEPENDENCIES if (shared / name).is_dir()]


def mirror(repo: Path, pushes: list[str]) -> str:
    """Copies the working trees a build reads to the Ally; returns the repo's path there."""
    framework, workspace, shared = workspace_paths(repo)
    excludes = [f"--exclude={pattern}" for pattern in MIRROR_EXCLUDES]
    trees = {framework: f"{ALLY_MIRROR}/psx/{framework.name}"}
    trees[repo] = f"{ALLY_MIRROR}/psx/{repo.name}"
    for dependency in shared:
        trees[dependency] = f"{ALLY_MIRROR}/shared/{dependency.name}"
    subprocess.run(["ssh", ALLY, "mkdir -p " + " ".join(trees.values())], check=True)
    for source, target in trees.items():
        subprocess.run(["rsync", "-a", "--delete", *excludes, f"{source}/", f"{ALLY}:{target}/"], check=True)
    remote_repo = trees[repo]
    for path in pushes:
        subprocess.run(["rsync", "-a", "--relative", f"{repo}/./{path}", f"{ALLY}:{remote_repo}/"], check=True)
    # The Ally's own clone holds its disc paths.
    subprocess.run(["ssh", ALLY, f"cp -f repo/psx/{repo.name}/.env {remote_repo}/.env 2>/dev/null || true"],
                   check=False)
    return remote_repo


def build_command(repo: Path, target: str | None, build_dir: str, jobs: int | None) -> list[str]:
    """The build; ``jobs`` sets its compile jobs (the Mac's worker takes the command without them)."""
    framework, _, _ = workspace_paths(repo)
    parallel = [] if jobs is None else ["--jobs", str(jobs)]
    if repo == framework:
        return ["uv", "run", "--frozen", "python", "tools/build.py", "--build", build_dir, *parallel]
    if target is None:
        raise SystemExit("[heavy] a title build needs --target")
    return ["uv", "run", "--frozen", "python", "external/psxport/tools/port/consumer_build.py", "--build",
            build_dir, "--target", target, *parallel]


def ally_shell(remote_repo: str, command: list[str], env: dict[str, str], box: bool,
               args: argparse.Namespace, label: str) -> int:
    assignments = " ".join(f"{key}={shlex.quote(value)}" for key, value in env.items())
    inner = f"env --chdir=$HOME/{remote_repo} {assignments} {shlex.join(command)}"
    if box:
        inner = f"distrobox enter {ALLY_BOX} -- sh -c {shlex.quote(inner)}"
    room = f"{ALLY_ROOM_RUNNER} {shlex.join(room_arguments(args, label, ['sh', '-c', inner]))}"
    return subprocess.run(["ssh", ALLY, room]).returncode


def ally_display_env() -> dict[str, str]:
    sockets = subprocess.run(["ssh", ALLY, "ls /run/user/1000"], capture_output=True, text=True).stdout.split()
    wayland = next((name for name in sockets if name.startswith("wayland-") and not name.endswith(".lock")),
                   None)
    if wayland is None:
        raise SystemExit("[heavy] the Ally has no Wayland session socket in /run/user/1000")
    return {"XDG_RUNTIME_DIR": "/run/user/1000", "WAYLAND_DISPLAY": wayland}


def mac_step(repo: Path, args: argparse.Namespace, build: list[str] | None, run_env: dict[str, str]) -> int:
    """Queues the step for the Mac's worker and streams its log until it writes `done`."""
    framework, _, shared = workspace_paths(repo)
    job = MAC_QUEUE / f"{time.strftime('%Y%m%dT%H%M%S')}-{uuid.uuid4().hex[:8]}"
    job.mkdir(parents=True)
    spec = {
        "repo": str(repo), "framework": str(framework), "shared": [str(path) for path in shared],
        "excludes": list(MIRROR_EXCLUDES), "push": args.push, "fetch": args.fetch,
        "build": build, "compile_env": {"CC": "clang", "CXX": "clang++"},
        "command": args.command if args.step == "run" else None, "run_env": run_env,
    }
    (job / "job.json.tmp").write_text(json.dumps(spec, indent=1))
    (job / "job.json.tmp").rename(job / "job.json")
    print(f"[heavy] queued for the Mac as {job.name}", flush=True)
    log = job / "log"
    shown = 0
    while not (job / "done").exists():
        if log.exists():
            text = log.read_text(errors="replace")
            sys.stdout.write(text[shown:])
            sys.stdout.flush()
            shown = len(text)
        time.sleep(2)
    if log.exists():
        sys.stdout.write(log.read_text(errors="replace")[shown:])
    return int((job / "done").read_text().strip() or 1)


def run_step(args: argparse.Namespace) -> int:
    repo = repo_root(Path(args.repo))
    building = args.step == "build" or args.target is not None
    default_cores, default_memory = BUILD_ROOM if building else RUN_ROOM
    args.cores = args.cores or default_cores
    args.memory = args.memory or default_memory
    host = choose_host(args.host, args)
    compile_env = {"CC": "clang", "CXX": "clang++"}
    jobs = None if host == MAC else args.cores
    build = build_command(repo, args.target, args.build, jobs) if building else None
    command = args.command
    if args.step == "run" and not command:
        raise SystemExit("[heavy] run needs a command after --")
    framework, _, _ = workspace_paths(repo)
    # A title run reads the overlay's fonts and menu from the framework checkout.
    title_env = {} if repo == framework else {"PSXPORT_ASSET_DIR": "external/psxport"}
    run_env = {**AGENT_RUN_ENV, **title_env, **dict(item.split("=", 1) for item in args.env)}

    if host == MAC:
        return mac_step(repo, args, build, run_env)

    label = f"{repo.name}:{args.target or args.step}"
    if host == "local":
        if not ROOM_RUNNER.is_file():
            raise SystemExit(f"[heavy] {ROOM_RUNNER} is missing: run re-harness `uv run --frozen python "
                             "tools/install_skills.py`")
        if build is not None:
            code = subprocess.run([str(ROOM_RUNNER), *room_arguments(args, label, build)], cwd=repo,
                                  env={**os.environ, **compile_env}).returncode
            if code != 0:
                return code
        if args.step == "run":
            return subprocess.run([str(ROOM_RUNNER), *room_arguments(args, label, command)], cwd=repo,
                                  env={**os.environ, **run_env}).returncode
        return 0

    with AllyMirrorLock():
        remote_repo = mirror(repo, args.push)
        if build is not None:
            code = ally_shell(remote_repo, build, compile_env, True, args, label)
            if code != 0:
                return code
        code = 0
        if args.step == "run":
            env = {**run_env, **({} if args.box else ally_display_env())}
            code = ally_shell(remote_repo, command, env, args.box, args, label)
        for path in args.fetch:
            (repo / path).mkdir(parents=True, exist_ok=True)
            subprocess.run(["rsync", "-a", f"{ALLY}:{remote_repo}/{path}/", f"{repo / path}/"], check=False)
        print(f"[heavy] ran on the Ally in ~/{remote_repo}", flush=True)
        return code


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("step", choices=("build", "run"))
    parser.add_argument("--host", choices=("auto", "local", ALLY, MAC), default="auto")
    parser.add_argument("--repo", default=".", help="the title or psxport checkout (default: cwd)")
    parser.add_argument("--target", help="product to build first (required for build in a title)")
    parser.add_argument("--build", default="build/agent", help="build tree")
    parser.add_argument("--env", action="append", default=[], help="KEY=VALUE for the run")
    parser.add_argument("--push", action="append", default=[], help="gitignored input to copy to a remote")
    parser.add_argument("--fetch", action="append", default=[], help="run output dir to copy back")
    parser.add_argument("--box", action="store_true", help="run inside the Ally's box (for uv tools)")
    parser.add_argument("--cores", type=int, help=f"cores the step keeps busy; a build's compile jobs "
                                                  f"(default: build {BUILD_ROOM[0]}, run {RUN_ROOM[0]})")
    parser.add_argument("--memory", type=float, help=f"GiB the step grows to "
                                                     f"(default: build {BUILD_ROOM[1]:g}, run {RUN_ROOM[1]:g})")
    argv = sys.argv[1:]
    split = argv.index("--") if "--" in argv else len(argv)
    args = parser.parse_args(argv[:split])
    args.command = argv[split + 1:]
    return run_step(args)


if __name__ == "__main__":
    sys.exit(main())

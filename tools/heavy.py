"""Run a heavy step (title build, product run) on whichever workspace machine has the shorter queue.

    uv run --frozen python external/psxport/tools/heavy.py build --target spyro_port
    uv run --frozen python external/psxport/tools/heavy.py run --target spyro_port --fetch scratch/x -- \
        build/agent/bin/spyro_port scratch/assets/spyro1/SCUS_942.28

The machines are this one and the Ally (docs/workspace/WORKSPACE.md, Second machine). The step takes
that machine's heavy lock. On the Ally the caller's working tree (uncommitted edits included) is
mirrored to ~/agents/mirror first, builds run in the my-distrobox box and runs on the host. The Mac
cannot be reached from here: a step for it is queued in ~/agents/queue/<job>/job.json and the Mac's
worker (which can ssh here and to the Ally) mirrors the tree, runs it, copies `fetch` back, streams
`log` and writes the exit code to `done` (97 refused, 98 worker error); it touches
~/agents/queue/mac.alive while it serves. It accepts only build/agent builds, product binaries and
checked-in tools, PSXPORT_* run keys, and runs them sandboxed (psxport tools/heavy_mac/).

A step's binary lives on the machine that built it, so a run builds with --target in the same step.
"""

from __future__ import annotations

import argparse
import fcntl
import json
import os
import shlex
import subprocess
import sys
import time
import uuid
from pathlib import Path

ALLY = "ally"
LOCAL_LOCK = Path.home() / "repo/psx/.heavy.lock"
ALLY_LOCK = "$HOME/agents/.heavy.lock"
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


def lock_waiters(process_args: str, lock_name: str) -> int:
    return sum(1 for line in process_args.splitlines() if "flock" in line and lock_name in line)


def local_queue() -> int:
    args = []
    for proc in Path("/proc").iterdir():
        if proc.name.isdigit():
            try:
                args.append((proc / "cmdline").read_bytes().replace(b"\0", b" ").decode(errors="replace"))
            except OSError:
                continue
    return lock_waiters("\n".join(args), str(LOCAL_LOCK))


def ally_queue() -> int | None:
    probe = subprocess.run(["ssh", "-o", "ConnectTimeout=5", ALLY, "ps -eo args"], capture_output=True,
                           text=True)
    if probe.returncode != 0:
        return None
    return lock_waiters(probe.stdout, ".heavy.lock")


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
    if requested != "auto":
        return requested
    queues = {"local": local_queue(), ALLY: ally_queue(), MAC: mac_queue() if mac_accepts(args) else None}
    host = min((name for name, depth in queues.items() if depth is not None), key=lambda name: queues[name])
    shown = " ".join(f"{name}={'off' if depth is None else depth}" for name, depth in queues.items())
    print(f"[heavy] queues {shown} -> {host}", flush=True)
    return host


class LocalLock:
    def __enter__(self) -> LocalLock:
        self._file = LOCAL_LOCK.open("a")
        fcntl.flock(self._file, fcntl.LOCK_EX)
        return self

    def __exit__(self, *exc: object) -> None:
        self._file.close()


class AllyLock:
    """Holds the Ally's heavy lock for as long as this process keeps the ssh session's stdin open."""

    def __enter__(self) -> AllyLock:
        self._holder = subprocess.Popen(
            ["ssh", ALLY, f"flock {ALLY_LOCK} sh -c 'echo locked; cat >/dev/null'"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True)
        if self._holder.stdout is None or self._holder.stdout.readline().strip() != "locked":
            raise SystemExit("[heavy] could not take the Ally's heavy lock")
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


def build_command(repo: Path, target: str | None, build_dir: str) -> list[str]:
    framework, _, _ = workspace_paths(repo)
    if repo == framework:
        return ["uv", "run", "--frozen", "python", "tools/build.py", "--build", build_dir]
    if target is None:
        raise SystemExit("[heavy] a title build needs --target")
    return ["uv", "run", "--frozen", "python", "external/psxport/tools/port/consumer_build.py", "--build",
            build_dir, "--target", target]


def ally_shell(remote_repo: str, command: list[str], env: dict[str, str], box: bool) -> int:
    assignments = " ".join(f"{key}={shlex.quote(value)}" for key, value in env.items())
    inner = f"env --chdir=$HOME/{remote_repo} {assignments} {shlex.join(command)}"
    if box:
        inner = f"distrobox enter {ALLY_BOX} -- sh -c {shlex.quote(inner)}"
    return subprocess.run(["ssh", ALLY, inner]).returncode


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
    host = choose_host(args.host, args)
    compile_env = {"CC": "clang", "CXX": "clang++"}
    build = build_command(repo, args.target, args.build) if args.step == "build" or args.target else None
    command = args.command
    if args.step == "run" and not command:
        raise SystemExit("[heavy] run needs a command after --")
    framework, _, _ = workspace_paths(repo)
    # A title run reads the overlay's fonts and menu from the framework checkout.
    title_env = {} if repo == framework else {"PSXPORT_ASSET_DIR": "external/psxport"}
    run_env = {**AGENT_RUN_ENV, **title_env, **dict(item.split("=", 1) for item in args.env)}

    if host == MAC:
        return mac_step(repo, args, build, run_env)

    if host == "local":
        with LocalLock():
            if build is not None:
                code = subprocess.run(build, cwd=repo, env={**os.environ, **compile_env}).returncode
                if code != 0:
                    return code
            if args.step == "run":
                return subprocess.run(command, cwd=repo, env={**os.environ, **run_env}).returncode
            return 0

    with AllyLock():
        remote_repo = mirror(repo, args.push)
        if build is not None:
            code = ally_shell(remote_repo, build, compile_env, box=True)
            if code != 0:
                return code
        code = 0
        if args.step == "run":
            env = {**run_env, **({} if args.box else ally_display_env())}
            code = ally_shell(remote_repo, command, env, box=args.box)
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
    argv = sys.argv[1:]
    split = argv.index("--") if "--" in argv else len(argv)
    args = parser.parse_args(argv[:split])
    args.command = argv[split + 1:]
    return run_step(args)


if __name__ == "__main__":
    sys.exit(main())

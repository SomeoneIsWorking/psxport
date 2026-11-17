"""The Mac's side of tools/heavy.py: takes heavy steps queued on Fedora and runs them here, sealed off.

Fedora cannot reach the Mac, so this worker pulls: it touches <queue>/mac.alive on Fedora while it serves,
claims the oldest unclaimed <queue>/<id>/job.json by creating <id>/claimed, mirrors the trees the job names into
its own work area, builds and runs, copies the job's `fetch` dirs back, streams output to <id>/log and writes the
exit code to <id>/done last. One job at a time.

A job is code from another machine, and this Mac holds confidential work, so a job never gets a free hand:

- Only psxport's own build and run shapes are accepted (build.py / consumer_build.py into build/agent, a built
  product under build/agent/bin, or a python script under the repo's tools/). Anything else is refused.
- Every build and run happens inside job.sb (macOS sandbox): it reads and writes only its work area, reads the
  disc images, and cannot read the rest of the home folder, the keychain, the clipboard or local services.
- Only the fetch dirs go back, copied file by file from an opened descriptor: links and multiply-linked files
  are refused, so a job cannot point the copy at anything outside its work area.
- The worker runs from its installed copy (install.sh), never from a checkout Fedora can push to.

Runs under launchd (com.psxport.heavy-worker); `python3 -I heavy_mac_worker.py` by hand works too.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import stat
import subprocess
import sys
import threading
import time
import traceback
from pathlib import Path
from typing import Dict, List, Optional

FEDORA = "fedora"
QUEUE = "/home/bhamil/agents/queue"
HOME = Path.home()
ROOT = HOME / "heavy-worker"            # installed worker, its sandbox profile and tool copies
TOOLS = ROOT / "bin"
WORK = ROOT / "work"                    # the only place a job can read or write
MIRROR = WORK / "repo"                  # psx/<repo>, psx/psxport, shared/<dep>
STAGING = ROOT / "staging"              # fetch copies, outside the job's reach
LOGS = ROOT / "logs"
DISCS = HOME / "Games" / "PSX"
PROFILE = ROOT / "job.sb"
CONTROL = ROOT / "ssh-%C"

HEARTBEAT_S = 30
POLL_S = 5
JOB_TIMEOUT_S = 4 * 3600
# .git stays: the build reads vendored dependencies' git identity (cmake/dependency_prefix.cmake).
MIRROR_EXCLUDES = ("/build/", "/scratch/", "/env-assets/", "/.env", "__pycache__", ".venv")

FEDORA_TREE = re.compile(r"^/home/bhamil/repo/(psx|shared)/([A-Za-z0-9][A-Za-z0-9._-]*)$")
JOB_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]*$")
PRODUCT = re.compile(r"^[a-z0-9][a-z0-9_]*$")
RELATIVE = re.compile(r"^[A-Za-z0-9_][A-Za-z0-9._/-]*$")
SCRIPT = re.compile(r"^(external/psxport/)?tools/[A-Za-z0-9_/]+\.py$")
RUN_ENV_KEY = re.compile(r"^PSXPORT_[A-Z0-9_]+$")
UV_PYTHON = ["uv", "run", "--frozen", "python"]
COMPILE_ENV = {"CC": "clang", "CXX": "clang++"}


class Refused(Exception):
    """The job asked for something outside what the worker runs."""


def ssh(command: str, **kwargs) -> subprocess.CompletedProcess:
    return subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=10", "-o", "ControlMaster=auto",
                           "-o", f"ControlPath={CONTROL}", "-o", "ControlPersist=300", FEDORA, command], **kwargs)


def rsync(*args: str) -> int:
    rsh = f"ssh -o BatchMode=yes -o ControlMaster=auto -o ControlPath={CONTROL} -o ControlPersist=300"
    return subprocess.run(["rsync", "-e", rsh, *args]).returncode


# --- the job, checked ---------------------------------------------------------------------------------------


def tree(path: object) -> Path:
    match = FEDORA_TREE.match(path) if isinstance(path, str) else None
    if match is None:
        raise Refused(f"not a workspace checkout on Fedora: {path!r}")
    return MIRROR / match.group(1) / match.group(2)


def relative(path: object, what: str) -> str:
    if not isinstance(path, str) or not RELATIVE.match(path) or ".." in path.split("/") or "//" in path:
        raise Refused(f"{what} must be a plain repo-relative path: {path!r}")
    return path.rstrip("/")


def strings(value: object, what: str) -> List[str]:
    if not isinstance(value, list) or not all(isinstance(item, str) and "\0" not in item for item in value):
        raise Refused(f"{what} must be a list of strings")
    return value


def checked_build(build: object, is_framework: bool) -> Optional[List[str]]:
    if build is None:
        return None
    argv = strings(build, "build")
    if is_framework and argv == [*UV_PYTHON, "tools/build.py", "--build", "build/agent"]:
        return argv
    title = [*UV_PYTHON, "external/psxport/tools/port/consumer_build.py", "--build", "build/agent", "--target"]
    if not is_framework and len(argv) == len(title) + 1 and argv[:-1] == title and PRODUCT.match(argv[-1]):
        return argv
    raise Refused(f"build is not one of psxport's build commands into build/agent: {argv}")


def checked_command(command: object) -> Optional[List[str]]:
    if command is None:
        return None
    argv = strings(command, "command")
    if argv and argv[0].startswith("build/agent/bin/") and PRODUCT.match(argv[0][len("build/agent/bin/"):]):
        return argv
    if len(argv) > 4 and argv[:4] == UV_PYTHON and SCRIPT.match(argv[4]) and ".." not in argv[4].split("/"):
        return argv
    raise Refused(f"command is not a built product under build/agent/bin or a tools/ script: {argv[:5]}")


def checked_run_env(env: object) -> Dict[str, str]:
    if not isinstance(env, dict):
        raise Refused("run_env must be an object")
    out = {}
    for key, value in env.items():
        if not isinstance(key, str) or not RUN_ENV_KEY.match(key) or not isinstance(value, str) or "\0" in value:
            raise Refused(f"run_env takes PSXPORT_* string values only: {key!r}")
        out[key] = value
    out.update(PSXPORT_SHOW_WINDOW="0", PSXPORT_NOAUDIO="1")
    return out


class Job:
    def __init__(self, spec: dict) -> None:
        self.repo = tree(spec.get("repo"))
        self.framework = tree(spec.get("framework"))
        self.sources = {self.repo: spec["repo"], self.framework: spec["framework"]}
        for path in spec.get("shared") or []:
            mirror = tree(path)
            if mirror.parent.name != "shared":
                raise Refused(f"shared entry is not under repo/shared: {path!r}")
            self.sources[mirror] = path
        if self.framework.parent.name != "psx" or self.repo.parent.name != "psx":
            raise Refused("repo and framework must be under repo/psx")
        self.build = checked_build(spec.get("build"), self.repo == self.framework)
        self.command = checked_command(spec.get("command"))
        compile_env = spec.get("compile_env") or {}
        if not isinstance(compile_env, dict) or any(COMPILE_ENV.get(k) != v for k, v in compile_env.items()):
            raise Refused(f"compile_env takes CC=clang and CXX=clang++ only: {compile_env}")
        self.run_env = checked_run_env(spec.get("run_env") or {})
        self.push = [relative(path, "push") for path in strings(spec.get("push") or [], "push")]
        self.fetch = [relative(path, "fetch") for path in strings(spec.get("fetch") or [], "fetch")]


# --- doing it -----------------------------------------------------------------------------------------------


class Log:
    """Appends to <id>/log on Fedora as output arrives, and keeps a copy here."""

    def __init__(self, job_id: str) -> None:
        LOGS.mkdir(parents=True, exist_ok=True)
        self._local = (LOGS / f"{job_id}.log").open("ab")
        self._remote = subprocess.Popen(
            ["ssh", "-o", "BatchMode=yes", "-o", "ControlMaster=auto", "-o", f"ControlPath={CONTROL}",
             "-o", "ControlPersist=300", FEDORA, f"cat >> {QUEUE}/{job_id}/log"], stdin=subprocess.PIPE)

    def write(self, data: bytes) -> None:
        self._local.write(data)
        self._local.flush()
        if self._remote.stdin is not None:
            try:
                self._remote.stdin.write(data)
                self._remote.stdin.flush()
            except BrokenPipeError:
                pass

    def line(self, text: str) -> None:
        self.write(f"[heavy-mac] {text}\n".encode())

    def close(self) -> None:
        if self._remote.stdin is not None:
            self._remote.stdin.close()
        self._remote.wait()
        self._local.close()


def seed_work_area() -> None:
    """The job's HOME, caches and temp live in WORK; uv's Python is copied in once. psxport's dependency cache
    is built here by the first job, not copied: its pkg-config files name their own prefix."""
    for path in (WORK / "home", WORK / "tmp", WORK / "cache", MIRROR / "psx", MIRROR / "shared"):
        path.mkdir(parents=True, exist_ok=True)
    python = HOME / ".local" / "share" / "uv" / "python"
    if python.is_dir() and not (WORK / "uv-python").exists():
        shutil.copytree(python, WORK / "uv-python", symlinks=True)


def mirror(job: Job, log: Log) -> None:
    excludes = [f"--exclude={pattern}" for pattern in MIRROR_EXCLUDES]
    for local, remote in job.sources.items():
        local.mkdir(parents=True, exist_ok=True)
        if rsync("-a", "--delete", *excludes, f"{FEDORA}:{remote}/", f"{local}/") != 0:
            raise Refused(f"could not mirror {remote}")
    remote_repo = job.sources[job.repo]
    for path in job.push:
        target = job.repo / path
        target.parent.mkdir(parents=True, exist_ok=True)
        if rsync("-a", f"{FEDORA}:{remote_repo}/{path}", f"{target.parent}/") != 0:
            raise Refused(f"could not copy push path {path}")
    own_env = HOME / "repo" / "psx" / job.repo.name / ".env"
    if own_env.is_file():
        shutil.copyfile(own_env, job.repo / ".env")
    log.line(f"mirrored {len(job.sources)} trees into {MIRROR}")


def toolchain() -> Dict[str, str]:
    """The real compiler directory and SDK. /usr/bin/clang is xcrun's stub, which keeps its cache in the
    per-user temp dir the sandbox closes, so jobs get the toolchain's own binaries and a fixed SDKROOT."""
    clang = subprocess.run(["xcrun", "--find", "clang"], capture_output=True, text=True, check=True).stdout
    sdk = subprocess.run(["xcrun", "--show-sdk-path"], capture_output=True, text=True, check=True).stdout
    return {"bin": str(Path(clang.strip()).parent), "SDKROOT": sdk.strip()}


def sandboxed(argv: List[str], cwd: Path, env: Dict[str, str], log: Log) -> int:
    tools = toolchain()
    base = {
        "PATH": f"{TOOLS}:{tools['bin']}:/opt/homebrew/bin:/usr/bin:/bin:/usr/sbin:/sbin",
        "SDKROOT": tools["SDKROOT"],
        "HOME": str(WORK / "home"), "TMPDIR": f"{WORK / 'tmp'}/", "XDG_CACHE_HOME": str(WORK / "cache"),
        "UV_CACHE_DIR": str(WORK / "cache" / "uv"), "UV_PYTHON_INSTALL_DIR": str(WORK / "uv-python"),
        "UV_NO_CONFIG": "1", "LANG": "en_US.UTF-8",
    }
    params = {"WORK": WORK, "DISCS": DISCS, "TOOLS": TOOLS, "ROOT": ROOT, "HOME_REAL": HOME}
    words = ["/usr/bin/sandbox-exec", "-f", str(PROFILE)]
    for key, value in params.items():
        words += ["-D", f"{key}={os.path.realpath(value)}"]
    words += ["/usr/bin/env", "-i", *(f"{k}={v}" for k, v in {**base, **env}.items()), *argv]
    process = subprocess.Popen(words, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               start_new_session=True)
    deadline = time.monotonic() + JOB_TIMEOUT_S
    assert process.stdout is not None
    for chunk in iter(lambda: process.stdout.read1(65536), b""):
        log.write(chunk)
        if time.monotonic() > deadline:
            log.line(f"timed out after {JOB_TIMEOUT_S} s")
            break
    try:
        os.killpg(process.pid, 9)  # whatever the job left running
    except OSError:
        pass  # the group is already gone
    return process.wait()


def stage_fetch(source: Path, target: Path, log: Log) -> None:
    """Copies `source` to `target` file by file through descriptors opened without following links; refuses
    links, devices and multiply-linked files so nothing outside the job's own output can ride along."""
    root = os.path.realpath(source)
    if not root.startswith(os.path.realpath(WORK) + os.sep) or not os.path.isdir(root):
        log.line(f"fetch {source} is not a directory in the work area; skipped")
        return
    for dirpath, dirnames, filenames, dirfd in os.fwalk(root, follow_symlinks=False):
        out = target / os.path.relpath(dirpath, root)
        out.mkdir(parents=True, exist_ok=True)
        for name in filenames:
            try:
                fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW, dir_fd=dirfd)
            except OSError:
                log.line(f"fetch: skipped {Path(dirpath, name)} (a link or unreadable)")
                continue
            try:
                info = os.fstat(fd)
                if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
                    log.line(f"fetch: refused {Path(dirpath, name)} (not a plain file of its own)")
                    continue
                with open(fd, "rb", closefd=False) as reader, (out / name).open("wb") as writer:
                    shutil.copyfileobj(reader, writer)
            finally:
                os.close(fd)


def fetch_back(job: Job, log: Log) -> None:
    remote_repo = job.sources[job.repo]
    for path in job.fetch:
        staged = STAGING / path
        shutil.rmtree(STAGING, ignore_errors=True)
        staged.mkdir(parents=True)
        stage_fetch(job.repo / path, staged, log)
        ssh(f"mkdir -p {remote_repo}/{path}", check=False)
        if rsync("-a", f"{staged}/", f"{FEDORA}:{remote_repo}/{path}/") != 0:
            log.line(f"could not copy {path} back")
    shutil.rmtree(STAGING, ignore_errors=True)


def run_job(job_id: str) -> int:
    log = Log(job_id)
    try:
        raw = ssh(f"cat {QUEUE}/{job_id}/job.json", capture_output=True, check=True).stdout
        job = Job(json.loads(raw))
        log.line(f"job {job_id} on the Mac (sandboxed in {WORK})")
        seed_work_area()
        mirror(job, log)
        code = 0
        if job.build is not None:
            log.line("build: " + " ".join(job.build))
            code = sandboxed(job.build, job.repo, COMPILE_ENV, log)
        if code == 0 and job.command is not None:
            log.line("run: " + " ".join(job.command))
            code = sandboxed(job.command, job.repo, job.run_env, log)
        fetch_back(job, log)
        log.line(f"exit {code}")
        return code
    except Refused as refusal:
        log.line(f"refused: {refusal}")
        return 97
    except Exception:
        log.write(traceback.format_exc().encode())
        return 98
    finally:
        log.close()


# --- serving ------------------------------------------------------------------------------------------------


def heartbeat(stop: threading.Event) -> None:
    while not stop.is_set():
        ssh(f"mkdir -p {QUEUE} && touch {QUEUE}/mac.alive", capture_output=True)
        stop.wait(HEARTBEAT_S)


def next_job() -> Optional[str]:
    listing = ssh(f"cd {QUEUE} 2>/dev/null && for d in */; do d=${{d%/}}; "
                  f"[ -f \"$d/job.json\" ] && [ ! -e \"$d/claimed\" ] && echo \"$d\"; done",
                  capture_output=True, text=True)
    for job_id in sorted(listing.stdout.split()):
        if JOB_ID.match(job_id) and ssh(f"mkdir {QUEUE}/{job_id}/claimed", capture_output=True).returncode == 0:
            return job_id
    return None


def main() -> int:
    if Path(__file__).resolve().parent != ROOT.resolve():
        print(f"heavy-mac: run the installed copy in {ROOT} (tools/heavy_mac/install.sh)", file=sys.stderr)
        return 2
    stop = threading.Event()
    threading.Thread(target=heartbeat, args=(stop,), daemon=True).start()
    print(f"heavy-mac: serving {FEDORA}:{QUEUE}", flush=True)
    try:
        while True:
            job_id = next_job()
            if job_id is None:
                time.sleep(POLL_S)
                continue
            print(f"heavy-mac: job {job_id}", flush=True)
            code = run_job(job_id)
            ssh(f"printf %s {code} > {QUEUE}/{job_id}/done.tmp && mv {QUEUE}/{job_id}/done.tmp "
                f"{QUEUE}/{job_id}/done", check=False)
            print(f"heavy-mac: job {job_id} exit {code}", flush=True)
    finally:
        stop.set()


if __name__ == "__main__":
    sys.exit(main())

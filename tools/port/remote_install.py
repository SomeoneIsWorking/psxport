"""Install this title's player build on another machine over ssh, with its menu and Desktop shortcut.

    uv run --frozen python external/psxport/tools/port/remote_install.py --host HOST --rom-map LOCAL=REMOTE

Run from the title root. The title's own launcher, run with the shortcut's no-rebuild switch, writes
its product exec as a plan instead of running it; this engine stages that executable, the shared
libraries outside the system directories, the framework UI assets, the title's ``.env`` and a
``play.sh`` that reproduces the exec, copies them to ``~/Games/<product>/`` on HOST, and installs the
title's desktop entry there pointing at ``play.sh``. Disc paths under LOCAL are rewritten to the same
place under REMOTE (relative to the remote home). Files the product provisions at run time beside
itself (``scratch/``) survive a reinstall. The player's settings are copied only when HOST has none.
"""

from __future__ import annotations

import argparse
import json
import os
import shlex
import shutil
import subprocess
import sys
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from automation.process import ToolError
from port import desktop_shortcut, launcher

STAGE = Path("scratch") / "remote_install"
# Libraries here are expected on the host too; copy_tree adds the ones it lacks.
SYSTEM_LIBRARIES = ("/lib/", "/lib64/", "/usr/lib/", "/usr/lib64/")
# Set by play.sh from its own location or the remote user's state directory, never carried over.
HOST_KEYS = frozenset({"PSXPORT_ASSET_DIR", "PSXPORT_LOG_FILE", launcher.RUN_BUILT})
# The product's runtime-provisioned files beside play.sh, kept across reinstalls.
KEPT = "/scratch/"


@dataclass(frozen=True)
class Plan:
    program: Path
    arguments: list[str]
    environment: dict[str, str]
    cwd: Path

    @property
    def product(self) -> str:
        return self.program.name


@dataclass(frozen=True)
class RomMap:
    local: Path
    remote: PurePosixPath  # relative to the remote home

    def remote_path(self, value: str, home: PurePosixPath) -> str | None:
        """``value`` moved under the remote ROM root, or None when it is not under the local one."""
        path = Path(value)
        if not path.is_absolute() or not path.is_relative_to(self.local):
            return None
        return str(home / self.remote / path.relative_to(self.local))


def ssh(host: str, command: str, *, capture: bool = True) -> str:
    completed = subprocess.run(["ssh", "-o", "ConnectTimeout=5", host, command], check=False, text=True,
                               capture_output=capture)
    if completed.returncode != 0:
        raise ToolError(f"ssh {host}: `{command}` failed: {(completed.stderr or '').strip()}")
    return completed.stdout if capture else ""


def launch_plan(root: Path) -> Plan:
    """The title launcher's product exec, recorded instead of run, on the newest build already made."""
    out = root / STAGE / "plan.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.unlink(missing_ok=True)
    environment = {**os.environ, launcher.RUN_BUILT: "1", launcher.LAUNCH_PLAN: str(out)}
    completed = subprocess.run([sys.executable, "bootstrap.py"], cwd=root, env=environment, check=False)
    if completed.returncode != 0 or not out.is_file():
        raise ToolError(f"the title launcher wrote no launch plan (exit {completed.returncode})")
    data = json.loads(out.read_text(encoding="utf-8"))
    return Plan(Path(data["program"]), list(data["arguments"]), dict(data["environment"]), Path(data["cwd"]))


def loaded_libraries(program: Path) -> dict[str, Path]:
    """Each shared library the program loads here, by soname."""
    listing = subprocess.run(["ldd", str(program)], check=True, text=True, capture_output=True).stdout
    found = {}
    for line in listing.splitlines():
        name, arrow, rest = line.strip().partition(" => ")
        if not arrow:
            continue
        if rest.startswith("not found"):
            raise ToolError(f"{program} cannot load {name} here")
        found[name] = Path(rest.split("(")[0].strip())
    return found


def mapped(value: str, plan: Plan, roms: RomMap, home: PurePosixPath, carried: list[Path]) -> str:
    """One environment or argument value as the remote install reads it."""
    if (remote := roms.remote_path(value, home)) is not None:
        return remote
    path = Path(value)
    local = path if path.is_absolute() else plan.cwd / path
    if local.is_relative_to(plan.cwd) and (path.is_absolute() or local.exists()):
        carried.append(local.relative_to(plan.cwd))
        return str(local.relative_to(plan.cwd))
    return value


def dotenv(root: Path) -> dict[str, str]:
    """The title's ``.env`` keys, which the product reads from its working directory."""
    source = root / ".env"
    if not source.is_file():
        return {}
    values = {}
    for line in source.read_text(encoding="utf-8").splitlines():
        key, separator, value = line.partition("=")
        if separator and key.strip() and not key.lstrip().startswith("#"):
            values[key.strip()] = value.strip().strip('"').strip("'")
    return values


def play_script(plan: Plan, environment: Mapping[str, str], arguments: Sequence[str]) -> str:
    """The remote launcher: the product exec with each setting overridable from the caller's environment."""
    lines = ["#!/bin/sh", "set -eu", 'here=$(dirname "$(readlink -f "$0")")', 'cd "$here"',
             'export LD_LIBRARY_PATH="$here/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"',
             'export PSXPORT_ASSET_DIR="$here"',
             f'log="${{XDG_STATE_HOME:-$HOME/.local/state}}/psxport/{plan.product}/last-run.log"',
             'mkdir -p "$(dirname "$log")"', 'export PSXPORT_LOG_FILE="${PSXPORT_LOG_FILE:-$log}"']
    for key, value in sorted(environment.items()):
        lines.append(f'export {key}="${{{key}:-{value}}}"' if '"' not in value and "$" not in value
                     else f"export {key}={shlex.quote(value)}")
    command = " ".join(shlex.quote(part) for part in [f"bin/{plan.product}", *arguments])
    lines.append(f'exec ./{command} "$@"')
    return "\n".join(lines) + "\n"


def stage(root: Path, plan: Plan, roms: RomMap, home: PurePosixPath) -> Path:
    """The install tree for ``plan`` under the title's scratch, rebuilt from nothing."""
    tree = root / STAGE / plan.product
    if tree.exists():
        shutil.rmtree(tree)
    (tree / "bin").mkdir(parents=True)
    (tree / "lib").mkdir()
    shutil.copy2(plan.program, tree / "bin" / plan.product)
    for library in loaded_libraries(plan.program).values():
        if not str(library).startswith(SYSTEM_LIBRARIES):
            shutil.copy2(library, tree / "lib" / library.name)
    assets = Path(plan.environment.get("PSXPORT_ASSET_DIR", launcher.FRAMEWORK)) / "assets"
    if not assets.is_dir():
        raise ToolError(f"no framework assets at {assets}")
    shutil.copytree(assets, tree / "assets")
    carried: list[Path] = []
    environment = {key: mapped(value, plan, roms, home, carried) for key, value in plan.environment.items()
                   if key.startswith("PSXPORT_") and key not in HOST_KEYS}
    arguments = [mapped(value, plan, roms, home, carried) for value in plan.arguments]
    env_file = {key: mapped(value, plan, roms, home, carried) for key, value in dotenv(root).items()}
    for relative in carried:
        source = plan.cwd / relative
        destination = tree / relative
        destination.parent.mkdir(parents=True, exist_ok=True)
        if source.is_dir():
            shutil.copytree(source, destination)
        elif source.is_file():
            shutil.copy2(source, destination)
        else:
            raise ToolError(f"the launch names {source}, which does not exist")
    (tree / ".env").write_text("".join(f"{key}={value}\n" for key, value in env_file.items()), encoding="utf-8")
    script = tree / "play.sh"
    script.write_text(play_script(plan, environment, arguments), encoding="utf-8")
    script.chmod(0o755)
    return tree


def copy_tree(tree: Path, plan: Plan, host: str, remote: str) -> None:
    """Send the tree, adding each system library of this machine that HOST lacks."""
    ssh(host, f"mkdir -p {shlex.quote(remote)}")
    local = loaded_libraries(plan.program)
    while True:
        subprocess.run(["rsync", "-a", "--delete", f"--filter=P {KEPT}", f"{tree}/", f"{host}:{remote}/"],
                       check=True)
        listing = ssh(host, f"cd {shlex.quote(remote)} && LD_LIBRARY_PATH=lib ldd bin/{plan.product} || true")
        missing = [line.split()[0] for line in listing.splitlines() if "=> not found" in line]
        if not missing:
            return
        absent = [name for name in missing if name not in local or (tree / "lib" / name).exists()]
        if absent:
            raise ToolError(f"{host} cannot load {', '.join(absent)} for {remote}, and this machine has no copy")
        for name in missing:
            shutil.copy2(local[name], tree / "lib" / name)


def install_shortcut(root: Path, host: str, remote: str, data: str, desktop: str) -> str:
    shortcut = desktop_shortcut.resolve(root)
    play = f"{remote}/play.sh"
    text = desktop_shortcut.entry_text(shortcut, desktop_shortcut.quoted_exec(play), play, remote)
    entry = root / STAGE / f"{shortcut.app_id}.desktop"
    entry.write_text(text, encoding="utf-8")
    icons = f"{data}/icons/hicolor/scalable/apps"
    applications = f"{data}/applications"
    ssh(host, f"mkdir -p {shlex.quote(icons)} {shlex.quote(applications)}")
    subprocess.run(["rsync", str(shortcut.icon), f"{host}:{icons}/"], check=True)
    destinations = [applications, desktop] if desktop else [applications]
    for directory in destinations:
        subprocess.run(["rsync", "--chmod=755", str(entry), f"{host}:{directory}/"], check=True)
    for command in desktop_shortcut.cache_commands(data):
        ssh(host, f"! command -v {command[0]} >/dev/null || {shlex.join(command)}")
    return f"{applications}/{entry.name}"


def seed_settings(host: str, product: str, home: PurePosixPath) -> None:
    """Copy the player's settings to HOST only when it has none of its own."""
    local = Path.home() / ".config" / product / "settings.ini"
    remote = home / ".config" / product / "settings.ini"
    if not local.is_file() or ssh(host, f"test -e {shlex.quote(str(remote))} && echo kept || true").strip():
        return
    ssh(host, f"mkdir -p {shlex.quote(str(remote.parent))}")
    subprocess.run(["rsync", str(local), f"{host}:{remote}"], check=True)


def rom_map(value: str) -> RomMap:
    local, separator, remote = value.partition("=")
    if not separator or not local or not remote:
        raise argparse.ArgumentTypeError("--rom-map takes LOCAL=REMOTE")
    return RomMap(Path(local), PurePosixPath(remote))


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", required=True, help="the ssh host to install on")
    parser.add_argument("--rom-map", type=rom_map, required=True, metavar="LOCAL=REMOTE",
                        help="the local ROM root and the same tree on the host, relative to its home")
    args = parser.parse_args(argv)
    root = Path.cwd().resolve()
    try:
        home = PurePosixPath(ssh(args.host, 'printf %s "$HOME"'))
        data = ssh(args.host, 'printf %s "${XDG_DATA_HOME:-$HOME/.local/share}"')
        desktop = ssh(args.host, "xdg-user-dir DESKTOP 2>/dev/null || true").strip()
        plan = launch_plan(root)
        remote = str(home / "Games" / plan.product)
        tree = stage(root, plan, args.rom_map, home)
        copy_tree(tree, plan, args.host, remote)
        seed_settings(args.host, plan.product, home)
        entry = install_shortcut(root, args.host, remote, data, desktop)
    except (ToolError, OSError, subprocess.CalledProcessError) as error:
        print(f"[remote-install] FAILED: {error}", file=sys.stderr)
        return 1
    print(f"[remote-install] {plan.product} on {args.host}: {remote}/play.sh, {entry}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

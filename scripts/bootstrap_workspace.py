#!/usr/bin/env python3
"""Reproduce the PSX port workspace around this framework checkout."""

from __future__ import annotations

import subprocess
import sys
from collections.abc import Sequence
from pathlib import Path

GITHUB_ROOT = "https://github.com/SomeoneIsWorking"
REMOTE_BACKED = ("spyro", "spider1", "Tomba2Engine", "vagrant", "megamanx4", "crash", "ctr", "crashbash", "tekken3")
LOCAL_ONLY: tuple[str, ...] = ()


def say(message: str) -> None:
    print(f"[bootstrap] {message}")


def run(arguments: Sequence[str | Path], *, cwd: Path, required: bool = True) -> subprocess.CompletedProcess[str]:
    command = [str(argument) for argument in arguments]
    completed = subprocess.run(command, cwd=cwd, check=False, capture_output=True, text=True)
    if required and completed.returncode:
        raise RuntimeError(f"command failed ({completed.returncode}): {' '.join(command)}\n{completed.stderr.strip()}")
    return completed


def submodule_paths(repo: Path) -> list[str]:
    modules = repo / ".gitmodules"
    if not modules.is_file():
        return []
    result = run(
        ["git", "config", "-f", modules, "--get-regexp", r"^submodule\..*\.path$"],
        cwd=repo,
        required=False,
    )
    if result.returncode not in (0, 1):
        raise RuntimeError(f"cannot enumerate {modules}: {result.stderr.strip()}")
    return [line.split(None, 1)[1] for line in result.stdout.splitlines() if len(line.split(None, 1)) == 2]


def initialize_framework_vendors(framework: Path) -> None:
    for relative in ("vendor/beetle-psx", "vendor/lucent"):
        run(["git", "submodule", "update", "--init", relative], cwd=framework)
        run(["git", "reset", "--hard", "-q", "HEAD"], cwd=framework / relative)
    run(["git", "submodule", "update", "--init", "deps/libchdr"], cwd=framework / "vendor/beetle-psx")


def sync_game_framework(game: Path) -> None:
    synchronizer = game / "tools/psxport_sync.py"
    state = "psxport_sync.py --auto" if synchronizer.is_file() else "no sync tool yet"
    say(f"{game.name}: establish external/psxport ({state})")
    if not synchronizer.is_file():
        return
    result = run([sys.executable, synchronizer, "--auto"], cwd=game, required=False)
    if result.returncode:
        say(f"{game.name}: psxport_sync.py --auto did not resolve external/psxport")


def main() -> int:
    framework = Path(__file__).resolve().parent.parent
    workspace = framework.parent
    try:
        say(f"framework dev clone: {framework}")
        initialize_framework_vendors(framework)
        run(["git", "submodule", "update", "--init", "external/psycross"], cwd=framework)

        cloned = 0
        present = 0
        for name in REMOTE_BACKED:
            game = workspace / name
            if (game / ".git").exists():
                head = run(["git", "rev-parse", "--short", "HEAD"], cwd=game).stdout.strip()
                say(f"{name}: present ({head})")
                present += 1
            else:
                say(f"cloning {name}…")
                run(["git", "clone", f"{GITHUB_ROOT}/{name}.git", game], cwd=workspace)
                cloned += 1
            for relative in submodule_paths(game):
                if relative == "external/psxport":
                    continue
                result = run(["git", "submodule", "update", "--init", relative], cwd=game, required=False)
                if result.returncode:
                    say(f"{name}: submodule {relative} not initialised")
            sync_game_framework(game)

        missing: list[str] = []
        for name in LOCAL_ONLY:
            game = workspace / name
            if (game / ".git").exists():
                head = run(["git", "rev-parse", "--short", "HEAD"], cwd=game).stdout.strip()
                say(f"{name}: present ({head}) — LOCAL ONLY, has no remote")
                sync_game_framework(game)
            else:
                missing.append(name)

        guide = workspace / "CLAUDE.md"
        if not guide.exists() and not guide.is_symlink():
            guide.symlink_to("psxport/docs/workspace/WORKSPACE.md")
            say(f"linked {guide} -> psxport/docs/workspace/WORKSPACE.md")
        (workspace / "coord/claims").mkdir(parents=True, exist_ok=True)

        total = len(REMOTE_BACKED) + len(LOCAL_ONLY)
        have = present + cloned + len(LOCAL_ONLY) - len(missing)
        say(f"game trees: {have} of {total} present ({cloned} cloned, {present} already here)")
        if missing:
            say(f"NOT REPRODUCED — {len(missing)} of {total} game trees are missing: {' '.join(missing)}")
            say("give each local-only tree a remote before expecting this workspace to be portable")
        say(f"Framework edits go in {framework} and nowhere else.")
        say(f"Build a game against it with: PSXPORT_DIR={framework} ./run.sh")
        say("A disc image is not provisioned here; each game resolves its user-supplied input.")
        return 0
    except (OSError, RuntimeError) as error:
        print(f"[bootstrap] REFUSED: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

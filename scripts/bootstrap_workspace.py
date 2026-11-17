#!/usr/bin/env python3
"""Reproduce the PSX port workspace around this framework checkout."""

from __future__ import annotations

import subprocess
import sys
from collections.abc import Sequence
from pathlib import Path

GITHUB_ROOT = "https://github.com/SomeoneIsWorking"
REMOTE_BACKED = ("psx-spyro", "psx-spider-man", "psx-tomba", "psx-vagrant-story", "psx-mega-man-x4", "psx-crash", "psx-crash-team-racing", "psx-crash-bash", "psx-tekken-3", "psx-toy-story-2", "psx-c12")
LOCAL_ONLY: tuple[str, ...] = ()
# Libraries cmake/shared_dependency.cmake builds from a `shared/<repo>` sibling, ahead of the vendored copy.
SHARED_LIVE = ("bug-report", "setup-ui", "touch-ui", "port-assets")

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "tools"))
from psxport_fetch import init_submodules, submodules  # noqa: E402
from submodule_state import Git, declared_entries  # noqa: E402


def say(message: str) -> None:
    print(f"[bootstrap] {message}")


def run(arguments: Sequence[str | Path], *, cwd: Path, required: bool = True) -> subprocess.CompletedProcess[str]:
    command = [str(argument) for argument in arguments]
    completed = subprocess.run(command, cwd=cwd, check=False, capture_output=True, text=True)
    if required and completed.returncode:
        raise RuntimeError(f"command failed ({completed.returncode}): {' '.join(command)}\n{completed.stderr.strip()}")
    return completed


def initialize_framework_vendors(framework: Path) -> None:
    init_submodules(str(framework))
    for parent, relative in submodules(str(framework)):
        if parent == ".":
            run(["git", "reset", "--hard", "-q", "HEAD"], cwd=framework / relative)


def sync_game_framework(framework: Path, game: Path) -> None:
    say(f"{game.name}: establish external/psxport")
    result = run([sys.executable, framework / "tools/psxport_fetch.py", "--auto", "--repo", game], cwd=game, required=False)
    if result.returncode:
        say(f"{game.name}: psxport_fetch.py did not resolve external/psxport")


def refresh_shared(shared: Path) -> None:
    """Bring each live shared checkout up to its remote when that loses nothing; name the ones it cannot move."""
    for name in SHARED_LIVE:
        checkout = shared / name
        if not (checkout / ".git").exists():
            continue
        run(["git", "fetch", "--quiet", "origin"], cwd=checkout)
        upstream = run(["git", "rev-parse", "origin/HEAD"], cwd=checkout, required=False).stdout.strip() or \
            run(["git", "rev-parse", "origin/main"], cwd=checkout).stdout.strip()
        head = run(["git", "rev-parse", "HEAD"], cwd=checkout).stdout.strip()
        if head == upstream:
            say(f"shared/{name}: current ({head[:12]})")
        elif run(["git", "status", "--porcelain"], cwd=checkout).stdout.strip():
            say(f"shared/{name}: STALE with local changes, left at {head[:12]}; the build uses it ahead of vendor/{name}")
        elif run(["git", "merge-base", "--is-ancestor", head, upstream], cwd=checkout, required=False).returncode == 0:
            branch = run(["git", "symbolic-ref", "--quiet", "--short", "HEAD"], cwd=checkout, required=False).stdout.strip()
            if branch:
                run(["git", "merge", "--ff-only", "--quiet", upstream], cwd=checkout)
            else:
                run(["git", "checkout", "--quiet", "--detach", upstream], cwd=checkout)
            say(f"shared/{name}: {head[:12]} -> {upstream[:12]}")
        else:
            say(f"shared/{name}: STALE, {head[:12]} is not in origin's history; left as is, and the build uses it "
                f"ahead of vendor/{name}")


def main() -> int:
    framework = Path(__file__).resolve().parent.parent
    workspace = framework.parent
    try:
        say(f"framework dev clone: {framework}")
        initialize_framework_vendors(framework)
        run(["git", "submodule", "update", "--init", "external/psycross"], cwd=framework)
        refresh_shared(workspace.parent / "shared")

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
            for _name, relative in declared_entries(Git(), game):
                if relative == "external/psxport":
                    continue
                result = run(["git", "submodule", "update", "--init", relative], cwd=game, required=False)
                if result.returncode:
                    say(f"{name}: submodule {relative} not initialised")
            sync_game_framework(framework, game)

        missing: list[str] = []
        for name in LOCAL_ONLY:
            game = workspace / name
            if (game / ".git").exists():
                head = run(["git", "rev-parse", "--short", "HEAD"], cwd=game).stdout.strip()
                say(f"{name}: present ({head}) — LOCAL ONLY, has no remote")
                sync_game_framework(framework, game)
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

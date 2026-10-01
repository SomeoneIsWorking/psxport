# Changing the framework

- Framework edits happen only in `~/repo/psx/psxport` (the main checkout), never in a game's `external/psxport`.
- One agent per repo, no worktrees. An agent blocked by a framework bug fixes it here: `git fetch origin`,
  rebase onto `origin/main`, stage only your own files by path, build in `build/<agent-name>`, commit locally.
  The operator pushes.
- After pushing a framework fix, each game picks it up when its pin is bumped (see `WORKSPACE.md`).

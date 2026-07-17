# Bug board — see the local kanban

The live bug board is a **local, in-repo kanban** at [`docs/kanban/board.md`](kanban/board.md)
(cards under `docs/kanban/cards/`). No GitHub Issues, no network — it travels with the repo.

Use the `bug-tracker` skill / `tools/kanban.py` CLI to interact from a session:

```
tools/kanban.py list                          # board contents (all columns)
tools/kanban.py search <words>                # search titles + bodies
tools/kanban.py show <id>                     # full card
tools/kanban.py add "<title>" --label bug --label <area> --body "<md>"   # new bug (col defaults to todo)
tools/kanban.py move <id> doing               # start a deep chase
tools/kanban.py note <id> "<investigation note>"   # append a dated trail note
tools/kanban.py evidence <id> docs/reference/issues/<file>.png   # attach a screenshot
tools/kanban.py move <id> done                # confirmed fixed (also promote to docs/findings/)
```

Columns: `backlog` | `todo` | `doing` | `done`. Kind/area via labels (`bug` / `render` /
`audio` / `pc-skip` / …). Evidence screenshots live in `docs/reference/issues/` (committed).

Fixed bugs get PROMOTED to `docs/findings/*.md` (root cause + fix) via `tools/findings.py` when
moved to `done`.

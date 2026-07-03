# Bug board — see GitHub Issues

The live bug board is now GitHub Issues at
[**github.com/SomeoneIsWorking/psxport/issues**](https://github.com/SomeoneIsWorking/psxport/issues).

Use the `bug-tracker` skill / `tools/bugs.py` CLI to interact from a session:

```
tools/bugs.py                         # board view (open bug-labelled issues)
tools/bugs.py <words>                 # search across title/body/comments
tools/bugs.py show <n>                # full issue + comments
tools/bugs.py add "<title>" --body "<md>" --label render   # new bug
tools/bugs.py investigating <n>       # mark under active investigation
tools/bugs.py ported <n>              # mark ported-unverified (post-port, awaiting user)
tools/bugs.py comment <n> "<note>"    # append investigation-note
tools/bugs.py close <n> "<note>"      # close with a farewell comment
tools/bugs.py reopen <n> "<note>"     # reopen (e.g. after a port regressed)
```

Status is expressed via labels: `investigating` / `ported-unverified` (mutually
exclusive; absence = OPEN). Area via `bug` / `render` / `audio` / `render-arch` / etc.
The investigation trail lives in the issue's comments; findings get promoted to
`docs/findings/*.md` on close.

Do NOT hand-edit this file with a per-bug list — that pattern was retired 2026-07-03
(user directive: use the tooling, not a duplicate markdown board). This file is only
the pointer.

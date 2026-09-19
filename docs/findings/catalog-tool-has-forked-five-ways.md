# The issue-catalog tool has forked five ways across the workspace

Measured 2026-09-19.

## What was found

`catalog.py` is the tool `info.py` names as the entry point for the issue registry. There is a
canonical implementation in `shared/re-harness/tools/`, reachable at `~/.codex/bin/catalog.py`, which
is a symlink to it. Every psx game repo that has the tool has its own private copy instead, and they
have drifted:

| where | size | state |
|---|---|---|
| `shared/re-harness/tools/catalog.py` | 14,166 B | the canonical implementation |
| `spyro/tools/catalog.py` | 10,751 B | private copy, 102 lines differ from canonical |
| `vagrant/tools/catalog.py` | 11,769 B | private copy |
| `megamanx4/tools/catalog.py` | 11,769 B | private copy |
| `spider1/tools/catalog.py` | 8,754 B | private copy |
| `Tomba2Engine` | — | **none, yet `tools/info.py` tells the reader to use it** |
| `tekken3`, `crash`, `ctr`, `crashbash` | — | none |

Five versions of one tool. The canonical one is the largest and the newest (2026-08-26 against
spyro's 2026-07-29), so the private copies are behind it, not ahead.

## How it was hit

Filing an issue against Tomba! 2 failed with `can't open file '.../tools/catalog.py'`. Tomba2's
`tools/info.py` line 5 documents the issue registry as `docs/issues/ (issue-catalog skill,
catalog.py)`, so the tool it points at is simply absent from that repo. The issue was filed by
invoking the canonical tool with `--dir docs/issues`, which works and needs no copy:

    ~/.codex/bin/catalog.py --dir docs/issues add "<title>" --symptom ... --tags ...

That is the correct invocation for every repo, including the ones that currently have a private copy.

## Why this matters here specifically

The workspace `CLAUDE.md` gives fork drift as the reason `shared/` exists at all — "nine forked
copies of one tool had drifted into seven versions before it". This is that failure, reproduced, in
the tool whose whole job is to stop findings being re-derived. A repo whose `catalog.py` is behind
the canonical one can silently lack a subcommand or a field that a skill or another repo's docs
assume, and the failure mode is a confusing error or a subtly different file format rather than a
clean refusal.

## The fix, which is NOT done

Delete the four private copies and have every repo reach the canonical tool, the way
`external/psxport` already reaches the one framework tree. This is a bounded migration but it is not
one line: each repo's `info.py`, any skill or doc that names `tools/catalog.py`, and each repo's
gate would need updating together, and the four copies must be diffed against canonical first in
case a private copy carries a fix that never went upstream — spyro's differs by 102 lines, which is
too many to assume are all drift.

Until that lands, use `~/.codex/bin/catalog.py --dir docs/issues` rather than adding a fifth copy or
"fixing" a repo by copying one in.

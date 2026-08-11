# `psxport/tools/port/` — the SHARED tool engines

Generic port-tooling ENGINES live here; the DATA they read stays in each game repo. Every game vendors
this repo as `external/psxport`, so an engine here reaches all of them:

```sh
cd <game-repo>
python3 external/psxport/tools/port/re_frontier.py next
```

**Why these were hoisted (measured, not tidiness).** `re_frontier.py`'s green-over-nothing bug — printing
"re-frontier OK: no unknown deps, no cycles, every re-verified step cites evidence" over a parse of ZERO
entries — has been fixed **four times across three diverged copies** (890 / 443 / skill lines), and two of
those copies were still lying on 2026-08-11, one of them the copy a game's CLAUDE.md tells you to run. The
same divergence produced the `RE_FRONTIER_ROADMAP` trap and the `docs/codemap.md` vs `docs/code-map.md`
split. A fix recorded against a tool is not a fix of every copy of that tool.

**The rule for an engine that lives here: it may hold NO game-specific fact.** Paths resolve against the
CURRENT WORKING DIRECTORY (the game repo you run it from), never against this file's location — an engine
that defaulted relative to itself would read the FRAMEWORK's `docs/` for every game, which is the same
class of bug as the framework holding a game's addresses. Verified for `re_frontier.py`: run from
`spider1` it parses that repo's 30 entries, from `spyro` that repo's 28.

**Adoption is ADDITIVE and per-tool, never a flag day.** A game keeps its own `tools/<x>.py` working until
someone is already in that repo and switches it to a shim. There is no window in which a repo has no
working tool, and each step is revertible on its own. See `docs/workspace/WORKSPACE.md` for the decision.

**Every engine here must refuse rather than pass over nothing.** `re_frontier.py check` exits non-zero on
a missing roadmap, on a roadmap that yields zero entries, AND when run from a directory that has no
`docs/` at all — all three verified. That is the bar for anything added here, because a shared engine's
false green is now a false green in every repo at once.

| engine | reads (per game) | hoisted |
|---|---|---|
| `re_frontier.py` | `docs/re-frontier.md` | 2026-08-11 — from spider1's copy, the most developed of the three |

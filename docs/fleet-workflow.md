# Operator + Subagent Fleet Workflow

How to drive psxport ownership/porting at scale: ONE operator (the main session) orchestrates a
fleet of cheap subagents. Proven 2026-07-08 (owned ~40 substrate addresses, fixed the black screen,
found+fixed a fake-green SBS gate, reached true zero-diff). Read this first in a fresh session that
wants to run the fleet.

## 0. Roles — the operator does NOT do the work
- **Operator (main session):** picks targets, dispatches subagents, INTEGRATES their commits onto
  `main`, runs the SBS gate, pushes. Root-causes only when triaging. Never does RE/port/fix by hand.
- **Subagents (sonnet, isolated):** do the RE, port, fix, verify. Each in its OWN git worktree, on a
  disjoint slice, reports a commit SHA + file list. The operator cherry-picks it.
- Hard rule: **no agent ever debugs UNOWNED code** — own it first (port the substrate leaf), then
  debug. Always keep a wave of agents closing the ownership gap.

## 1. Dispatch an ownership wave
- `Agent(subagent_type:'general-purpose', model:'sonnet', isolation:'worktree', prompt:…)`.
  **`isolation:'worktree'` is MANDATORY for any source-editing agent** — without it they inherit the
  session cwd, collide on shared wiring files (cmake/boot.cpp), and break each other's builds.
- Give each agent a **disjoint address band** (e.g. 0x8003xxxx / 0x8004-5xxxx / 0x8006-7xxxx) so two
  agents never touch the same files. Read-only SCOUT/audit agents don't need isolation.
- Prompt skeleton (every ownership agent):
  1. Read CLAUDE.md + faithful-execution.md. RE-first (Ghidra headless; `generated/` C is ground
     truth — Ghidra garbles GTE/COP2). REAL C++ classes in the right `game/` subsystem folder.
  2. Self-select an unowned cluster: `PSXPORT_DEBUG=recdep-all` (uncapped dispatch histogram) on the
     default path to rank busy substrate leaves; confirm each unowned via `tools/codemap.py --addr`.
  3. Wire correctly: EngineOverrides intercepts only NATIVE (rec_dispatch) callers; SUBSTRATE callers
     reach `func_<addr>` via the process-global `g_override[]` (`shard_set_override`). Most leaves are
     substrate-called → use `shard_set_override` (or dual-wire like ActorReward/Math). Per-Game
     EngineOverrides go inside `register_engine_overrides()` (runs for SBS Games too).
  4. **Guest-stack frames: MIRROR, never revert/exclude.** If the substrate body does `addiu sp,-N`
     + spills, reproduce it (`c->r[29] -= N`, spill ra/s0..s3 with LIVE values at RE'd offsets, ascend).
     Reference: game/world/object_table.cpp, game/render/cull.cpp.
  5. Gate: SBS-full 0-diff AND confirm the override FIRES (`PSXPORT_DEBUG=ovhit` flags "registered but
     NEVER HIT" — a 0-diff with your handler never hit is meaningless).
  6. Build in a SEPARATE dir (`build2`); copy `generated/` + MAIN.EXE + re-apply the beetle
     SPU_PokeRAM edit if the link fails. Kill procs with `pids=$(pgrep -x tomba2_port); kill $pids`
     (NEVER `pkill -f`). Report SHA + files (EXCLUDE vendor/beetle-psx + generated/). Do NOT push.

## 2. Integration loop (operator, per landed commit)
Run from the MAIN checkout `<HOME>/repo/psxport` (on `main`); subagent commits are reachable via
the shared object store.
```
git cherry-pick -x <SHA>
# resolve conflicts: generated docs (code-map.md / findings/INDEX.md) -> checkout --theirs then regen;
#   shared wiring files (cmake/boot.cpp) -> UNION merge (keep both sides, dedup) — they are additive.
python3 tools/codemap.py ; python3 tools/findings.py   # regen, git add, --continue
cmake --build build --target tomba2_port -j$(nproc)    # must compile
# SBS-full gate — PUSH ONLY IF 0-diff:
timeout 95 env PSXPORT_VK_HEADLESS=1 PSXPORT_SBS=1 PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1 \
  PSXPORT_NOAUDIO=1 PSXPORT_SBS_NOPAUSE=1 ./scratch/bin/tomba2_port > sbs.log 2>&1   # run in background
grep -cE 'sbs-div|VIOLATION' sbs.log    # 0 -> git push ; nonzero -> HOLD, root-cause
```
- A cherry-pick that entangled wiring files across agents (shared-worktree accident) → union-resolve;
  never blind-`--theirs` on code. Verify no leaked `<<<<<<<`/`=======` markers before continuing.
- EXCLUDE from every pick: `vendor/beetle-psx` bumps + `generated/`.

## 3. Verification discipline — honest gate, never fake-green
- SBS-full (`MODE=full`) is byte-exact: core A = pc_faithful native, core B = recomp substrate. **Every
  diff is fatal.** A cluster that "passes" only because its override never fired is NOT verified — this
  actually happened (SBS Games didn't register EngineOverrides → both cores ran substrate → fake-green).
  Fixed; always confirm firing with `ovhit`.
- A revealed divergence is a FIX TARGET, not a checkmark to hide. Root-cause it (bisect which override,
  `PSXPORT_SBS_PREWATCH=<addr>` + `sbs bt`/`sbs diff`, compare native vs substrate values), fix the port
  to byte-match. The ONLY allowed residual exclusion is memory the recomp side provably never reads.
- Push only 0-diff. Hold anything with a real residual; converge before piling on.

## 4. WORKFLOW-FIRST
A defect in the workflow (a fake-green gate, an unreliable target list, a blind diagnostic, colliding
agents) outranks the task. Fix it, then resume. Examples fixed this way: codemap `beh_*` false-ORPHANs
(reliable target list), `PSXPORT_DEBUG=ovhit` (confirm overrides fire), `recdep-all` (uncapped frontier),
isolation for parallel editors.

## 5. Cadence
Narrate one line before acting; after each landed commit restate what happened. Report the SHA range on
each push. Keep a standing wave so the ownership gap always closes; only pause piling on when the gate is
red (converge first). See also: docs/faithful-execution.md, docs/port-progress.md, tools/codemap.py,
tools/findings.py, docs/config.md (PSXPORT_DEBUG channels).

## 6. Two tiers: frontier (verified) + wide-RE (ahead of it)
Decouple expensive RE from the serial 0-diff gate by running two kinds of agent at once:
- **Frontier tier (verified ownership):** the tight loop of §1–§3 — own → wire → SBS-full 0-diff →
  operator integrates → push. One coherent cluster at a time; every push is byte-verified. This is what
  actually advances `main`'s trustworthy coverage.
- **Wide-RE tier (ahead of the frontier):** a BIGGER fleet on disjoint address regions doing deep RE
  into named struct types + FAITHFUL DRAFT native methods that stay **UNWIRED and UNVERIFIED** — no
  override registration, no SBS run. Drafts are dead code that must still COMPILE (added to cmake). They
  bank understanding + a ready-to-wire port so that when the frontier reaches a region, wiring+gating is
  a fast follow instead of a from-scratch RE. Deliverable is RE docs (engine_re.md / code-map / findings)
  + draft classes; the operator integrates the docs freely and the drafts compile-checked (they can't
  affect the gate because nothing calls them).
- Wiring a banked draft later = move it onto the frontier tier: register it, SBS-gate 0-diff, push. Mirror
  guest-stack frames at draft time (per the CLAUDE.md directive) so the later gate passes first try.
- Sizing: wide-RE can be many agents (RE is embarrassingly parallel and gate-free); the frontier tier
  stays serial-ish because each push must gate 0-diff. Don't pile frontier integrations while the gate is
  red — converge first. Wide-RE never blocks on the gate, so it can always run.

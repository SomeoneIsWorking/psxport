# Operator + Subagent Fleet Workflow

How to drive psxport ownership/porting at scale: ONE operator (the main session) orchestrates a
fleet of cheap subagents. Proven 2026-07-08 (owned ~40 substrate addresses, fixed the black screen,
found+fixed a fake-green SBS gate, reached true zero-diff). Read this first in a fresh session that
wants to run the fleet.

## 0. Roles — model tiers by task shape (★USER 2026-07-10)
Sonnet is NOT capable of open-ended "fix X" / root-cause work — the 2026-07-10 session produced a
string of confidently-wrong sonnet diagnoses (each needing a later correction pass) before this rule.
Task verbs decide the model tier:
- **Sonnet (cheap, fleets):** mechanical, contract-verifiable execution ONLY — collect evidence
  (traces/dumps/censuses, NO interpretation), transcribe against `generated/` ground truth with the
  port framework (`port_gen`/`abi_extract`/`port_check`), apply an EXACT fix spec written by a
  higher tier, run gates, integrate-style chores. A sonnet prompt must never contain the verbs
  "root-cause", "figure out why", or the word "fix" at all — spell out in detail exactly what to do,
  and keep each sonnet task SMALL in scope (one function, one trace, one spec application; split
  anything bigger into multiple small tasks or move it up a tier).
- **Fable (main session / high-tier agents):** diagnosis, root-cause, fix DESIGN, adversarial
  verification of applied fixes, and anything where a wrong-but-plausible answer costs more than the
  tokens saved. For big problems use the ultracode Workflow pipeline:
  **Evidence (sonnet fan-out) → Diagnose (Fable, writes ROOT CAUSE + exact FIX SPEC + PREDICTIONS)
  → Execute (sonnet, applies spec verbatim, may only report SPEC MISMATCH — never improvise)
  → Verify (Fable, adversarial PASS/BOUNCE against the predictions; BOUNCE loops to Diagnose).**
  Keep Fable agent counts low (session-limit risk); if a Fable stage dies, the operator does that
  stage in the main loop from the evidence.
- **Operator (main session):** picks targets, authors specs/workflows, INTEGRATES commits onto
  `main`, runs the gates, pushes, and personally reviews diffs + drives the game after each batch.
  The operator READS CODE, REs, and edits directly — do not route thinking-work through agents to
  avoid it (USER 2026-07-10: "you need to be aware of the codebase as well; use Sonnet for manual
  labor"). Delegate only mechanical labor: long trace collection, spec application across files,
  gate runs, transcription. The 2026-07-10 cause-#3 diagnosis (stale-a0 matMul clobber) was found
  operator-hands-on in ~10 tool calls after two agent rounds produced only partial evidence.
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
     **Start the draft from `tools/port_gen.py <addr> --class Foo --method bar`** (see
     `docs/port-framework.md`) — it emits the gen body verbatim as a compilable class method, so the
     agent's job is renaming/restructuring (verified by `tools/port_check.py`) instead of hand-
     transcribing the ABI/stack shape from scratch. For a from-scratch hand-write (no `port_gen`
     draft), use `runtime/recomp/guest_abi.h`'s `GuestReg`/`GuestFrame`/`guest_call`/`guest_dispatch`
     instead of raw `c->r[]` so the callee-saved-liveness and missing-`ra` bug classes are
     structurally harder to write.
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
# SBS-full gate — PUSH ONLY IF 0-diff, BOTH legs. Leg 1: AUTONAV=combat = standard nav + a
# deterministic melee encounter (2026-07-10; exercises ActorMeleeEngage/MeleeProximity, which plain
# =1 never reaches). Leg 2: WATCH_CUT = the intro cutscene PLAYS OUT (2026-07-10; combat mashes
# Start through it, so cutscene-path natives were never exercised — this leg found and now guards
# the attach/loadFrame/op3E-trampoline fidelity family; 0-diff through f57k+ since ce522c4):
timeout 95 env PSXPORT_VK_HEADLESS=1 PSXPORT_SBS=1 PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=combat \
  PSXPORT_NOAUDIO=1 PSXPORT_SBS_NOPAUSE=1 ./scratch/bin/tomba2_port > sbs.log 2>&1   # run in background
timeout 300 env PSXPORT_VK_HEADLESS=1 PSXPORT_SBS=1 PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1 \
  PSXPORT_SBS_WATCH_CUT=1 PSXPORT_NOAUDIO=1 PSXPORT_SBS_NOPAUSE=1 ./scratch/bin/tomba2_port > sbs_cut.log 2>&1
grep -cE 'sbs-div|VIOLATION' sbs.log sbs_cut.log    # 0 in both -> git push ; nonzero -> HOLD, root-cause
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

## 7. Integration gotchas (hard-won 2026-07-08)
- **Reset to `origin/main` before EVERY cherry-pick.** A non-isolated agent can leak files/edits into the
  main checkout (see below); a dirty tree makes `git cherry-pick` abort ("commit your changes first").
  `git reset --hard origin/main` first guarantees a clean, known base each time.
- **NEVER `git commit --amend` as a fallthrough.** If a cherry-pick aborts and your script still runs
  `--amend`, it rewrites the CURRENT (already-pushed) tip → local diverges from origin, and the next push
  is blocked or (worse) force-territory. Only regen docs + amend AFTER a confirmed-successful pick, guarded
  by `git diff --quiet docs/code-map.md ||`.
- **Iterate the conflict-file list with `while IFS= read -r f; do … done`**, NOT `for f in $U` — a
  newline-joined `$U` mis-splits and passes both paths as one arg.
- **Conflict resolution by file type:** generated docs (`code-map.md`, `findings/INDEX.md`) → `checkout
  --theirs` then REGENERATE (`tools/codemap.py` / `tools/findings.py`); append docs (`engine_re.md`,
  `port-progress.md`) → union-merge (keep both sides, dedup). Wiring files (`cmake`, `boot.cpp`) →
  union-merge (additive).
- **Marker check must be precise:** grep `^(<<<<<<< |>>>>>>> |=======$)` — a bare `^=======` over-matches
  long `====…` section separators in docs and cries wolf.
- **Wide-RE (unwired) integration needs no full SBS gate** — the drafts are dead code — but run a SHORT
  `MODE=full` smoke anyway to prove a stray global ctor / cmake change didn't perturb 0-diff, then push.

## 8. Agent isolation is NOT guaranteed — verify it
Some `isolation:'worktree'` agents in the 2026-07-08 wide-RE wave were NOT truly isolated: one wrote
`quad_rtpt_submit.*` into the MAIN checkout and dirtied its `cmake`, and an operator `git reset --hard`
wiped that agent's uncommitted work. Defenses:
- The operator integrates from the agent's COMMITTED BRANCH SHA (which is intact), never from whatever
  leaked into the working tree. Relocate stray untracked files to `scratch/stray/` (mv, don't rm) and
  `git reset --hard origin/main` before integrating.
- Tell every source-editing agent EXPLICITLY: work only under your worktree cwd; NEVER write to
  `<HOME>/repo/psxport/...`; copy `generated/` + `scratch/bin/tomba2/MAIN.EXE` INTO your worktree,
  build in your own `build2`. Absolute-path writes are the leak vector.
- If the main checkout keeps re-dirtying after a reset, an agent is actively leaking — stop it or wait for
  it to finish before integrating.

## 9a. `PSXPORT_MIRROR_VERIFY=all` is a MANDATORY step of every wiring pass (2026-07-10)
Every wiring pass in §1 must, before declaring an address verified, run a normal (non-SBS) session
with `PSXPORT_MIRROR_VERIFY=all PSXPORT_MIRROR_VERIFY_CONTINUE=1` covering the content that reaches
the newly-wired address(es) — this is now the STANDARD mechanical detector for register/frame/store-
order bugs, catching them at the exact offending invocation instead of waiting for an SBS diff frames
(or a whole session) later. It is generalized: it needs no per-call-site `MV_CHECK`, so `=all` covers
every address wired via `engine_set_override_main`/`_a00` OR `EngineOverrides::register_` in one run
(see docs/config.md "Mirror TDD gate" for the mechanism + the `EVERY`/`CONTINUE` sampling knobs).
**As of the 2026-07-10 write-journal fast path, `EVERY=1` covers a normal multi-thousand-frame free-
roam soak in gate-window time** (measured: 3000 headless frames in ~9.5s, vs the old full-2MB-scan
path only reaching ~570 frames in 95s) — `EVERY>1` is now only needed if you specifically want to
trade coverage for even more speed, not to make a soak finish at all. `MIRROR_VERIFY_FULL=1` forces
the old full-scan path if you need to re-validate the journal after touching `strictCheck`/
`journalTrack`/`mem_w8`/`16`/`32` (game/core/verify_harness.{h,cpp}, runtime/recomp/mem.cpp). SBS-full
0-diff alone is NOT sufficient: SBS's `diff_mode` skips whole subsystems
(e.g. the per-vblank audio block, docs/findings/audio.md) and only compares whatever autonav actually
reaches — MIRROR_VERIFY checks the ABI contract (regs + RAM + scratchpad, no exemptions) on every
armed invocation regardless of whether SBS's autonav path exercises it. A real discovery run
(2026-07-10, unmodified `main`, `=all EVERY=50 CONTINUE=1`, ~3000 headless free-roam frames)
reproduced the ALREADY-TRACKED perobj_billboard/overlay_ground_gt3gt4 register-faithfulness /
stack-depth cluster (docs/findings/render.md "stack-depth OPEN") mechanically at the exact call —
confirming the tool finds REAL bugs, not harness artifacts — plus surfaced several previously-
untracked-by-address mismatches in the same render-dispatch chain (logged as OPEN, not fixed in that
pass; see docs/findings/render.md addendum). Treat a `=all` run that finds nothing on freshly-wired
content as real verification; one that finds something is a fix target like any other divergence —
record it (findings, OPEN if not fixed same-session) rather than waving it off.

## 9. Wide-RE drafts are UNTRUSTED until verified — budget a verify pass before wiring
The wide-RE tier's drafts are hand-transcriptions from the recompiled C and routinely contain MULTIPLE
bugs even when they compile and the drafting agent self-checked. Real 2026-07-08 counts when wiring banked
drafts: cube_text+actor_tomba = 2 bugs, render leaves = 4 bugs, and the two melee drafts = **9 bugs** (6 in
one function) — inverted branch/band polarities, swapped `ratan2`/`rsin`/`rcos` args, wrong source register,
a missing ABI-slot live value, an unmirrored stack frame, `&&`-vs-`||`. So:
- The wiring step is ALSO a mandatory line-by-line VERIFY step: diff the native method against its
  `gen_func_<addr>` / `ov_a00_gen_<addr>` in `generated/` (instruction-exact ground truth — Ghidra garbles
  GTE/COP2 and delay slots), checking every branch polarity, register lifetime, field offset, and guest
  frame + callee-saved spill. Fix all discrepancies before trusting the draft.
- **Run `tools/port_check.py <native.cpp>` as part of this verify step** (add an `// ORACLE:
  gen_func_<addr>` marker above the method if it isn't a `port_gen.py` draft already carrying a
  `// PORT_GEN:` marker). PASS/FAIL/UNPROVABLE per docs/port-framework.md — treat FAIL as a required
  fix, and manually cross-check any UNPROVABLE (usually an indirect/sibling-method call the tool
  can't resolve, or a loop-unrolling-boundary width-count artifact — see that doc's Validation §3 for
  a worked example of telling the two apart) before trusting the draft. This is a mechanical
  complement to the by-eye diff above, not a replacement for it.
- The SBS-full 0-diff gate catches these ONLY IF autonav exercises the leaf. Many AI/enemy leaves are NOT
  reached by intro-area autonav (no enemy encounter) — for those, the 0-diff gate proves "no regression to
  the frames reached," and correctness rests on the RE verification, not the gate. Say so honestly; a future
  session with enemy-area coverage should re-gate. Don't claim a leaf is verified because SBS was 0-diff if
  `ovhit` shows it never fired.
  - **`PSXPORT_SBS_AUTONAV=combat`** (2026-07-10, docs/findings/ai.md) closes this gap for the
    seaside `ActorMeleeEngage`/`MeleeProximity` melee cluster specifically — an OPT-IN extension of
    the standard gate's Nav script that walks Tomba into the first melee encounter after reaching
    control. It is NOT part of the standard gate command below (stays off by default — see the next
    bullet for why) but is the tool to reach for when a combat/AI leaf's `ovhit` reads 0 under the
    standard command. It found (and left OPEN, not fixed) a real pre-existing SBS divergence tied to
    the already-tracked "stack-depth OPEN" register-faithfulness cluster (docs/findings/render.md) —
    a red gate from this leg is that cluster, not a regression from whatever you just wired.
- **Run `tools/abi_extract.py <addr> --contract` FIRST at draft time, and again at wiring time.** The
  "unmirrored stack frame" / "missing ABI-slot live value" bug classes above are exactly what it's for:
  it parses the `gen_func_<addr>`/`ov_<area>_gen_<addr>` body straight out of `generated/` and reports the
  frame size, every prologue spill/epilogue restore offset, every scratch sp-relative store in program
  order, and every call site's `c->r[31]` return-address constant plus which callee-saved registers
  (r16..r23/r30) look live going into it — the exact fields a hand-transcription drops. `--scaffold` emits
  a ready-to-paste Frame RAII struct + call-site block in the house style. It is NOT a substitute for the
  line-by-line verify above (no dataflow/CFG — a purely linear scan, so cross-branch register liveness is
  a heuristic), but it removes the bulk of the boilerplate-transcription error surface before that pass
  starts. See `docs/abi-extract.md`.

# abcompare — reliable ./run.sh-vs-oracle comparison (design, 2026-07-10)

**Problem (USER, 2026-07-10):** we have no reliable compare tool. `PSXPORT_SBS_MODE=full` compares
pc_faithful-vs-recomp inside ONE process: its core A forces `mPcSkip=false` (NOT the user's config),
its present path is the SBS composite (neither pane looks like `./run.sh` nor like the oracle), and
pc_skip-only bugs (the missing throw chain) pass every gate. The user had to screenshot the two
configs separately because compare mode "shows a whole another thing".

**Requirement:** side A = EXACTLY `./run.sh` (pc_skip + pc_render, normal present). Side B = EXACTLY
the oracle (`PSXPORT_ORACLE=1`, pure recomp + PSX render, normal present). No in-process coupling.

## Shape: two separate processes + deterministic replay + offline compare

1. **Input**: one REPL command script (or a recorded `pad_session.pad` — padrec already exists) fed
   identically to both runs. The port is deterministic given identical input from boot.
2. **Run A**: `PSXPORT_VK_HEADLESS=1 PSXPORT_REPL=1 ./tomba2_port < script` — the true default config.
   **Run B**: same with `PSXPORT_ORACLE=1`. Nothing else differs. Each run uses plain `shot`/`preseq`
   captures and `dumpram` at sample points — the NORMAL paths, zero harness contamination.
3. **Alignment**: pc_skip leads the faithful/oracle path by a boot-collapse offset (~12 frames) plus
   per-fork drift (beats gate on conditions). Align on OBSERVABLE MILESTONES, not frame numbers:
   both runs log stage transitions (`[native_boot] frame N: stage=...`), cutscene up, autoskip
   gameplay-start, area loads. The compare script pairs samples by (milestone, offset-within-
   milestone). The skip-compare rendezvous doctrine (feedback memory 2026-07-10) applies: at every
   skip fork the A side must rendezvous; milestone alignment implements that externally.
4. **Compare** (`tools/abcompare.py` orchestrates all of the above + reports):
   - **pixels**: per-pair image diff (mean abs + bbox of the largest differing region + side-by-side
     PNG for the user), tolerant only of the documented render-style deltas the caller opts into.
   - **state**: per-pair `dumpram` diff with the pc_skip mask doctrine (two-compare-modes memory:
     shortcuts allowed → CD-scratch/stack-scratch masked; shared/consumable state must match),
     reported as spans with owner labels (reuse sbs addrLabel taxonomy).
   - output: first-divergence milestone + evidence gallery under `scratch/abcompare/<runid>/`.
5. **Gate use**: this becomes the missing **user-config leg** of the push gate (the standing
   user-config golden-gate directive): run a scripted field walk + throw + cutscene sample on every
   push; a NEW pixel/state divergence vs the recorded golden = HOLD.

## Non-goals
- Not a replacement for SBS-full (which stays the byte-exact pc_faithful-vs-recomp harness).
- No in-process dual-core mode; the whole point is process isolation.

## Status (implemented 2026-07-10)
- [x] `tools/abcompare.py` — orchestrates 2 isolated headless runs from one probe script.
  Side A = `PSXPORT_PC_SKIP=0` (pc_faithful + pc_render), side B = `PSXPORT_ORACLE=1`.
  **Alignment simplification vs the sketch above:** the compare runs A as pc_FAITHFUL, not
  pc_skip — pc_faithful is byte-exact to recomp per frame (the Job#1 invariant), so the two
  processes are frame-locked by construction and NO milestone pairing / mask is needed: every
  probe demands strict RAM+spad 0-diff, and any surviving pixel diff is a pure RENDERER bug.
  (A pc_skip-leg compare with milestone alignment remains future work if ever needed; the
  in-process MODE=skip rendezvous harness covers that axis.)
- [x] probe script: `tools/golden_drive.repl` — plain REPL lines + `probe <name>` (expands to
  `dumpram` + `shot` per side). Mods pinned factory-neutral via PSXPORT_SETTINGS→nonexistent.
- [x] pixel diff: ±tol/channel AND ±1px spatial tolerance (3×3 neighborhood, both directions)
  so float-vs-fixed edge jitter is absorbed but a missing prim/character still flags; evidence
  composite (A | B | diff-mask) per divergent probe under `scratch/abcompare/<runid>/diff/`.
- [x] strict RAM + scratchpad diff per probe (no mask — pc_faithful mode needs none).
- [ ] golden-run capture + push-gate wiring (docs/fleet-workflow.md §2 leg 3).

Usage: `tools/abcompare.py [--script tools/golden_drive.repl] [--no-build] [disc.chd]`
(exit 0 clean / 1 pixel divergence / 2 state divergence / 3 harness error; full run ≈ seconds,
both sides run in parallel headless).

First catches (2026-07-10, golden drive): f160 prologue cutscene — oracle renders Tomba AND
Zippo, pc_render draws NEITHER character; f1500 field — a ground prim missing. Both with RAM
byte-exact, i.e. confirmed renderer-boundary bugs (same class as the throw-chain finding).

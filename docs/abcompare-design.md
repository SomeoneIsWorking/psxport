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

## Status
- [ ] tools/abcompare.py skeleton (orchestrate 2 runs from one script, collect shots/logs)
- [ ] milestone parser + pairing
- [ ] pixel diff + gallery
- [ ] masked RAM diff
- [ ] golden-run capture + gate wiring (docs/fleet-workflow.md §2 leg 3)

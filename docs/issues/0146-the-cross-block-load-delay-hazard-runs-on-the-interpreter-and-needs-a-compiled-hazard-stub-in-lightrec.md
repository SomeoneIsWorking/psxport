---
id: 146
title: The cross-block load-delay hazard still runs on the interpreter; Lightrec needs a compiled hazard stub (design, not implemented)
symptom: Toy Story 2 takes 1,267 `load_delay_hazard` fallbacks per 1000-pad-frame route, each interpreting up to 3 guest instructions, and psxport exempts them from the fallback block limit (e5888e22)
state_items: S002
tags: lightrec,load-delay,fallback,interpreter,design,toystory2
created: 2026-10-01
updated: 2026-10-01
---

## Answer

**Status: design only. Nothing was shipped for this defect, because the change is a new block kind inside
the recompiler (compile path, dispatcher, lifetime and invalidation), and a partial version would put
unverified codegen under every title.** The exemption in `fallbackAdmission` and the `docs/config.md`
paragraph stay until the stub below lands.

## What happens today (verified in Lightrec `emitter.c`, `lightrec.c`, `generate_dispatcher`)

A branch whose delay-slot load targets `R` jumps to `ds_check_func`, which calls
`lightrec_check_load_delay(state, pc, R)`. If the first opcode at the target does not read `R`, it stores
`gpr[R] = temp_reg` and the dispatcher goes to `loop2`. Otherwise it calls
`lightrec_begin_fallback(LOAD_DELAY_HAZARD)` and `lightrec_handle_load_delay` interprets at most three
instructions, then resumes at `loop2`.

Interpreter semantics to reproduce exactly:

- op0 not a branch: it runs with the OLD `R`; `R` is committed afterwards unless op0 itself writes `R`.
- op0 a branch: the branch is evaluated, `R` is committed, the link register is written (the link wins
  over the load), then the delay slot runs.

## Design: a hazard stub

A tiny compiled block per `(pc, R)`, built from the guest words at `pc`:

1. Body is op0 (non-branch) or branch plus its delay slot. Loads and stores in it carry
   `LIGHTREC_IO_MODE(LIGHTREC_IO_HW)` so they take the generic callback and need no block lookup of
   their own.
2. The commit `gpr[R] = temp_reg` is emitted through the regcache (`lightrec_alloc_reg_in(REG_TEMP)`,
   `lightrec_alloc_reg_out(R)`): after op0 for a non-branch; inside `lightrec_rec_delay_slot()` before
   the delay slot for a branch (covers both delay-slot sites of a conditional branch). Skipped when op0
   writes `R`, including a link write.
3. A non-branch stub ends with an EOB to `pc + 4`, which needs a non-static wrapper around
   `lightrec_emit_eob`.
4. New `struct block` field `hazard_reg` and flag `BLOCK_HAZARD_STUB`. `lightrec_compile_block` skips
   `lut_write` and the interior-target reaping for a stub, so a stub is never reachable through the code
   LUT.
5. The stub cache is owned by the blockcache so `lightrec_free_all_blocks` frees it (it runs on destroy,
   `lightrec_set_store_observer` and `lightrec_set_cycles_per_opcode`).
6. SMC safety without invalidation hooks: on every use compare the stored guest words with memory and
   rebuild on mismatch.
7. Dispatcher: after `check_load_delay`, load `state->hazard_entry`; if non-null, clear it, set
   `curr_pc`, and jump to the `loop` label with `V1` = stub function; otherwise continue at `loop2`.
8. A stub that cannot be built (a branch in a branch's delay slot, or a compile failure) falls back with
   `JIT_COMPILE_FAILURE` / `UNSUPPORTED_CONTROL_FLOW`; the psxport exemption would then no longer cover
   that case and the fallback limit applies.
9. `tools/check_runtime_contract.py` requires the marker string
   `lightrec_begin_fallback(state, LIGHTREC_FALLBACK_LOAD_DELAY_HAZARD` and must be updated with the
   change.

Required tests (Lightrec `runtime_contract_test.c`, plus psxport): non-branch op0; branch op0
conditional and unconditional, taken and untaken; `jalr` link conflict with `R`; op0 writing `R`; op0 a
load and a store; SMC at the target; all with ZERO fallback. In psxport,
`test_cross_block_load_delay_hazards` in `tests/test_fallback_hazard_budget.cpp` must keep asserting
correct results and `loadDelayHazard == 0` with zero fallback, and the exemption plus the `docs/config.md`
paragraph are deleted in the same change.

## A second, existing defect found while reading this path

On the hazard edge the boundary callback is NOT invoked for the target pc (the stub/interpreter path
resumes at `loop2` after the commit, but the check runs before the boundary). A title override at a
function entry reached as `jal f; lw a0, ...` is skipped. Not measured in a real title; the stub's
dispatcher change should route through the boundary for `pc`.

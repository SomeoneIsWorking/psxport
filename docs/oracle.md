# The ORACLE — in-process interpreter + software-GPU second Core, lockstep divergence diff

**Directive (USER, 2026-07-01, firm).** We need a real oracle for INTERLEAVED DIVERGENCE COMPARE — find
VRAM diffs, render diffs, RAM/state diffs in lockstep with the native port and pin the FIRST divergence.
Screenshots-to-eyeball are allowed but NOT the goal. The cutscene render bugs (double fade / void-effect /
cliff water) proved un-fixable by guessing; without a trustworthy PSX reference we are blind.

**Shape (what the user asked for).** A PURE INTERPRETER-driven SECOND `Core` (our Core class), with a
SOFTWARE/CPU rasterizer, run INTERLEAVED (lockstep) with the native port Core. NOT a separate-process
emulator, NOT screenshots-only.

**Why interpreter, not the existing SBS "PSX" core.** `sbs.cpp` (PSXPORT_SBS_MODE=both) already runs two
Cores in lockstep with a per-frame RAM diff and pause-at-first-divergence — but its B core runs the RECOMP
SUBSTRATE (`psx_fallback`), which FREEZES/aborts in the intro cutscene (recomp-coroutine limit later-272;
recomp-MISS 0x80146478 on the un-recompiled overlay code). A PURE INTERPRETER executes whatever MIPS is in
RAM — including the overlay cutscene code the recompiler has no entry for — so it neither freezes nor misses.
That is the whole point: the oracle must REACH and RUN the scenes the recomp can't.

**Why NOT wide60rt / external emulator.** wide60rt (the deleted Beetle standalone, runtime/main.cpp) was
faulty, and a separate process emitting screenshots can't do byte-level interleaved divergence diffing
against our Core's RAM/VRAM. The oracle must share our Core memory model so the diff is direct.

This SUPERSEDES the "oracle is GONE, never recreate it" rule (memory oracle-harness-removed) — the user is
reinstating an oracle, specifically this in-process divergence-diff form. It is DIAGNOSTIC, not shipping
behavior (like sbs.cpp / dualcore.cpp): one PC-native game still ships; this is a debugger.

## Building blocks (all in-tree)
- **Interpreter:** `runtime/recomp/interp.cpp` (662 lines), removed in commit **6e3951d** ("Drop the
  interpreter"). Self-contained MIPS R3000 flat interpreter operating on `Core` memory (`c->mem_*`, `c->r[]`).
  `is_bios` + `exec_simple` + `coro_next_pc` + `ldhaz_step` + `g_callring` + `ifn_record` are internal to it;
  externals it needs (`gte_op`, `CORO_SENTINEL`, `platform_hle_lookup`, `cfg_str`, `rec_func_index`,
  `rec_dispatch_miss`) all still exist. Recover from `git show 6e3951d^:runtime/recomp/interp.cpp`.
- **Software GPU:** Beetle's vendored-but-uncompiled rasterizer `vendor/beetle-psx/mednafen/psx/gpu.c` +
  `gpu_polygon.c` / `gpu_sprite.c` / `gpu_line.c` (full GP0 processor + VRAM). Compile for the oracle Core;
  route the oracle's GP0 stream into it → an oracle VRAM image to diff vs the native VRAM.
- **Lockstep + diff infra (EXISTS):** `runtime/recomp/sbs.cpp` — two Cores lockstep, identical input,
  per-frame RAM+scratchpad diff, pause-at-first-divergence, debug-server `sbs diff`/`bt`/`watch`/`show`/`step`.
- **BIOS:** `bios/scph5501.bin`, `bios/openbios-fast.bin`. GTE/SPU/MDEC already compiled from Beetle.

## STATE-SYNC, not frame-sync (USER, 2026-07-01 — critical)
A pure-PSX interpreter core CANNOT be frame-lockstepped against the native port: the PSX side spends real
frames on CD LOADS / CD waits / VSync settles that the native side does SYNCHRONOUSLY (instantly). So frame
N on the native core is a different point in the game's progression than frame N on the PSX core — a naive
per-frame RAM diff (what sbs.cpp does today) compares unrelated states and is meaningless across
native-vs-true-PSX. The native core reaches a state FIRST; we must FREEZE it there until the PSX core catches
up. (Same principle as memory drive-by-game-state-not-frames / dual-core-state-synced-diff /
sbs-native-vs-psx-gameplay-cores "sync at the gameplay-start flag", now generalized to a checkpoint sequence.)

**The diff is gated on GAME STATE, not frame number.** This needs the harness to OWN the game's state machine
enough to detect "core X reached checkpoint Y" — which we do (stage id 0x801fe00c, sm[0x48]/[0x4a]/[0x4e],
the gameplay-start flag *(0x1f800137), the SOP scene byte 0x800bf9b4, the narration scroller, …).

**Barrier model:**
1. An ordered checkpoint list C0 < C1 < … — each a predicate on guest state (e.g. "stage==GAME 0x8010637C",
   "gameplay flag *(0x1f800137)==1", "SOP scene byte 0x800bf9b4 advanced to k", "narration beat n reached").
2. Run each core forward — feeding the needed input (e.g. mash Start) — until it reaches the NEXT checkpoint.
3. BARRIER: the core that arrives first PARKS (stops stepping) until the other arrives at the same checkpoint.
   (Typically native parks and the interpreter PSX core grinds its real CD loads to catch up.)
4. At the aligned checkpoint, DIFF only the COMPARABLE state — game-logic RAM + scene state — EXCLUDING the
   fields that legitimately differ (CD/load progress, timing/root-counter, frame counters, render-only scratch).
5. Release both to run to the next checkpoint. Between checkpoints the cores run a DIFFERENT number of frames;
   only checkpoint-aligned state is diffed.

Implication: input automation is PER-CORE and checkpoint-driven (each core gets Start until IT passes the
title checkpoint), not a single mirrored host pad. The existing sbs.cpp "mirror the same host pad to both
cores each frame" must become "drive each core toward its next checkpoint independently."

## Integration design
- Per-Core flag `use_interp` (Game/Core). When set, the four engine entry points route to the interpreter
  instead of the substrate:
  - `rec_dispatch` (overlay_router.cpp), `rec_coro_run` / `rec_interp` / `rec_super_call` (dispatch.cpp):
    `if (c->use_interp) interp_*(c, addr); else <substrate>`.
- interp.cpp's public entries renamed to `interp_coro_run` / `interp_run` so they DON'T collide with the
  substrate shims of the same `rec_*` names.
- `coro_native_call` (inside interp.cpp) gets an ORACLE gate: keep `is_bios` (→ rec_dispatch_miss HLE) and
  `platform_hle_lookup` (hardware sync — VSync/CD/GPU/MDEC), but SKIP the `rec_func_index>=0 → rec_dispatch`
  recomp-body routing (that was a hybrid optimization). Result: pure interpretation of game+overlay code with
  only BIOS + hardware HLE native. Crucially it must NOT call `rec_dispatch` in oracle mode (would re-enter
  the interpreter via the gated entry → recursion).
- The oracle core takes NO native game-overrides (it is the PSX ground truth) — only BIOS + hardware leaves.

## Phases
1. **Interpreter oracle (RAM/state divergence).** Restore interp.cpp adapted to per-Core; add `use_interp` +
   routing; wire it as a new `PSXPORT_SBS_MODE=oracle` core (B = interpreter PSX). Verify the B core reaches +
   runs the cutscene WITHOUT freezing. **Build the STATE-SYNC BARRIER** (above) — checkpoint list + per-core
   checkpoint-driven input + park-the-faster-core + checkpoint-aligned diff — this is the heart of Phase 1,
   NOT the existing frame-lockstep. Deliverable: trustworthy PSX guest state at each checkpoint — reveals what
   the native side gets wrong (e.g. the void-scene "effect" state we're missing; confirm/deny bug-1's fade).
2. **Software rasterizer (VRAM/render divergence).** Compile Beetle soft-GPU for the oracle Core; route its
   GP0 to it; produce oracle VRAM. Add VRAM/render diff + an oracle present pane (see the real PSX cutscene
   render, diff it against native). Deliverable: the render diff that fixes void-effect + cliff water.

## Status
- 2026-07-01: scoped; this doc + memory written. Bug-1 (double fade-in) fixed natively (later-277, committed).
  Bug-2 (void draws sea) reverted — pure-black was wrong, there is supposed to be an EFFECT; needs the oracle.
  Phase 1 implementation starting.
- 2026-07-01 (later-279): **Phase-1 RAM diff EXHAUSTED — narration state is convergent.** oraclediff with the
  guest CdSearchFile CD-cache (0x80102xxx) excluded shows only benign drift (PRNG LCG 0x80105EE8, a 0..15
  callback-ring counter 0x80105BAC, the stdio line buffer 0x80105EF8). The native side runs the cutscene
  LOGIC correctly; the void-effect / cliff-water bugs are RENDER-only. → Phase 2 (software GPU) is now the
  critical path. (docs/findings/sbs.md "oraclediff: narration GAME STATE is convergent".)

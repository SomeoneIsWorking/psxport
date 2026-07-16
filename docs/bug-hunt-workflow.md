# Bug-hunt workflow (the loop)

The canonical loop for finding and fixing bugs in the port. Read this before chasing any symptom.
This is a DURABLE DIRECTIVE — if it falls short, fix it in place.

## The configuration matrix

Any run is one **PC SKIP** setting × one **RENDERER**. Bugs can live in any cell:

| PC SKIP | RENDERER PC | RENDERER PSX |
|---------|-------------|--------------|
| ON      | ?           | ?            |
| OFF     | ?           | ?            |

- **PC SKIP ON**  = `mPcSkip=true`, the shortcut path (`./run.sh` default). Multi-step inits collapse
  to their end-state.
- **PC SKIP OFF** = `mPcSkip=false`, the faithful multi-step path (what SBS full exercises).
- **RENDERER PC** = the native renderer (default).
- **RENDERER PSX** = `PSXPORT_RENDER_PSX=1`, the substrate GTE/OT/GP0 renderer.

Hunt across ALL four cells. **Start where the bugs currently are** (right now: PC SKIP ON has the
output-difference bugs on its own) — but revisit the others; a "fixed" bug in one cell can hide in
another.

## Bugs are found via oracle compare

The oracle = `recomp_path` (`PSXPORT_GATE=1`, the pure substrate). Find bugs by comparing a
configuration against it, never by eyeballing a single run:

- **PC SKIP OFF cell** → `PSXPORT_SBS_MODE=full` (core A = pc_faithful, core B = recomp_path; both
  `mPcSkip=false`; byte-compares guest RAM every lockstep frame). This is the honest gate AS LONG AS
  the oracle is clean (see "Oracle integrity" below).
- **PC SKIP ON cell** → `PSXPORT_SBS_MODE=skip` (core A = pc_skip=true / the real `./run.sh`
  shortcut config, core B = the pure recomp oracle). **Frame-aligned as of 2026-07-10** (docs/
  findings/sbs.md "SKIP-mode frame alignment"): every collapsed-multi-step fork is meant to call
  `Sbs::skipRendezvousReached` (sbs.h) so A idles rather than racing ahead of B at the same lockstep
  frame — only ONE fork is wired so far (`Engine::stage0AdvanceSkip`'s START.BIN-load gate); the
  compare itself is now strict per-frame (no more 60-frame settle tolerance) plus an auto-armed
  per-frame picture pixel-diff. A fork that isn't rendezvous-gated yet can still legitimately drift
  and will show up as a real divergence — check the fork inventory in docs/findings/sbs.md before
  assuming a PC-SKIP-ON divergence is a genuine logic bug vs "this fork's rendezvous isn't wired
  yet".
- **RENDERER cells** → `PSXPORT_SBS_MODE=render` (A = pc_render, B = psx_render, both pc_faithful);
  pc_render must leave guest RAM identical to psx_render.

A divergence is a bug. Every divergence is fatal — no allowlist, no "known diff".

## FOUND A BUG? DO NOT JUMP TO FIXING IT.

This is the rule that saves the most time. Before debugging anything:

1. **Is the behavior that produces the divergence FULLY OWNED natively, end to end?** I.e. is every
   function in the divergent call chain a native C++ method (not still running as `gen_func_*`
   substrate)?
2. **NO → grow ownership first.** Keep porting functions along that call chain until the whole path
   is native. A divergence rooted partly in substrate is often an artifact of incomplete ownership
   (a missing native write, a state the substrate assumes), not a logic bug you can isolate. Owning
   the path end-to-end is the only way to be sure the residual divergence is real.
3. **YES → then debug and fix it.** Only now is a divergence a genuine native-vs-substrate logic bug.

**Never debug code that isn't fully owned.** If you catch yourself reasoning about a `gen_func_*`
write-site in a still-substrate caller, stop — port the caller first.

## Oracle integrity (the precondition for everything above)

The oracle (SBS core B) must run ONLY the pure substrate for engine/game code. Legitimately on the
oracle:

- **PlatformHle** — async→sync conversions (VSync/CdSync/MDEC waits → native non-stall) + HLE BIOS.
  These MUST fire on both cores or the no-IRQ runtime hangs.
- The pure recompiled body (`gen_func_*` / `ov_*_gen_*`) for everything else.

Nothing else. If an engine/game native override leaks onto the oracle, SBS compares native-vs-native
and reports a **false 0-div** — masking every real bug in that cluster (the 2026-07-09
`perobj_billboard`/`overlay_gt3gt4` regression hid 19 real packet-pool divergences behind a clean
gate for a day).

ONE override registry, one gate point (the `g_<mod>_override[]` tables are process-global, shared by
both SBS cores):

- **`overrides::install(addr, name, native, gen[, setter])`** (`runtime/recomp/override_registry.h`)
  is the ONLY way to wire an engine/game native. It records `{ native, gen }` and installs ONE shared
  oracle-gated thunk. Both call paths reach it: the `g_<mod>_override[]` thunk (direct `func_X(c)`
  calls) and `rec_dispatch` (`overrides::dispatch`, `overlay_router.cpp`). The single dispatch runs the
  `gen` body on the oracle leg (core B / `psx_fallback` / `verify.inSubstrateLeg`) and `native`
  elsewhere — the gate is in ONE place, it can't be forgotten per-cluster.
- Do NOT add a per-trampoline `psx_fallback` guard and do NOT call the raw `shard_set_override` /
  `ov_<tag>_set_override` for an engine/game native — that reintroduces the fake-0-diff-on-core-B bug.
  Pass the module setter as `install`'s `setter` arg to intercept direct callers, or `nullptr` for
  rec_dispatch-only. PlatformHle + native scheduler BIOS leaves use the RAW installers directly (they
  must fire on both cores).

If SBS suddenly shows 0-div where it used to diverge, SUSPECT THE ORACLE first (a new cluster
installed ungated), not a miraculous fix.

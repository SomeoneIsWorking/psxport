# Autonomous campaign checklist

Standing plan for the render-native + code-structure + oracle-driven bug-hunt loop (USER 2026-07-15:
"you have your directives and you have your oracle compare, you shouldn't need me for anything").
The oracle (SBS-full 0-diff + MIRROR_VERIFY byte-compare vs substrate) is the gate — no user review
needed per commit. Work top-to-bottom; each item lands committed+pushed with its cited gate.

## Phase 1 — double-ownership dedup  ✅ DONE
Two natives implementing one guest address = duplication (or a mis-RE). `codemap.py --conflicts` finds
them; consolidate to one owner, gate byte-exact.
- [x] FUN_80040B48 — SceneEvents::armBody sole owner (was dup'd by CubeTextLedger::activateSlot). MV OK, SBS 0-diff.
- [x] FUN_80040CDC — ScriptInterp::init sole owner (was dup'd by Engine::animEnvInit, mis-named). SBS 0-diff.
- [x] FUN_800518FC — NodeXform::buildWithOffset sole owner (was dup'd by Engine::objMatrixCompose). MV 23937 passes.
- [x] codemap.py: live-registration parse + `--conflicts` + authoritative filter (catches the class).
- Remaining --conflicts: 0x80044BD4 (intentional bd4 family), 0x8002AB5C (terrain display-pass FP). Both non-bugs.

## Phase 2 — code structure (reads like a PC game)  ◐ ONGOING
Byte-exact refactors of heavy transcribed files → typed lenses, named constants, guest_abi.h vocab.
Only EXERCISED functions; SBS-full is the gate. (node_xform, sequencer, actor_tomba, font, engine.cpp ×2 done.)
- [ ] Continue the register-literal-dense files (see `codemap`/grep for raw hex-poke density).

## Phase 3 — fallthrough-for-already-native  ☐ NEXT
A native exists but the guest address is NOT override-registered, so rec_dispatch/guest_leaf callers
run the EMULATED substrate while direct callers run the port (a split). Register + MIRROR_VERIFY gate.
- [ ] `codemap.py --substrate-fallthrough` — precise detector (dispatch-target only, not any addr on line).
- [ ] Engine::animTick (0x8004190C) — dispatched from combat behaviors → substrate. Register + MV.
- [ ] Engine::walkStart (0x80054D14) — same. (Watch the early-exit-before-frame path.)
- [ ] Triage the rest of the detector output; native-ize the real leaf cases, skip boot-dispatched FPs.

## Phase 4 — SBS bug-hunt over the replay library  ☐
Run SBS-full over every replay; every divergence is a Job#1 bug → root-cause + fix. (Currently all
replays 0-diff to deep frames — see docs/findings/sbs.md "Job #1 replay sweep".)
- [ ] Re-run after each phase-3 native-ization (new native paths = new divergence surface).

## Phase 5 — extend exploration/driving to surface NEW divergences  ☐
When SBS finds nothing on existing replays, the frontier is COVERAGE: reach scenes the replays don't.
Build the capability to drive into new territory headlessly (USER: "extend the debugging to move around
more freely, test different scenes, try to trigger a dialogue").
- [ ] Audit current driving tools (REPL press/tap, PSXPORT_AUTO_SKIP, replays/) for gaps.
- [ ] Add scripted free-movement / scene-warp / dialogue-trigger drivers (deterministic, headless).
- [ ] Run SBS-full across the new scenarios; root-cause any divergence.

## Working rules
- Oracle IS the gate: MIRROR_VERIFY=<addr> (byte-compare vs substrate) for a wired override; SBS-full
  0-diff for the faithful path. Cite the pass count / frame reached in every done-claim.
- Register overrides via EngineOverrides + psx_fallback-gated shard_set_override (core B stays pure).
- `codemap.py --conflicts` clean after every phase-1/3 change (no new dual-ownership).
- Commit + push each landed unit; keep this checklist updated in the same commit.

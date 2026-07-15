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
- [x] `codemap.py --substrate-fallthrough` — precise detector (dispatch-target only). 97 authoritative candidates.
- [x] Engine::animTick (0x8004190C) — MV 27969 passes, 0-diff. Native-ized (RegisterEngineAnimLeafOverrides).
- [x] Engine::walkStart (0x80054D14) — MV 1 pass, 0-diff. Native-ized. (Thin coverage — early-exit is write-free.)
- [ ] Triage the rest. KEY FINDING: most of the 97 dispatch from behaviors in scenes the 3 replays never
      reach (e.g. NodeXform::build fires in NONE of dark-screen/hut-entry — its callers are seaside/
      visibility-gate). Those are BLOCKED on Phase 5 coverage — can't MV-gate without the scene. Only
      combat/movement-path fallthroughs (like animTick/walkStart) are gate-able now. Do those; defer rest.
- [x] Trig::rsin (0x80083E80) + Trig::ratan2 (0x80085690) — MV 102593/79617 passes, 0-diff. Native-ized
      (Trig::registerOverrides). Table-based (read guest SIN_TAB/ATAN_TAB), byte-exact to substrate GTE leaves.
- [ ] Trig::angleCmp (0x80077768) — DEFERRED: cutscene-camera caller, not exercised by current replays.
- Skip: Sequencer channel* leaves (0x80090E40/91050/91910/92080) — intentionally-unwired never-fire leaves.
- Phase-3 gate-able-now progress: buildWithOffset ✅ animTick ✅ walkStart ✅ rsin ✅ ratan2 ✅.
  Remaining candidates are mostly scene-gated (need Phase 5) or deferred (angleCmp, build → cutscene/seaside).

## Phase 4 — SBS bug-hunt over the replay library  ☐
Run SBS-full over every replay; every divergence is a Job#1 bug → root-cause + fix. (Currently all
replays 0-diff to deep frames — see docs/findings/sbs.md "Job #1 replay sweep".)
- [ ] Re-run after each phase-3 native-ization (new native paths = new divergence surface).

## Phase 5 — extend exploration/driving to surface NEW divergences  ☐
When SBS finds nothing on existing replays, the frontier is COVERAGE: reach scenes the replays don't.
Build the capability to drive into new territory headlessly (USER: "extend the debugging to move around
more freely, test different scenes, try to trigger a dialogue").
- [x] Audit driving tools: SBS_AUTONAV=1 (→ player control @f246) + SBS_KEYS="FROM-TO:BTN,..." (timed
      input, both cores) IS the scripted-movement tool — no new mechanism needed. Movement CONFIRMED
      (Tomba X 0x800E7EAE moved 0x0F64→0x1770 holding right). Recipe in docs/findings/sbs.md "Phase 5".
- [x] First exploration: blind 4-direction walk of START field under SBS-full = 0-diff (byte-exact,
      no divergence there; stayed in start field — blind walk hits walls).
- [x] ⭐ DRIVING SOLVED (USER "figure out how to drive the game"): tools/pad_decode.py (decode replay →
      button timeline / SBS_KEYS, or build .pad from spec) + position-feedback method. Movement calibrated
      (left/right=±X ~22/f @0x800E7EAE; up/down=±Z near doors @0x800E7EB6). VERIFIED: drove into the hut via
      own scripted input (right34→up180 → sm[0x4c]=3), SBS-full 0-diff f9480. docs/driving-the-game.md.
- [ ] APPLY the method to reach a GENUINELY new area (not the hut): find the start-field MAIN exit (walk
      to each edge/door, probe X/Z for a trigger zone that latches sm[0x4c]/area-load), script it, SBS-full.
- [ ] For each new area: root-cause any divergence (Phase 4) + MV the scene-gated fallthroughs that fire.

## Working rules
- Oracle IS the gate: MIRROR_VERIFY=<addr> (byte-compare vs substrate) for a wired override; SBS-full
  0-diff for the faithful path. Cite the pass count / frame reached in every done-claim.
- Register overrides via EngineOverrides + psx_fallback-gated shard_set_override (core B stays pure).
- `codemap.py --conflicts` clean after every phase-1/3 change (no new dual-ownership).
- Commit + push each landed unit; keep this checklist updated in the same commit.

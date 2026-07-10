# Findings — tooling / debug server / harness

## ovhit A/B mismatch is often a call-GRAPH asymmetry, not a counting bug
- **symptom:** the first honest `PSXPORT_DEBUG=ovhit` SBS-full run (post the 2026-07-10 target-
  binding + g_tab-merge fix) surfaced ~15 addresses with A/B count mismatches, some large:
  `0x8003CDD8 (Render::cmdListDispatch) native=0 oracle=37527`, `0x8003C8F4 (billboardEmit)
  native=0 oracle=22689`, `0x8013FB88/0x8013FE58 (gt3/gt4) native=0 oracle=377233`,
  `ActorTomba::frameTick A=2878 B(gen)=0`, `ActorTomba::turnBiasCompute/outerTransitionGate/
  outerTransitionCommit` similarly A-only, `Animation::advanceLinkChain A=5719 B(gen)=2841`
  (~2x, not exact), plus small (0-5 count) mismatches on PcScheduler's 5 primitives,
  `GraphicsBind::recordArrayInit`, `CubeTextLedger::spawnPopup`.
- **status:** RESOLVED — every one triaged is a measurement ARTIFACT, not a functional defect.
  0-diff SBS-full confirmed per-frame (not just the 30-frame print granularity — `Sbs::Impl::run`
  calls `stepCore(A)`, `stepCore(B)`, `checkDivergence()` every single frame) through f3000, before
  and after this triage pass.
- **cause — two distinct artifact shapes, both already-known blind spots resurfacing through the
  NEW ovhit counters:**
  1. **Direct native-to-native call, mirroring the guest's own direct intra-shard call.** A ported
     method calling its own already-ported sibling directly — `perobj_billboard.cpp`'s
     `c->mRender->cmdListDispatch()` (5 sites) / `c->mRender->billboardEmit()`,
     `outerTransitionCommit()`'s unconditional first-statement call to `outerTransitionGate()`,
     `OverlayGroundGt3Gt4::entityLoop`'s `gt3(c)`/`gt4(c)` calls — bypasses BOTH ovhit counters on
     the NATIVE side (never touches `rec_dispatch`, never touches the `g_override[]` wrapper since
     it's a direct C++ call), while the SUBSTRATE side's IDENTICAL direct intra-shard call DOES hit
     the g_tab wrapper (`shard_set_override`/`engine_set_override_main` installs at the wrapper,
     which intercepts regardless of caller) — giving the `native=0, oracle=N` shape. PROVEN, not
     just plausible: `billboardEmit`'s oracle count (22689) equals `billboardCompose1`(17007) +
     `billboardCompose2`(5682) EXACTLY (its only two native callers); `outerTransitionGate`'s oracle
     count (2842) equals `outerTransitionCommit`'s own count (2842) EXACTLY (read the source:
     `if (outerTransitionGate()) goto done;` is literally outerTransitionCommit's first statement,
     game/player/actor_tomba.cpp:1004). This is the SAME root cause already documented in
     docs/findings/render.md "dead end avoided" (the `0x8003CCA4 → 0x8003CDD8 → 0x8003F698` chain)
     — that finding predates `ovhit`; this is it resurfacing through a different counter.
  2. **The two SBS cores reach a shared leaf via structurally different call GRAPHS.** Core A
     (pc_faithful) runs the ported native `Engine` per-frame driver (`frameStartTickFaithful`,
     `sceneEventFifoFaithful`, …), which explicitly `rec_dispatch`es to wired leaves. Core B
     (recomp_path/oracle) reaches the SAME final RAM state via the PURE SUBSTRATE per-frame chain
     instead — a topologically different call graph. PROVEN via a runtime probe, not static
     reading: `sefprobe` (game/core/engine.cpp, gated `PSXPORT_DEBUG=sefprobe`) printed on every
     entry to `Engine::sceneEventFifoFaithful`; over a 200-frame SBS-full run it showed 78 core=A
     `ENTRY` lines and ZERO core=B lines, despite `mB->pc_skip=false` (same as A, per
     runtime/recomp/sbs.cpp:1576) and 0-diff RAM every frame. Core B never runs this native method
     at all — it reaches `Animation::advanceLinkChain` (called from this method's own unconditional
     tail, `d1(c, 0x80077b5cu, B)`) via the substrate's OWN per-frame chain instead, which for THIS
     particular leaf is ALSO reached from an independent still-substrate a00-overlay caller
     (`ra=0x80117AC0`, confirmed via `PSXPORT_DISPWATCH=0x80077B5C`) common to both cores — hence
     the observed A=5719 (2841 shared + 2878 A-only) vs B(gen)=2841 (shared only, exactly). A leaf
     reached via the EXACT SAME call-graph shape on both sides shows clean parity (Math functions,
     `billboardCompose1`/`2`, `ActorTomba::type7Interact`, `CubeTextLedger::activateSlot`, …) — the
     mismatch signal is only meaningful once the caller graph itself is confirmed symmetric.
  - **PcScheduler's small mismatches** (spawnPrim/spawnAndWait/forceClose/selfClose, values 0-5) are
    boot-time-only (frames <200): `PSXPORT_DEBUG=dispatch` shows `PcScheduler::spawnPrim` fired
    once at f0 with `ra=DEAD0000` from the native boot driver's task0 bootstrap (`FUN_80050b08`
    equivalent) — a one-time native-only call with no oracle-side per-frame equivalent, same class
    as (1)/(2) but at boot-scale rather than per-frame-scale.
- **dead end — DO NOT "fix" by adding a `noteSubstrateDispatch` call inside a psx_fallback-gated
  hand-rolled `shard_set_override` trampoline's gen branch.** Tried live during this triage
  (`EngineOverrides::traceGenHit` helper + call sites in pc_scheduler.cpp/graphics_bind.cpp/
  cube_text_ledger.cpp), then reverted: the gen branch is ALSO reached via `rec_dispatch`'s own
  `main_dispatch(c,addr)` fallthrough for a resident-module target (`generated/shard_disp.c
  main_dispatch` calls the SAME `func_<addr>(c)` wrapper), which `noteSubstrateDispatch` (called
  inside `rec_dispatch`, BEFORE the fallthrough) already counted — so the extra trampoline-side call
  DOUBLE-COUNTS every rec_dispatch-routed invocation. Confirmed: `PcScheduler::yieldPrim`, which
  matched EXACTLY at A=2990/B(gen)=2990 before the change, became B(gen)=5989 (~2×) after. The two
  ovhit counters are each individually complete for their OWN call shape (rec_dispatch vs raw
  g_override wrapper); a mismatch between them is real, structural, caller-graph information, not
  an instrumentation gap to paper over with more counting.
- **fix:** documentation only (docs/config.md "A/B call-count MISMATCH triage" + the permanent
  `sefprobe` probe as the reference technique for the next such question) — no code/counter change
  was correct here. `ovhit`'s existing `<-- COUNT MISMATCH` flag stays as-is (it's an accurate
  signal that the two call shapes differ in count; the triage step of "is the caller graph itself
  symmetric" still has to happen by hand, the same way `recdep`'s "0 in recdep ≠ dead" caveat does).
- **verification:** `PSXPORT_SBS_MODE=full` autonav headless, 0 `sbs-div`/`VIOLATION` through
  f3000, both before this triage pass and after landing the `sefprobe` probe.
- **refs:** docs/config.md (ovhit + sefprobe sections); game/core/engine.cpp
  `sceneEventFifoFaithful` (`sefprobe`); game/render/perobj_billboard.cpp; game/render/
  overlay_ground_gt3gt4.cpp; game/player/actor_tomba.cpp (`outerTransitionCommit`); game/core/
  pc_scheduler.cpp; docs/findings/render.md "dead end avoided" (the pre-existing sibling finding
  for the cmdListDispatch chain, predates `ovhit`).

## Engine/game overrides on the process-global g_override[]/g_ov_* tables contaminated the SBS oracle (false 0-div)
- **symptom:** SBS full reported 0 `sbs-div` through 15000+ frames for clusters that were actually WRONG. The oracle (core B) was running the NATIVE mirror for several render-packet-emitter clusters, not the pure substrate — so SBS compared native-vs-native and reported a clean gate, masking every real divergence.
- **status:** fixed 2026-07-09 (central oracle-gated thunk installer).
- **cause:** `g_override[]` and the `g_ov_<mod>_override[]` arrays are single PROCESS-GLOBAL tables shared by every Core, including SBS core B. The generated wrapper `func_X(c){ c->pc=X; if(g_override[i]){g_override[i](c);return;} gen_func_X(c); }` consults that table on BOTH cores. Four render clusters landed 2026-07-08 (`perobj_billboard` 0x8003CCA4/C2D4/C464/C8F4, `overlay_gt3gt4` 0x801465EC/679BC, `overlay_ground_gt3gt4` 0x8013FB88/3FE58/401B8, `quad_rtpt_submit` 0x8003B054/B320) with trampolines that called the native method UNCONDITIONALLY — no `psx_fallback` gate — so core B ran native. The `rec_dispatch`→`EngineOverrides` path was NOT affected (overlay_router.cpp already gates it on `!psx_fallback`); only the direct-substrate-call path was broken. (Per-trampoline `psx_fallback` gates work — gte_math/cull/node_xform/etc. self-gate correctly — but they get FORGOTTEN each new cluster, which is how this regressed.)
- **what "clean oracle" means (the spec):** the oracle runs ONLY PlatformHle (async→sync conversions + HLE BIOS, which MUST fire on both cores or the no-IRQ runtime hangs) + the pure `gen_func_*`/`ov_*_gen_*` body for everything else. This matches what `PSXPORT_GATE=1` ran ~7 days ago (commit 95157d3, before the 2026-07-08 render clusters landed).
- **fix:** `runtime/recomp/engine_override_thunk.cpp` — ONE shared thunk installed into the table; it reads `c->pc` (the wrapper stamps it immediately before invoking the override, so it is the guest address at entry) and runs `gen` on the oracle, `native` everywhere else. Engine/game installs go through `engine_set_override_main` / `engine_set_override_a00`; the raw `shard_set_override`/`ov_*_set_override` is reserved for PlatformHle + the scheduler primitives. The four contaminating clusters were converted; the gate now lives in one place and can't be forgotten per-cluster.
- **verification:** post-fix `PSXPORT_SBS_MODE=full` autonav immediately surfaced **19 real `[sbs-div]` at f117** in the packet pool (0x800BFFxx) + scratchpad — the exact render-cluster guest-RAM writes the false 0-div had been hiding. (Those f117 divergences are now eligible bug-hunt targets per docs/bug-hunt-workflow.md: the render clusters ARE fully owned → debug+fix.) Core A ran 117 frames through boot→field via the thunk, so the thunk routes correctly on the native side too.
- **rule / how to avoid re-deriving:** if SBS suddenly shows 0-div where it used to diverge, SUSPECT THE ORACLE (a new cluster installed ungated) before believing a fix. Never call raw `shard_set_override`/`ov_*_set_override` for engine/game code — use `engine_set_override_*`. The `rec_dispatch` path needs no per-cluster work (already gated).
- **refs:** runtime/recomp/engine_override_thunk.cpp; game/render/{perobj_billboard,overlay_gt3gt4,overlay_ground_gt3gt4,quad_rtpt_submit}.cpp; runtime/recomp/overlay_router.cpp:183 (the rec_dispatch gate, already correct); docs/bug-hunt-workflow.md "Oracle integrity".

## codemap.py mis-parsed inline description addresses as owner tags
- **symptom:** `tools/codemap.py --addr <hex>` reported false owners for many addresses. E.g. `0x8003E264 → ov_transition_e20 [LIVE]` was wrong: ov_transition_e20's real guest address is 0x80107E20, and 0x8003E264 is just a description reference (`// FUN_80107e20 — … 1 effect 0x8003e264 …`). Trap: acting on that mapping would try to "wire" a substrate address as if it were already native.
- **status:** fixed 7a396b4 (2026-07-02, session 15)
- **cause:** the parser ran `ADDR_RE.findall(htext) + FUN_RE.findall(htext)` over the WHOLE first header comment line and treated every match as an owned address. Any callee reference / data-block address in the description prose became a false owner.
- **fix:** owner harvest now runs only on the TAG PORTION of each header line (left of the first `—`/`:`/`- `). For headers without an in-tag address (mostly class methods like `Engine::frameStartTick — per-frame prologue at guest 0x80059D28`), the last-resort scan takes the FIRST address in textual order across the whole comment block, so a description-referenced callee (FUN_8005950C) doesn't outrank the earlier owner declaration (0x80059D28). Net: 228 → 212 owned addresses; all removed entries were description refs.
- **refs:** tools/codemap.py parse_file tag_portion; commit 7a396b4.

## recdep / any rec_dispatch hook is BLIND to resident→resident (intra-MAIN) calls
- **symptom:** `recdep` and ad-hoc `camtrace`-style hooks in `rec_dispatch` show ZERO calls to a resident MAIN function (e.g. the camera 0x8006E3B0/0x8006D02C) even while it demonstrably runs every frame — leading to a WRONG "that code never runs here" conclusion (cost later-290..292 chasing a phantom free-roam camera).
- **status:** known-issue / measurement caveat (2026-07-01, later-293)
- **cause:** the recompiler emits **intra-module (MAIN→MAIN) calls as DIRECT C calls** `func_XXXX(c)` (see generated/shard_*.c, shard_disp.c `func_XXXX`), NOT via `rec_dispatch`. `rec_dispatch` only fires for CROSS-module / overlay / unknown-indirect targets. So any hook in `rec_dispatch` (recdep histogram, camtrace) sees ONLY cross-module and indirect calls — it is structurally blind to the (large) set of resident→resident calls. Absence from recdep does NOT mean "not executed".
- **fix / how to actually tell if a resident fn runs:** don't infer from recdep. Use a GUEST-STACK backtrace at a known side effect (`PSXPORT_WWATCH=<addr>` on a memory location the fn writes + `guest_backtrace_to`, i.e. PSXPORT_WWATCH_BT), or instrument the recompiled `func_XXXX` / `gen_func_XXXX` directly, or grep the shards for `func_XXXX(c)` call sites. recdep remains valid for its stated purpose (ranking substrate/overlay dependency to minimize cross-module dispatch) — just never read "0 in recdep" as "dead".
- **refs:** runtime/recomp/overlay_router.cpp rec_dispatch (only cross-module); generated/shard_3.c:16799 func_8006E3B0(c) direct; docs/findings/camera.md (the false trail this caused).

## EngineOverrides::register_ is BLIND to a direct substrate call — needs shard_set_override too
- **symptom:** a new native port (ActorReward, FUN_80049A60/9E54/A3D4/B150/B208) registered ONLY via
  `EngineOverrides::register_` would silently never run when its sole caller is still-substrate
  (`FUN_8004AAC4`, unowned) — the SBS gate would show a fake 0-diff (both cores keep running the OLD
  substrate body unchanged), which looks identical to a real verified port. Same shape as the recdep
  blind-spot above: absence-of-divergence does NOT mean "reached".
- **status:** documented workaround (this session) — a real fix would make `EngineOverrides::register_`
  always also wire `shard_set_override`, closing the gap generally; not done here (out of scope for a
  single-cluster port).
- **cause:** `EngineOverrides::run()` fires ONLY inside `rec_dispatch(c, addr)` — i.e. only for calls a
  NATIVE caller makes explicitly through `rec_dispatch`. The recompiler's OWN emitted calls (`func_XXXX
  (c)` for a direct jal, or the `main_dispatch` switch for an indirect jalr) NEVER go through
  `rec_dispatch` — they check the recompiler's separate `g_override[]` table (`shard_disp.c`'s
  `func_XXXX` wrapper: `if (g_override[i]) { g_override[i](c); return; } gen_func_XXXX(c);`).
  `EngineOverrides::register_` does NOT populate `g_override[]`. So an EngineOverrides-only registration
  is invisible to a substrate-originated call — only visible to a native caller that already knows to
  reach the address via `rec_dispatch`. `PlatformHle::register_` (sync_overrides.cpp) already works
  around this for its own table by calling `extern void shard_set_override(uint32_t, OverrideFn)` after
  registering — the same fix a new game-logic port needs when its caller is still substrate.
- **gotcha (core-B safety):** `g_override[]` is a SINGLE PROCESS-GLOBAL array shared by every `Core`
  (unlike EngineOverrides, which is per-`Game`) — so `shard_set_override` can NOT gate on
  `psx_fallback` the way `rec_dispatch` does for EngineOverrides. The installed function itself must
  check `c->game->psx_fallback` and fall back to calling the real `gen_func_XXXX(c)` when true, or SBS
  core B (the pure substrate reference) would silently start running the native port too — turning the
  compare into "native vs itself" (a fake 0-diff that proves nothing). See game/object/actor_sm_reward.cpp
  `ov_sm*` trampolines for the pattern.
- **fix (this session, local):** register BOTH tables from one `registerOverrides(Game*)`: `EngineOverrides
  ::register_` (native-caller tracing) + `shard_set_override` with a `psx_fallback`-gated trampoline
  (actual substrate-call redirection). Called once from `boot.cpp`'s `main()`.
- **FALSIFIED claim, corrected 2026-07-08:** the line above used to also claim this single call was
  "sufficient for EVERY Core/Game created afterward (including SBS's two separately-constructed
  cores...) because `g_override[]` is process-global, populated once." That is true ONLY for the
  `g_override[]`/`shard_set_override` half. It is FALSE for the `EngineOverrides::register_` half:
  `EngineOverrides` is a **per-Game** member (`game->engine_overrides`), and `rec_dispatch`'s gate
  reads the CALLING Core's OWN Game (`c->game->engine_overrides.run(c, addr)`). SBS/DualCore/
  Selftest each construct their OWN `Game` instances and never ran `boot.cpp`'s `main()`, so their
  `engine_overrides` table was **completely empty** — see the next entry, which is the bug this
  false claim was hiding.
- **refs:** runtime/recomp/overlay_router.cpp `rec_dispatch` (EngineOverrides gate); generated/shard_disp.c
  `func_XXXX` wrapper + `shard_set_override`; runtime/recomp/sync_overrides.cpp `PlatformHle::register_`
  (the pre-existing dual-registration precedent); game/object/actor_sm_reward.{h,cpp}.

## CRITICAL (found + fixed 2026-07-08): SBS/DualCore/Selftest never populated their own Game's EngineOverrides table — every EngineOverrides-only override was COMPLETELY INERT under SBS
- **symptom:** a verification pass (spawned to prove EngineOverrides actually intercepts on the
  running game) found TWO smoking guns: (1) `PSXPORT_DEBUG=all` over a full session showed ZERO
  `[dispatch]` lines project-wide, even for long-established overrides; (2) `recdep` showed HIGH
  substrate-dispatch counts for guest addresses that codemap says are already natively owned
  (`Math::applyMatlv` 0x80084220 etc.). Chasing (1)+(2) down with a real headless run (CHD attached,
  `PSXPORT_AUTO_SKIP=1`, `PSXPORT_DEBUG=dispatch,recdep,ovhit`) found the TRUE, much bigger bug.
- **status:** fixed (commit b078729, merged to main). New `ovhit` channel added alongside so this
  class of gap is observable going forward without re-deriving it by hand.
- **root cause, signal (2) — NOT a bug, an ORPHAN (already-documented pattern, re-confirmed):**
  `Math::applyMatlv`/`applyMatrixLV`/`rotZ`/`rotY` are real native C++ methods with their OWN
  NEW callers (`NodeXform::propagate` etc., per `codemap.py --addr`) — but they were NEVER
  registered as an `EngineOverrides` entry NOR dual-wired via `shard_set_override`. So OLD
  overlay-resident callers (still-substrate AI code in the 0x8014xxxx range, calling these MAIN-
  module addresses cross-module — hence visible to `rec_dispatch`/`recdep`) still run the OLD
  substrate `gen_func` body every time. `recdep`'s "high count for an owned function" is exactly
  the already-documented "recdep hot-leaf leaderboard: entries may ALREADY be native-ported
  (ORPHAN)" finding above, generalized past render/cull functions to Math. Confirmed empirically:
  zero `[dispatch]` lines for these 4 addresses in a real free-roam session despite 26932/5492/
  5392/5392 `recdep` hits each.
- **root cause, signal (1) — a REAL, CRITICAL bug, much bigger than "PcScheduler looks silent":**
  `EngineOverrides` is per-`Game`. `boot.cpp`'s `main()` populates it on ONE throwaway `Game` it
  constructs itself, then (for SBS/DualCore/Selftest) hands off to a harness that builds its OWN
  separate `Game`(s) and boots them via the shared `dc_boot_init(Core*)` (native_boot.cpp) —
  which NEVER called any `registerOverrides()`. Result: on every SBS core (A AND B), `rec_dispatch`'s
  `c->game->engine_overrides.run(c, addr)` check ALWAYS missed for every override wired ONLY via
  `EngineOverrides::register_` (no `shard_set_override` dual-registration) — that's Animation
  (loadFrame/advanceLinkChain/attach), ActorZonedAttacker (6 addresses), Spawn (4 addresses),
  ReleaseTriggerMotion (6 addresses), and — until this session's separate fix below — PcScheduler
  (5 addresses). **Both SBS cores silently ran the IDENTICAL substrate body for all of these
  addresses.** SBS's byte-exact compare "passed" for anything routed only through these natives —
  not because the native port matched, but because the native port never ran on EITHER side. This
  is precisely the "SBS gate passed trivially" failure mode CLAUDE.md's "no residual RAM diverges"
  rule exists to catch, and it had been silently true for every EngineOverrides-only registration
  since the mechanism was introduced (2026-07-07).
- **separate, additional finding — PcScheduler's 5 primitives were ALSO blind even OUTSIDE SBS:**
  independent of the above, `PcScheduler::yieldPrim/spawnPrim/spawnAndWait/forceClose/selfClose`
  were registered via `EngineOverrides::register_` ONLY (no `shard_set_override`). Every real
  substrate call site to these 5 guest addresses is a DIRECT intra-module `func_<addr>(c)` (grep
  `generated/shard_*.c`: 15/7/2/7/8 call sites respectively) — never `rec_dispatch` — so even on
  the default (non-SBS) `pc_faithful`/`pc_skip` path these 5 addresses ran the OLD substrate body
  for every real in-game yield/close. A live default-path run before the fix showed exactly ONE
  `[dispatch]` hit total across all 5 (spawnPrim, called once from the native boot driver at
  `ra=DEAD0000` — not from gameplay) and zero for the other four, despite the game visibly
  scheduling tasks every frame.
- **fix:**
  1. `game/core/pc_scheduler.cpp`: dual-register all 5 primitives via `shard_set_override` with
     psx_fallback-gated trampolines (the `game/object/actor_sm_reward.cpp` pattern), each calling
     the NEW `EngineOverrides::traceHit()` before invoking the native handler so the hit stays
     observable on the `dispatch`/`ovhit` channels despite bypassing `run()`.
  2. `runtime/recomp/boot.cpp`: extracted the inline registration block in `main()` into
     `void register_engine_overrides(Game*)`.
  3. `runtime/recomp/native_boot.cpp`: call `register_engine_overrides(c->game)` inside
     `dc_boot_init` — the ONE shared boot helper SBS/DualCore/Selftest all use — BEFORE
     `crt0_setup`/`game_init` (a live crash proved ordering matters: `game_init`'s init prefix can
     itself reach a direct-substrate g_override[] call before the table is populated, aborting
     `traceHit`'s "unregistered" assert).
  4. `runtime/recomp/engine_overrides.{h,cpp}`: added `traceHit(Core*, addr)` (records a hit that
     bypassed `run()`) and `dumpHitCounts()` (the `ovhit` channel — per-address hit counts dumped
     at exit, flagging any registered-but-zero address).
- **verification:** rebuilt, re-ran. Default path (`PSXPORT_AUTO_SKIP=1`,
  `PSXPORT_DEBUG=ovhit,dispatch,recdep`): PcScheduler's spawnPrim/forceClose/selfClose now show
  real hit counts (2/1/2 in a 90s session) via `traceHit`, where before the fix they were exactly
  0. SBS full mode (  `PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1`): no longer crashes (after the
  ordering fix), and — significant — **immediately surfaces a real guest-RAM divergence around
  frame 61** in `Animation::loadFrame`/`advanceLinkChain` (SBS's last-writer report: core A pc
  0x8007AAE8 vs core B pc 0x80076904/0x80077C74) that the broken registration was previously
  hiding. **RESOLVED in a follow-up commit (8c6e1ce)** — root-caused to a one-line bit-accounting
  bug in `anim_unpack_pose_triple` (`stream = s + 5` ate a shared nibble → `stream = s + 4`);
  post-fix SBS full autonav is 0 sbs-div through f9100+. Full trail in
  docs/findings/animation.md ("anim_unpack_pose_triple ate a shared nibble").
- **refs:** runtime/recomp/engine_overrides.{h,cpp}; runtime/recomp/boot.cpp
  `register_engine_overrides`; runtime/recomp/native_boot.cpp `dc_boot_init`; game/core/
  pc_scheduler.cpp; docs/config.md `dispatch`/`ovhit`; scratch/logs/default_ovhit.log,
  scratch/logs/sbs_ovhit3.log (this session's evidence, worktree-local, not committed).

## A debug-server client disconnect kills the whole game (SIGPIPE)
- **symptom:** the live port dies ("connection refused" on the next dbgclient call) when a debug client is killed / times out mid-reply — looked like a crash on entering a scene, but no abort/diagnostic is printed.
- **status:** fixed 10a07e0
- **cause:** the debug server's socket write() raised SIGPIPE with the DEFAULT disposition (terminate the process); a dropped/timed-out dbgclient connection therefore killed the game.
- **fix:** dbg_server.cpp dbg_server_start: signal(SIGPIPE, SIG_IGN). A dropped client now just makes write()<0 and drops that one connection.
- **refs:** journal later-252; dbg_server.cpp dbg_server_start

## Reaching the New-Game cutscene live: mash START (it confirms the menu AND skips FMV)
- **symptom:** automated drive (debug-server taps) loops in the attract demo / never reaches the field cutscene (stage 0x8010637C); the title menu's confirm wasn't registering.
- **status:** known-good (how-to)
- **cause:** the real-time menu→cutscene cadence; the attract demo replays OP.STR if no valid selection lands.
- **fix:** drive: tap START at the title demo (→ menu, scene-active=2), then mash START — it both confirms New Game and skips the FMV — and poll `stage` (debug server) for 0x8010637C. The debug server now survives client drops (SIGPIPE fix above) so a `timeout`-killed dbgclient won't kill the game. Headless can't reproduce the menu cadence — needs the windowed game.
- **refs:** tools/dbgclient.py; docs/driving-the-game.md

## SBS both-mode: panes black + silent on macOS/MoltenVK (fine on linux/AMD)
- **symptom:** `PSXPORT_SBS_MODE=both ./run.sh` reaches free-roam but BOTH panes are black at EVERY stage (title/demo AND field — confirmed by the user it's total black) and there's no audio, on Apple M2 / MoltenVK; only the RmlUi overlay shows. Plain `./run.sh` (single core) is visible AND audible on the same Mac.
- **status:** ROOT-CAUSED + fixed (barrier READ access) — awaiting Mac confirm
- **cause:** ROOT CAUSE (found via sync validation, NOT a guess): `SYNC-HAZARD-READ-AFTER-WRITE` in `panel_render` (gpu_gpu.cpp). The panel render pass uses `loadOp = VK_ATTACHMENT_LOAD_OP_LOAD` (it preserves the just-uploaded VRAM backdrop, then draws geometry on top), so beginning it READS the color attachment. But the barrier transitioning that image TRANSFER_DST→COLOR_ATTACHMENT_OPTIMAL granted only `VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT`, not `..._READ_BIT` — so the loadOp-read was unsynchronized against the prior transfer-write. RADV ignores the missing READ (renders fine — which is why linux/AMD SBS worked + masked it); MoltenVK/Metal ENFORCES it, so the load read nothing → both panes black at every stage. THE PER-PANE s_vram_tex THEORY WAS WRONG (reverted): sync validation flagged NO hazard on s_vram_tex — that sharing was correctly barriered. (The other validation noise, VUID-vkCmdPushConstants-offset-01796 ×20 from overlapping FRAGMENT[0,64)/VERTEX[16,48) push ranges, is spec-pedantry MoltenVK's per-stage push model handles; NOT the black cause — left as-is.)
- **fix:** gpu_gpu.cpp panel_render: the TRANSFER_DST→COLOR_ATTACHMENT_OPTIMAL barrier's dstAccessMask is now `COLOR_ATTACHMENT_WRITE | COLOR_ATTACHMENT_READ`. Verified on linux: the SYNC-HAZARD validation error went from many→0 (`VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_VALIDATE_SYNC=1 PSXPORT_SBS_MODE=both ./run.sh`). DIAGNOSTIC LESSON: the validation layer IS installed on linux/AMD — run sync validation THERE to catch MoltenVK-enforced hazards that RADV silently tolerates, instead of guessing.
- **refs:** gpu_gpu.cpp panel_render barrier; journal later-253; the per-pane mis-fix was reverted (commit f395e1d → revert)

## codemap.py: an orphan doc-comment block hides the addresses of the def below it
- **symptom:** after owning a function natively (correct code, builds, A/B 0-diff), `tools/codemap.py --addr 0x80025588` still prints "NO native owner found", and docs/code-map.md maps the new native symbol to a WRONG/neighboring guest address (with a mismatched description).
- **status:** worked-around (blank-line separation); underlying heuristic gap noted
- **cause:** codemap keys a native's primary address off the FIRST line (up to the first em-dash/colon) of the CONTIGUOUS `//` block immediately above its `void ov_xxx(Core*` def. A pre-existing *orphan* doc-comment block (documentation for a handler that has no function under it — e.g. the "GAME sm[0x4a]==1 handler 0x801088d8" block in engine_stage.cpp) that sits directly above your new function, with NO blank line, gets merged into your function's comment. The merged block's first line names the orphan's address, so THAT becomes the primary and your function's real addresses (which live on later comment lines, past the em-dash) are never indexed.
- **fix:** put a blank line between an orphan/standalone doc-comment and the next function's own comment, so codemap collects only the function's own block. Verify with `tools/codemap.py --addr <fn-addr>`. Proper long-term fix (not yet done): codemap should stop attaching a comment block to a def when the block's first line addr ≠ any address referenced in the def body, or should scan the whole block (not just the header line) for the impl address.
- **refs:** tools/codemap.py parse_file (comment collection ~L78-116) + impl-address selection (~L100-116); engine_stage.cpp ov_scene_25588/ov_scene_4fe84; journal later-289

## recdep hot-leaf leaderboard: entries may ALREADY be native-ported (ORPHAN) — check codemap FIRST
- **symptom:** the handoff points at the top substrate-dispatch entries by calls/frame (e.g. 0x8007778C @115k, 0x80077ACC @48k, 0x8002B278 @38k, 0x8004D7EC @36k) as "port these next". Session-2026-07-02 disassembled 0x8007778C to reimplement — only to discover `tools/codemap.py --addr 0x8007778C` already shows it as `ov_cull_wrapper` in game/render/cull.cpp, ORPHAN. Same for the other three. Wasted time on a redundant RE pass before the check.
- **status:** workflow rule (2026-07-02)
- **cause:** the leaderboard measures rec_dispatch hits at CALL SITES — a hot substrate leaf can appear either because it has no native port OR because a written-but-orphan native port isn't wired into its native callers. The two states look identical at the recdep layer.
- **fix / rule:** for EVERY frontier target the handoff (or recdep) lists, run `tools/codemap.py --addr 0xADDR` FIRST before disassembling. If it's already ORPHAN, the work is WIRING (grep the callers, replace `rec_dispatch(c, 0xADDR)` with the native call), not porting. Only if codemap says NO native owner do you disassemble and reimplement.
- **refs:** game/render/cull.cpp (ov_cull_wrapper / ov_cull_wrap_77acc / ov_cone_cull_2b278 all ORPHAN); game/math/mathlib.cpp ov_bittest_4d7ec (was ORPHAN, wired e9f4b1e); scratch/logs/recdep_new.log

## ORPHAN native with a `_verify` wrapper is NOT proof it's byte-perfect
- **symptom:** trying to wire ov_orch597AC (FUN_800597AC) at engine_submit.cpp:1211 tripped a 9-byte main-RAM A/B mismatch @0x1FFCC9 at frame 500 even though the native has an `orchverify` gate. Wire had to be reverted.
- **status:** workflow caveat (2026-07-02)
- **cause:** a `*_verify` wrapper (record_gate / an inline snapshot+super_call harness) means someone WROTE the compare scaffolding, not that the compare has ever RUN clean. The gate is off by default (cfg_dbg("orchverify") is 0 unless set), so the default path is unverified native. Wire-time A/B may still fail if the native impl and recomp body actually differ.
- **fix / rule:** the wire-time A/B (main-RAM + scratchpad cmp = 0) is the ONLY proof. If a wire trips a diff, revert immediately — the "byte-perfect" native was never actually proven. Optionally rerun with the gate on (e.g. `PSXPORT_DEBUG=orchverify`) to localize the divergence, but do NOT ship the wire.
- **refs:** attempted wire reverted 2026-07-02; game/render/engine_submit.cpp ov_orch597AC (orchverify gate); the earlier successful wires in this session (ov_unpack_group, ov_obj_pos_compose, spawn_dispatch, ov_obj_record_init, the collision-grid trio) all had A/B = 0.

## A fully-RE'd, bit-exact `class Math` (GTE matrix cluster) sat dead — nobody ever called `registerOverrides()`
- **symptom:** `tools/codemap.py --addr 0x80084110` said "NO native owner found", but `Math::matMul` was
  right there in game/math/gte_math.cpp — RE'd to the 44-bit GTE accumulator, with a header comment
  calling it "the biggest single perf lever" (16.2% of hot interpreter time, 55k+ calls/frame). The class
  had NO `registerOverrides()` method at all; nothing ever called `EngineOverrides::register_` or
  `shard_set_override` for its 7 addresses (matMul/applyMatlv/applyMatrixLV/rotmat/rotX/rotY/rotZ —
  0x80084110/80084220/80084470/80085480/80084D10/80084EB0/85050). Only the small number of direct
  `c->math.*` call sites in node_xform.cpp/cutscene_camera.cpp/graphics_bind.cpp ever ran it; every
  substrate-originated `func_<addr>(c)` call (the actual hot path) still ran the interpreted `gen_func_*`
  GTE-op body untouched.
- **status:** fixed (this session) — `Math::registerOverrides()` added + called from `boot.cpp`; SBS
  0-diff over 3 separate runs (~7000/~7650/final frames) with an instrumented print CONFIRMING the
  native path actually executes (not just an untested 0-diff — see the caveat pattern below).
- **cause:** a prior session ported + bit-exact-verified the class (per its header comments) but never
  did the final wiring step. Nothing catches this: codemap.py's LIVE/ORPHAN reachability check only
  walks the C call graph from native_boot roots — it doesn't know whether an address is actually WIRED
  to a Core-reachable path, only whether the code exists and something calls it (and `c->math.matMul(...)`
  direct call sites from node_xform.cpp DO make it call-graph-reachable, so it read as "LIVE" by that
  metric even before this fix — LIVE ≠ "every caller of the guest address reaches this native").
- **fix:** dual-wire same as `ActorReward` (see the entry above this file) — `EngineOverrides::register_`
  for `rec_dispatch`-based native callers + `shard_set_override` with a `psx_fallback`-gated trampoline
  for the substrate's own direct `func_<addr>(c)` calls. Verify a wire ACTUALLY fires (not just "SBS
  stayed 0-diff", which is also true of a no-op wire) with a temporary instrumented print in the
  installed trampoline, reverted before commit.
- **also fixed:** codemap.py's "comment block immediately above the def" heuristic found nothing for 4 of
  the 7 methods because their `// FUN_xxxx` header comments sat above file-local helper functions
  (`load_mat3`/`clamp16s`/`sign44`), not directly above the `Math::` method defs. Added the standard
  trailing `// FUN_xxxx` def-line tag (same convention as `Camera::lookAt`) — same class of gap as the
  "orphan doc-comment block" entry above, just the opposite direction (comment too far ABOVE, not a
  wrong block MERGED in).
- **WORKFLOW GAP flagged, not fixed:** `dc_boot_init` (shared by SBS/dualcore/selftest) never calls ANY
  subsystem's `registerOverrides()` — those only run in `boot.cpp`'s `main()`, on a throwaway `Game`
  that SBS never uses. `shard_set_override` still works under SBS (process-global table, populated once
  by that throwaway `game` before `Sbs::run()` diverts) but `EngineOverrides::register_` does NOT (SBS's
  `mA`/`mB` each get an empty `engine_overrides` — `rec_dispatch`'s `EngineOverrides::run` can never fire
  on an SBS core). This likely explains the "0 dispatch hits observed" caveats on other clusters
  (`ActorZonedAttacker`, `ActorReward`) — not that autonav doesn't reach those actors, but that
  `EngineOverrides` is structurally unreachable under SBS regardless. Real fix: call every subsystem's
  `registerOverrides()` from `dc_boot_init` too. Out of scope here — touches every prior cluster's wiring.
- **refs:** game/math/gte_math.{h,cpp} (Math::registerOverrides); runtime/recomp/boot.cpp; docs/port-
  progress.md "GTE matrix cluster" session entry; the `ActorReward` dual-wiring entry above.

## Wiring a banked draft found 2 real transcription bugs via generated/ byte-trace (not caught by SBS alone)

- **symptom:** two wide-RE-drafted-but-unwired clusters (game/object/cube_text_ledger.{h,cpp},
  game/player/actor_tomba.cpp's 4 postInteractWalk sub-handlers) were ready to wire onto the frontier.
  Both had extensive RE doc comments and "matches Ghidra 1:1" claims, but a line-by-line cross-check
  against `generated/shard_*.c` (the recompiler's own emission — ground truth per faithful-execution.md)
  found two real bugs the draft-time RE missed.
- **bug 1 — register-leak clobbered:** `ActorTomba::type7Interact` set `c->r[6] = item` before its
  second dispatch call (`LEAF_STEP_MODE_FLAG`). Ground truth (`gen_func_800235A0`) never reloads r6
  there — a2 is a caller-saved scratch register left over from the FIRST call's (`LEAF_PROX_STEP`)
  side effects, and the original PSX compiler simply never re-set it, so whatever that callee left in
  r6 becomes an unintended argument to the second callee. Since `LEAF_PROX_STEP` is the SAME substrate
  body on both SBS cores, its post-call r6 value is naturally identical either way — clobbering it with
  `item` broke that identity. This is the kind of bug that's easy to miss reading a decompile (Ghidra
  hides "unused" register carryover) but jumps out immediately in the raw per-register recompiler
  emission.
- **bug 2 — polarity swap:** `ActorTomba::growthYSnap` picked the growth-offset constant backwards:
  `(g17E < 0) ? 0x8C : 0x46` when ground truth (`gen_func_80022C78`) is `(g17E < 0) ? 0x46 : 0x8C` — the
  branch-taken path explicitly RESETS the constant register at its target label, while the fallthrough
  keeps the branch instruction's own delay-slot preset; a decompile-level read can easily flip which
  path "owns" which constant. The same swapped constant was inlined a second time in
  `ActorTomba::type8Interact`'s "just left growth" tail, so one fix corrected both call sites.
- **why SBS alone wouldn't have caught these cleanly:** SBS gates on DIVERGENCE, not on "did this branch
  even execute" — bug 1's `LEAF_STEP_MODE_FLAG` reads a2 for something SBS would show diverging (good),
  but only on a play-through that actually reaches type7Interact's `v0>=0` path; bug 2's growth-flag
  branch requires the grow/shrink mechanic to trigger. A short autonav SBS run can easily land 0-diff
  while never exercising the buggy branch, then regress later. **Lesson: for a wired draft, do the
  register/constant-level RE cross-check against `generated/` BEFORE relying on a single SBS run to
  "prove" it — SBS confirms no divergence on the states it reached, not correctness of unreached
  branches.**
- **fix:** both corrected in game/player/actor_tomba.cpp (see the in-code "BUG FIX" comments); re-gated
  SBS-full, 0-diff over ~7650 frames, with `type7Interact` firing 1040 times in that run (exercising the
  exact branch bug 1 touched) and 0 divergence. `stepModeInteract`/`type8Interact`/`growthYSnap`
  (bug 2's branch) did not fire in this autonav window — their correctness rests on the RE trace, not
  fresh empirical confirmation; flag for a future session with a longer/different playthrough.
- **also wired this session (no bugs found):** `CubeTextLedger::activateSlot`/`deactivateSlot`/
  `spawnPopup` (0x80040B48/80040C00/80040AA4), dual-wired same as `ActorReward`; `lookupCost`
  (0x80040A58) deliberately left UNwired since it's only ever reached by a direct intra-function call
  from inside `gen_func_80040B48`/`gen_func_80040C00`'s own bodies — no rec_dispatch caller exists, and
  wiring it via `shard_set_override` would risk diverting core B's reference execution (the g_override[]
  table is process-global and unconditional at the reader, unlike `EngineOverrides` which self-gates on
  `psx_fallback`).
- **refs:** game/object/cube_text_ledger.{h,cpp}; game/player/actor_tomba.{h,cpp}; runtime/recomp/
  boot.cpp `register_engine_overrides`; generated/shard_2.c:4542 (`gen_func_80040A58`), shard_3.c:11258
  (`gen_func_80040AA4`), shard_4.c:4944 (`gen_func_80040B48`), shard_5.c:5496 (`gen_func_80040C00`),
  shard_7.c:1379 (`gen_func_80020364`), shard_0.c:1112/1466 (`gen_func_800205CC`/`gen_func_80022C78`),
  shard_4.c:1267 (`gen_func_800235A0`).

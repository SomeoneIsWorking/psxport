# Findings — tooling / debug server / harness

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
- **status:** fixed this session (worktree `agent-a1a0cae350ac4aa9d`, not yet merged to main — see
  commit list). New `ovhit` channel added alongside so this class of gap is observable going
  forward without re-deriving it by hand.
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
  0. SBS full mode (`PSXPORT_SBS_MODE=full PSXPORT_SBS_AUTONAV=1`): no longer crashes (after the
  ordering fix), and — significant — **immediately surfaces a real guest-RAM divergence around
  frame 61** in `Animation::loadFrame`/`advanceLinkChain` (SBS's last-writer report: core A pc
  0x8007AAE8 vs core B pc 0x80076904/0x80077C74) that the broken registration was previously
  hiding. That divergence is a NEW, real Job#1 target — NOT investigated or fixed in this session
  (out of scope: this was a verification+tooling task); root-causing it is follow-up work, and it
  should NOT be waved off as expected/residual per the "no residual RAM diverges" rule.
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

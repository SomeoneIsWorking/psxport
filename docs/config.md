# Configuration & debug flags — `cfg` module + the REPL-driven `debug` channel set

Read this before adding a new `getenv("PSXPORT_…")`. The repo accumulated ~105 ad-hoc env flags, each
with its own `static int x=-1; if(x<0) x=getenv(...)` boilerplate. That is now centralized.

## The 5 canonical run flags (path selection) — see CLAUDE.md for full vocab

| flag | selects | notes |
|---|---|---|
| (none) | pc_faithful + pc_render | Default. Byte-exact target for SBS. Currently broken (Job#1). |
| `PSXPORT_GATE=1` | recomp_path + pc_render | Substrate runs gameplay; native renderer. Works, render issues. |
| `PSXPORT_RENDER_PSX=1` | pc_faithful + psx_render | Substrate renderer only. Faithful still broken. |
| `PSXPORT_GATE=1 PSXPORT_RENDER_PSX=1` | recomp_path + psx_render | THE REFERENCE. Works perfectly. |
| `PSXPORT_SBS_MODE=full` | dual-core byte-compare | Core A = pc_faithful, Core B = recomp_path. Job#1 harness. |

The per-fork shortcut bool is `Game::mPcSkip` — see the class comment on `runtime/recomp/game.h`. Default
`mPcSkip=true` (shortcuts on); SBS forces it `false` so the faithful branch of every fork is exercised.

**No new env GATING (2026-06-20).** The PC-native port is the only path — do NOT add a `PSXPORT_*` flag
that branches game behavior/render/features. Visual settings (widescreen/hi-res/SSAO/light/60fps) are
the F1 overlay + `psxport_settings.ini` (`runtime/recomp/mods.c`), not env. Diagnostics are the REPL
`debug <chan>` command (below), not env. `cfg_*` remains only for genuine launch config (disc path,
window/headless run mode).

### Video settings file (`psxport_settings.ini`, path overridable with `PSXPORT_SETTINGS`)
The F1 overlay writes `key=value` lines; `mods_load()` restores them at launch (`runtime/recomp/mods.{h,c}`).
The two video selectors:
- **`aspect`** — `0`=Vanilla (4:3), `1`=16:9, `2`=21:9, `3`=Auto (match the live window aspect). Widescreen
  is a genuine wider FOV (the engine shifts the projection center OFX to `nw/2`), not a present stretch;
  it is forced OFF under `PSXPORT_ORACLE` and in the SBS legs (they run 4:3), so the byte-exact reference
  is untouched. Overlay row: "Aspect Ratio".
- **`ires`** — internal-resolution scale, ONE merged selector: `0`=Auto (derive from window height),
  `1`=Vanilla (1x), `2`=X2, `3`=X3, `4`=X4. Capped by `VRAM_W(1024) / native_w` in `gpu_gpu_video_status`.
  Overlay row: "Internal Resolution". (Replaced the old two-row `ires` 1..3 + `ires_auto` bool; a legacy
  file carrying `ires_auto=1` is migrated to `ires=0` on load.) NOTE: the Pass-1 SDL_GPU renderer does not
  yet render to a supersampled scratch FB (`GpuGpuState::frame_via_fb()==0`), so `ires` currently persists
  and reports but is not yet consumed by the raster — X2..X4 do not visibly sharpen until the scaled-FB pass
  lands. The selector + cap are in place for that pass.

## The rule: don't call `getenv` — use `cfg` (`runtime/recomp/cfg.h`)
```c
#include "cfg.h"
cfg_on("PSXPORT_FOO")     // boolean config/feature flag: env present and != "0" -> 1   (cached)
cfg_int("PSXPORT_FOO", d) // integer-valued flag (frame number, scale, port…), default d (cached)
cfg_str("PSXPORT_FOO")    // string-valued flag (paths, "x,y" coords); NULL if unset    (cached)
cfg_dbg("chan")           // is debug CHANNEL `chan` on? set at runtime via the REPL `debug chan,chan…`
```
- Every lookup reads the environment **once** and caches. In hot paths (per-prim / per-GTE-op /
  per-store) still keep a local `static int x=-1; if(x<0) x=cfg_*(…)` so there is no per-call scan.
- `-w` is on in the build, so an int/pointer mix-up (e.g. mapping a *valued* flag to `cfg_dbg`) is **not**
  warned — but `-Wint-conversion` is still an **error**, so the compiler catches that specific mistake.
  Still: classify before you wire (boolean → `cfg_on`/`cfg_dbg`; anything whose value is read → `cfg_str`/`cfg_int`).
- `cfg_dump()` logs every active `PSXPORT_*` var once (boot-time visibility).

## Debug output is REPL-driven: `debug chanA,chanB` (or `debug all`, `debug` to clear)
~31 boolean `*_DBG` / `*LOG` / `*WATCH` / `VERBOSE` flags were collapsed into named channels, enabled at
runtime via the REPL / debug-server `debug` command (`cfg_dbg_set`), NOT an env var. Enable several
together: `debug spu,cdcmd,bgm`. Old → new channel:

| old env flag | channel | | old env flag | channel |
|---|---|---|---|---|
| `PSXPORT_AUDIO_LOG`    | `audio`   | | `PSXPORT_REDDBG`      | `red`       |
| `PSXPORT_AUDIO_RATE`   | `audiorate`| | `PSXPORT_RTPCALLER`  | `rtpcaller` |
| `PSXPORT_CARD_VERBOSE` | `card`    | | `PSXPORT_SCHEDDBG`   | `sched`     |
| `PSXPORT_CDCMD_DBG`    | `cdcmd`   | | `PSXPORT_SEQDBG`     | `seq`       |
| `PSXPORT_CDC_VERBOSE`  | `cdc`     | | `PSXPORT_SPINDBG`    | `spin`      |
| `PSXPORT_CD_VERBOSE`   | `cd`      | | `PSXPORT_SPU_DBG`    | `spu`       |
| `PSXPORT_ENGINE_DBG`   | `engine`  | | `PSXPORT_SPU_PROF`   | `spuprof`   |
| `PSXPORT_ENVDBG`       | `env`     | | `PSXPORT_STAGETL`    | `stage`     |
| `PSXPORT_FMV_DEBUG`    | `fmv`     | | `PSXPORT_STREAMDBG`  | `stream`    |
| `PSXPORT_GP1LOG`       | `gp1`     | | `PSXPORT_TEXTDBG`    | `text`      |
| `PSXPORT_GPU_LOG`      | `gpu`     | | `PSXPORT_UNPACKLOG`  | `unpack`    |
| `PSXPORT_LDHAZARD`     | `ldhazard`| | `PSXPORT_UPLOADLOG`  | `upload`    |
| `PSXPORT_OBJLOG`       | `obj`     | | `PSXPORT_VSYNCLOG`   | `vsync`     |
| `PSXPORT_OTDBG`        | `ot`      | | `PSXPORT_VRAMSCAN`   | `vramscan`  |
| `PSXPORT_CDCMD_DBG`    | `cdcmd`   | | `PSXPORT_FPS60_SDBG` | `fps60`     |
| (so e.g. `PSXPORT_CDCMD_DBG=1` → `PSXPORT_DEBUG=cdcmd`) | | | `PSXPORT_WS_SXHIST` | `sxhist` |

New channels (no legacy var): `schedf` (per-frame cooperative task0/1/2 state + GAME `sm[0x48/4a/4c/5c]`
trace, native_boot.cpp — for stage/scheduler debugging) · `stage` (GAME stage-machine native-ownership log,
game/core/engine.cpp) · `rqhist` (per-frame render-queue layer×opaque/semi histogram, render_queue.cpp — "is
the world even being queued?") · `ovload` (per-core MODE/AREA-slot overlay residency: logs each
`overlay_note_load` — `core A/B slot N <- TAG` — so you can see WHICH overlay each core thinks is resident
and WHEN it loaded; the tool that pinned later-273's "A00 code overlay never loaded on the PSX core",
overlay_router.cpp) · `recdep` (RECOMP-DEPENDENCY meter: histograms every substrate function `rec_dispatch`
routes to and dumps the top-40 by call count at exit, overlay_router.cpp — the metric for the "minimize
recomp" goal; run `PSXPORT_DEBUG=recdep` on a free-roam session to rank which substrate functions to own
natively next. 410 unique in free-roam; #1 is `rand` 0x8009A450 @ 86/frame). Add `recdep-all` (e.g.
`PSXPORT_DEBUG=recdep,recdep-all`) to dump the FULL histogram instead of just the top-40 — needed to spot
rare/low-call-count targets in a specific address band the top-40 truncation would otherwise hide. See
journal later-168 /
engine_re.md "GAME stage state machine" · `silbbox` (dark-outline render-bug diag, `render_internal.h`
`sil_bbox_log`/`_i`: logs the screen bbox of every quad — native_gt3gt4's GT3/GT4/byte-packed submitters,
`ov_bg_tilemap_native`'s sky backdrop — that overlaps the coastal-ridge repro window x=5..30 y=134-138, see
`docs/findings/render.md` "Screen-fade transitions" / `scratch/handoff.md`) · `vmt` (music_coord.cpp —
`[vmt]`/`[gain2]` traces of `MusicCoord::voiceMixTick`'s ramp/smoother state (cur/tgt/g2cur/g2tgt) and
every `setGain2` call, tagged `A(skip)`/`B(oracle)` + `Timing::logicFrame`; the tool for correlating the
two SBS cores' CD-volume-ramp state directly instead of re-deriving it from the SPU-register write log —
see docs/findings/audio.md "pc_skip vs oracle: SPU register stream divergences").

Full-PSX (psx_fallback / SBS core-B) coroutine diagnostics (native_boot.cpp `ov_switch`): `sched` (coro
start/resume/out + task slot state) · `yieldpc` (per-yield `ra`/`r16`/`r29` + the stale-on-inner-frames
`waitloop` heuristic — prefer btyield) · `btyield` (at each coro yield: guest-stack scan AND a PRECISE
C-level `backtrace()` of the fiber thread = the recompiled `func_XXXX`/`ov_*_gen_*` call chain, so you can
see exactly which recomp function yields and read its callee-saved regs — the reliable tool for the
deep-field-coro freeze; see findings/sbs.md).

`dispatch` (engine_overrides.cpp) — logs every ENGINE-OVERRIDE hit: `[dispatch] fN core=A 0xADDR
Class::method ra a0-a3`. Fires from TWO places, both funneled through `EngineOverrides`: (1)
`run()`, called at the top of `rec_dispatch` for the CALLER-explicit path; (2) `traceHit()`, called
by a `shard_set_override`/`ov_<mod>_set_override` psx_fallback-gated trampoline BEFORE it invokes
the native handler — this is the path a DIRECT recompiler-emitted `func_<addr>(c)`/`ov_<mod>_
func_<addr>(c)` call takes (never touches `rec_dispatch`; see `docs/findings/tooling.md`
"EngineOverrides::register_ is BLIND to a direct substrate call"). A trampoline that skips
`traceHit()` is invisible here even while its native handler genuinely runs — always add the call
when writing a new dual-registered override (game/core/pc_scheduler.cpp is the reference).

`ovhit` — per-address override HIT COUNTS, dumped once at exit from TWO sources, both gated on
this one channel (2026-07-10 rework, docs/findings/animation.md "ovhit tooling caveat" RESOLVED):

1. `EngineOverrides::dumpHitCounts()` (engine_overrides.cpp) —
   `[ovhit] EngineOverrides hit counts (N registered): 0xADDR Class::method : count`, flagging any
   `: 0   <-- NEVER HIT (registered but unreached)`. Under SBS, prints as
   `A=<hits> B(gen)=<count>` — A is core A's real override-fire count; B(gen) is SBS core B's
   substrate-dispatch count for the SAME address (via `noteSubstrateDispatch`, cheap — no new
   per-address instrumentation in generated code), so parity (`A=1020 B(gen)=1020`) confirms the
   two cores reach the address the same number of times. A count MISMATCH (fires unequal) is
   flagged and is a real signal worth investigating (control-flow divergence between the ported
   native body and the substrate, OR the native body legitimately takes a different call count for
   a reason you should be able to name — do not treat it as noise).
2. `engine_override_thunk` (engine_override_thunk.cpp) — `[ovhit] engine_override_thunk (g_tab)
   hit counts (native=coreA / oracle=coreB): 0xADDR : native=N oracle=M`, covering every address
   wired ONLY through `engine_set_override_main`/`_a00` (PutDrawEnv 0x800815D0, DrawSync
   0x80080F6C, renderWalk 0x8003C048, the billboard/sequencer/font clusters, …) — these live in a
   COMPLETELY SEPARATE table from (1) and were previously invisible to `ovhit` even after the
   target-binding fix, because they were never registered into `EngineOverrides` at all (no
   `ov.register_` call for most of them). Merging this dump into the same channel makes `ovhit`
   the ONE place to check "did this override fire", regardless of which of the two override tables
   it's wired through.

Cheap enough (a counter bump, no per-hit I/O) to leave on for a full session, unlike the verbose
`dispatch` trace — this is the tool for "was this override EVER reached", the exact question
`recdep`/`dispatch` individually get wrong (each is blind to a different subset of call paths —
see the class comment in runtime/recomp/engine_overrides.h). A `0` here for an address you KNOW
should fire every frame (gameplay-critical scheduling, a hot AI leaf) is a real bug, not "not yet
exercised" — found + fixed live 2026-07-08: PcScheduler's 5 primitives (yieldPrim/spawnPrim/
spawnAndWait/forceClose/selfClose) were registered via `EngineOverrides::register_` only, but ALL
their real callers reach them as a direct `func_<addr>(c)` substrate call that bypasses both
`rec_dispatch` and `recdep` — `ovhit` (added alongside the fix) is what makes that class of gap
observable going forward instead of requiring a manual grep of `generated/shard_*.c` for
`func_<addr>(c)` call sites every time. See docs/findings/tooling.md for the full writeup,
including the SBS/DualCore/Selftest registration gap this also uncovered.

**Target-selection fix (2026-07-10):** `EngineOverrides::dumpHitCounts()` used to bind to whichever
Game's `EngineOverrides::register_` fired FIRST via a single static pointer — under SBS full,
`main()` ALSO unconditionally constructed and registered a THROWAWAY `Game` before dispatching to
the SBS/DualCore/Selftest harness (each of which builds its own separate Game(s)), and that
throwaway registered first every time, so the dump always printed an all-zero table that was never
actually driven — reading 0 for EVERY address including long-verified overrides. Fixed two ways:
(a) `main()` (runtime/recomp/boot.cpp) now only calls `register_engine_overrides()` on the plain
no-harness path, right before `game->stub.run(path)` — eliminating the throwaway registration at
the source instead of working around it; (b) the dump target is now a bounded registry of every
instance that DID register (the "sanctioned atexit exception" shape, same as the `recdep`/alloc-
trace hooks), picking the non-`psx_fallback` registrant as primary and its `sbs`-matched peer for
the B(gen) column — order-independent, no longer "first wins".

**A/B call-count MISMATCH triage (2026-07-10) — read before treating a `COUNT MISMATCH` as a bug**:
a first honest SBS-full run with `ovhit` surfaced ~15 addresses where A and B(gen)/oracle disagree,
sometimes wildly (`0x8003CDD8 native=0 oracle=37527`, `ActorTomba::frameTick A=2878 B(gen)=0`).
Every one triaged so far (docs/findings/tooling.md "ovhit A/B mismatch is often a call-GRAPH
asymmetry, not a counting bug") is a MEASUREMENT ARTIFACT of one of two shapes, NOT a functional
bug — confirmed via 0-diff SBS (every frame, not just the 30-frame print granularity) plus, for the
hardest case, a runtime probe (`sefprobe` below) rather than static reading alone:
  1. **Direct native-to-native call mirrors the guest's own direct intra-shard call.** A ported
     method calling its own already-ported sibling directly (`c->mRender->billboardEmit()`,
     `outerTransitionCommit()`'s `outerTransitionGate()` call, `entityLoop`'s `gt3(c)`/`gt4(c)`) is
     invisible to BOTH counters (bypasses `rec_dispatch` AND the `g_override[]` wrapper) on the
     NATIVE side, while the SUBSTRATE side's identical direct intra-shard call DOES hit the g_tab
     wrapper (since `shard_set_override` intercepts the wrapper regardless of caller) — giving a
     `native=0, oracle=N` shape that's provably correct: e.g. `billboardEmit`'s oracle count (22689)
     equals `billboardCompose1`(17007) + `billboardCompose2`(5682) EXACTLY, and
     `outerTransitionGate`'s oracle count equals `outerTransitionCommit`'s own count EXACTLY (it's
     the unconditional first statement in that method's body).
  2. **The two SBS cores reach a shared leaf via structurally different call GRAPHS.** Core A
     (pc_faithful) runs the ported native `Engine` per-frame driver (`frameStartTickFaithful`,
     `sceneEventFifoFaithful`, …), which explicitly `rec_dispatch`es to wired leaves
     (`ActorTomba::turnBiasCompute`, `Animation::advanceLinkChain`, …). Core B (recomp_path/oracle)
     reaches the SAME final RAM state via the PURE SUBSTRATE per-frame chain instead — proven by the
     `sefprobe` channel below: `Engine::sceneEventFifoFaithful` gets 0 core=B `ENTRY` hits over 200
     frames vs ~78 for core=A, despite 0-diff RAM every single frame. A leaf reached from BOTH graphs
     (e.g. `Animation::advanceLinkChain`, reached from a still-substrate a00-overlay caller on BOTH
     cores AND from `sceneEventFifoFaithful` on core A only) legitimately fires more often on A.
     Call-count parity is only a meaningful signal for a leaf reached via the EXACT SAME call-graph
     shape on both sides (Math functions, `billboardCompose1`/`2`, `ActorTomba::type7Interact`, …,
     which all show clean parity in this run) — for a leaf whose UPSTREAM caller chain is itself
     asymmetric between the ported driver and the pure substrate, a mismatch is EXPECTED.
  - **PcScheduler's small mismatches (spawnPrim/spawnAndWait/forceClose/selfClose, values 0-5)** are
    boot-time-only (frames <200) and shape (1) applies once more: `PSXPORT_DEBUG=dispatch` shows
    `PcScheduler::spawnPrim` fired once at f0 with `ra=DEAD0000` — a native-only boot bootstrap call
    with no oracle-side equivalent (`FUN_80050b08`'s task0 arming), not a per-frame gameplay signal.
  - **Do NOT "fix" this by adding `noteSubstrateDispatch`/`traceGenHit` calls inside a
    psx_fallback-gated trampoline's gen branch** — tried live during this triage and reverted: the
    gen branch is ALSO reached via `rec_dispatch`'s own `main_dispatch(c,addr)` fallthrough for the
    resident-module case (`generated/shard_disp.c main_dispatch` calls the SAME `func_<addr>(c)`
    wrapper `noteSubstrateDispatch` already ran ahead of), so an extra call inside the trampoline
    DOUBLE-COUNTS every rec_dispatch-routed invocation (confirmed: `PcScheduler::yieldPrim`, which
    matched exactly at A=2990/B=2990 before the change, went to B=5989 after — ~2×). The counters as
    they stand are individually complete for their OWN call shape; the mismatch is a real, structural
    caller-graph fact, not something to paper over.
  - **Verdict for this triage pass**: every mismatch found is CLASS 1 or CLASS 2 above (artifact),
    none is a real functional defect — 0-diff SBS confirmed per-frame through f3000 before and after.
    No code changes were needed to "fix" the counting (both attempted counter changes were reverted
    as wrong); the fix that stuck is this documentation + the permanent `sefprobe` reference probe.

`animstack` (overlay_router.cpp, `rec_dispatch`) — TEMP INVESTIGATION PROBE for `Animation::attach`
(0x80077C40): logs `[animstack] fN core=A/B r29=... ra=... a0=...` on every reach. Used 2026-07-08 to
disprove the assumption behind the (now-removed) `isDeadStackScratch` SBS exclusion — the probe
showed r29 is IDENTICAL between SBS core A and core B at every call, proving attach's guest-stack
frame COULD be mirrored (see game/object/animation.cpp, docs/findings/animation.md). Left in place
(cheap, single address-filtered branch) as the reference technique for the next "does this leaf's
r29 actually diverge before you refuse to mirror it" question.

`fadetrace` (screen_fade.cpp) — logs every native-path `ScreenFade::set` / `applyLeafCall` with the
mode+rgb (first-time-per-tuple backtrace). Pairs with `PSXPORT_DISPWATCH=0x8007E9C8` (which surfaces
every substrate fade dispatch with its guest stack). Together they show BOTH sides of the fade caller
graph — essential when a cutscene fadeout stays stuck black: silent `fadetrace` + active `dispwatch` =
the failing fade caller is a substrate SM handler that needs porting native. Issue #27. NOTE:
`ScreenFade` has no cross-frame hold — see docs/findings/render.md "ScreenFade held-latch permanent
black" for the removed heuristic and why.

`fadesites` — one line per native fade call site each time it fires (`Sop::fieldMode` case 1/3,
`Engine::fieldRun` case 10, `BgSceneTransitionSm::fadeRect`), tagged by site name + the raw ramp
counter. Use when `fadetrace` shows the RGB value oscillating/stuck and you need to know WHICH of the
several state machines sharing `c->screenFade` fired this frame (`fadetrace` alone dedupes by
(op,mode,rgb) so a repeat value from a different caller prints no backtrace).

`quadrtpt` (quad_rtpt_submit.cpp) — fires every ~512th `QuadRtptSubmit::submitQuad` call
(0x8003B320, the rope/flame per-quad RTPT+OT-link submitter): `[quadrtpt] submitQuad call#N`.
Sampled firing confirmation (every 512th call, not every call) for this `engine_set_override_main`-
wired leaf; `ovhit`'s `engine_override_thunk (g_tab)` dump (see above, 2026-07-10) now ALSO covers
this address with an exact per-call count — `quadrtpt` remains useful as a live/streaming signal,
`ovhit` for the definitive end-of-run count.

`ovgtgnd` (overlay_ground_gt3gt4.cpp) — fires every ~128th `OverlayGroundGt3Gt4::entityLoop` call
(0x801401B8, the ground/scene GT3/GT4 entity-loop walker): `[ovgtgnd] entityLoop call#N`. Same
"confirm it actually fires" role as `quadrtpt`, for the ov_a00_set_override-wired ground GT3/GT4
cluster (sibling of the field pair's `ovgt` channel).

`sefprobe` (game/core/engine.cpp, `Engine::sceneEventFifoFaithful`) — logs
`[sefprobe] fN core=A/B ENTRY st=<FIFO-state-byte>` on every entry. Added 2026-07-10 during the
`ovhit` A/B call-count triage (docs/findings/tooling.md "ovhit A/B mismatch is often a call-GRAPH
asymmetry, not a counting bug") to settle, with a runtime measurement rather than static reading,
whether a native `Engine` per-frame method genuinely never runs on SBS core B. It does: 0 core=B
ENTRY lines over a 200-frame run, vs ~78 for core=A, despite 0-diff RAM every frame — core B
(recomp_path/oracle) reaches the same final state via the pure-substrate per-frame chain instead.
Kept as the reference technique for the next "is this A-only/B-only ovhit count a real bug or an
expected native-driver-vs-substrate-chain asymmetry" question — same role `animstack` plays for
guest-stack-residency questions.
`combatnav` (sbs.cpp, `Nav::DONE`'s `PSXPORT_SBS_AUTONAV=combat` leg) — periodic progress print
(every 100 frames) of Tomba's G-block world position + the pad drive state (`repl_on`/`repl_hold`/
`repl_tap_n`) while the combat-coverage leg runs. The tool used to trace the leg live while building
it (2026-07-10, docs/findings/ai.md) — use it to confirm the leg is actually walking/jumping instead
of stuck against an obstacle in a future session.
`skiprv` (sbs.cpp, `Sbs::Impl::skipRendezvousReached`) — MODE=skip frame-alignment barrier trace:
logs `[sbs][rendezvous] f<N> waiting on '<label>': …` every 60 frames while a fork-site rendezvous
is stalled, and a `SETTLED after N frame(s)` line when the wait resolves. Cheap enough to leave on
for a whole `MODE=skip` run; the end-of-run `dumpRendezvousSites()` summary (checks/stalls/maxWait
per label) prints unconditionally under `MODE=skip` regardless of this channel. See docs/findings/
sbs.md "SKIP-mode frame alignment".
**`PSXPORT_DEBUG=chanA,chanB` env now works at launch** (seeded once in `cfg_dbg`, runtime/recomp/cfg.c) —
previously channels were ONLY settable via the REPL/debug-server `debug` command, so headless/SBS runs
couldn't enable one despite this doc claiming the env drove it. A later REPL `debug …` overrides the seed.

Render layer-isolation diags (value flags, `cfg_str`, gpu_native.cpp gpu_emit_rq_item) for "where did the
native world go?": `PSXPORT_ONLYWORLD=1` (emit ONLY RQ_WORLD), `PSXPORT_NOBG=1` (drop RQ_BACKGROUND),
`PSXPORT_NOHUD=1` (drop RQ_HUD). `PSXPORT_PRIMAT="x,y[,f0]"` gained an optional min-frame `f0` so the
6000-line cap isn't exhausted before the target scene (e.g. reach free-roam at f216 with `,400`). The
`primat-rq` line now also prints the barycentric-INTERPOLATED depth at the pixel (`interp_ord`/`D32`) so a
z-fight shows as two prims with near-equal interpolated D32.

Z-FIGHT diagnostics + fix knob (coplanar barrel/decoration surfaces; see docs/findings/render.md):
- `PSXPORT_ZFIGHT[=eps]` — auto z-fight FINDER (default eps 6e-5). SW-rasterizes opaque 3D-depth prims into
  a per-pixel top-2 D32 buffer and per frame reports fighting-pixel count, worst contesting prim pairs
  (node/color/depths/emit-order), paint-order stability raw-vs-biased (a U-sweep), and a heatmap PPM to
  `scratch/screenshots/zfight/heat_f<N>.ppm`. `PSXPORT_ZFIGHT_FRAME=<N>` gates it to frame ≥ N;
  `PSXPORT_ZFIGHT_BOX="x0,y0,x1,y1"` restricts the report to a display-coord region. (render_queue.cpp
  `RenderQueue::zfightScan`.) Pure host diagnostic, no guest write.
- `PSXPORT_ZBIAS=<f>` — tunes the SHIPPED paint-order depth-tiebreak unit (default 4e-7; 0 disables the
  tiebreak). The fix is ON by default (this is a magnitude knob, not a behavior A/B gate); larger values
  resolve more coplanar ties but risk overrunning genuine world depth separations (span = unit × prim
  count, capped at 1.5e-3). (gpu_gpu.cpp `gpu_zbias_unit`.)

`preseqobj` (per-object fps60 motion tracker, `RenderQueue::emitItem` in game/render/render_queue.cpp) —
when this channel is on AND a REPL `preseq <N>` present-sequence capture is armed, every render-queue emit
pass ALSO logs one line PER emitted RqItem to stderr:
`[preseqobj] p<presentIdx> key=<fps_key> layer=<layer> x=<xs0> y=<ys0> scene=<0|1>`. `presentIdx` is the
present frame the pass will dump (`gpu_gpu_preseq_present_index`), so lines are keyed to the exact present
(both fps60 present passes — the interpolated in-between AND the real frame — emit through here and log
under their own index). `key` is the object identity (fps_key: per-particle billboard address / node; 0 =
un-keyed 2D/HUD prim), `scene=1` marks a prim REBUILT by sceneNative at the interpolated midpoint (dense,
correct-by-construction terrain/mesh/backdrop the tracker does not per-object judge). Cost is zero outside
an armed capture (the present index is −1 → the `cfg_dbg` scan is skipped). Feed the log to
`tools/preseqobj_check.py` (the acceptance gate): it groups by object identity and flags any object present
in ≥6 consecutive presents that OSCILLATES (sign-alternating jitter) or STALL-STEPS (snaps every 2nd
present) — the two signatures of a badly-interpolated 60fps object — and prints a FAIL/PASS summary. This is
the instrument the operator runs to verify the fps60 per-object work; see docs/findings/render.md.

## Flags that kept their own var (they carry a VALUE, not just on/off)
These stay as `PSXPORT_*` (read via `cfg_int`/`cfg_str`) because they take a frame number, coords, path,
or level — they can't be a bare channel:
- **Renderer / mode:** `VK` (default on), `SW_GPU`, `VK_NODEPTH`, `VK_TRITEST`, `VK_HEADLESS`,
  `GPU_WINDOW`, `WINDOWED`, `IRES`, `WIDE`, `FPS60`, `FPS60_GATE`, `FPS60_SYNTH`, `NATIVE_DEPTH`,
  `SSAO` (+ `SSAO_STRENGTH`/`SSAO_RADIUS`/`SSAO_BIAS`/`SSAO_RANGE`/`SSAO_VIZ`), `LIGHT`
  (+ `LIGHT_DIR`="x,y,z"/`LIGHT_AMBIENT`/`LIGHT_DIFFUSE`; SSAO+LIGHT share one deferred pass, `SSAO_VIZ`
  =2 shows normals, =3 shows the lit factor), `UI` (Dear ImGui mod-toggle overlay, windowed only —
  toggle live: wide/ires/fps60/ssao/light; ` or F1 to hide; forces native-depth + deferred infra on so
  the toggles work live; seeds g_mods in mods.c), `ATTACH`, `PROJPROBE`,
  `CULL`/`CULL_FAR`/`CULL_FOV`, `*_RECOMP` (`OT_/LZ_/GEOM_/RECOMP_OBJWALK`), `TRANSPLANT`.
- **SDL_GPU renderer (gpu_gpu.cpp):** `GPU_TRACE` (per-present src-VRAM occupancy + sampled disp region +
  readback nonzero count), `GPU_DEBUG` (enable the SDL_GPU device validation layer — slows pipeline
  compile, can trip the boot watchdog; raise `WATCHDOG_BOOT` when using it), `GPU_SELFTEST` (headless
  renderer regression test: render a known VRAM pattern through the present pipeline into an offscreen
  RGBA8 target and assert orientation + 1555 unpack, then exit 0=PASS/1=FAIL — runs `tools/test_gpu_render.sh`;
  no disc needed). `VK_HEADLESS` (offscreen, no window) and `FULLSCREEN`/`WINDOWED` are honored unchanged.
- **Boot / automation:** `NO_FMV`, `NOAUDIO`, `NOPACE`, `NOSKIP`, `NATIVE_FRAMES`, `AUTO_GAMEPLAY`,
  `AUTO_NEWGAME`, `SCEA_SKIP`, `WATCHDOG`, `REPL`, `DEBUG_SERVER`, `T2_NOSEQTICK`, `FMV_*`, `FORCE_*`.
- **Paths:** `TOMBA2_DISC`, `TOMBA2_CARD`, `DISC`.
- **SBS observable mode:** `SBS_MODE=skip` — pc_skip (real default config, core A) vs recomp
  (oracle, core B), compared on a curated observable-state list (see skill `sbs-diverge`).
  **FRAME-ALIGNED, strict per-frame semantics (2026-07-10, docs/findings/sbs.md "SKIP-mode frame
  alignment"):** every collapsed-multi-step `pc_skip` fork (CLAUDE.md "The 5 paths") is meant to
  call `Sbs::skipRendezvousReached(c, addr, minVal, label)` (sbs.h) right after doing its own
  (host-only) shortcut work but BEFORE flipping any guest-visible "load complete" state — while the
  oracle core hasn't independently reached the same shared-layout completion field yet, the
  shortcut side idles (no state advance) instead of racing ahead at the same lockstep frame. A
  no-op (pass-through) outside `MODE=skip`, so this never affects `./run.sh` or any other SBS mode.
  A wait that never resolves in 3600 frames (60s @ 60fps) **aborts** with both sides' state — a
  loud diagnostic, not a hang. Because of this barrier the observable compare's old 60-frame
  "settled divergence" tolerance is GONE — `checkObservables` is now strict per-frame (first
  differing frame reports and, by default, aborts; `PSXPORT_SBS_SKIP_CONTINUE=1` demotes to
  log-and-continue for triage). `MODE=skip` also auto-arms the existing pane pixel-diff
  (`checkPaneDiff`, normally opt-in via `SBS_RENDERDIFF`) as the per-frame VISUAL compare — covers
  STRUCTURAL rendered-picture differences only, not audio or non-visual state. **Only ONE fork is
  actually wired so far** (`Engine::stage0AdvanceSkip`'s START.BIN-load gate, label
  `start_bin_load`) — the fork inventory entry in docs/findings/sbs.md lists which of the ~25
  `pc_skip` sites are genuine load-collapse rendezvous candidates (most are per-frame parity forks
  that don't need one) and which are still unwired; an unwired fork's downstream content can still
  legitimately drift and will now report as a real (unmasked) divergence instead of being silently
  tolerated — that is by design, not a regression.
- **SBS bounded clean exit:** `SBS_EXIT_FRAME=<n>` (`cfg_int`) — the SBS loop calls `exit(0)` once
  frame n is reached, so atexit dumps (engine_override_thunk per-address native/oracle hit counts,
  EngineOverrides `ovhit`) actually print. A `timeout`-killed gate dies via the watchdog's SIGTERM
  `_exit(130)`, which skips atexit — wiring passes need the hit counts to prove every registered
  address FIRED on both cores.
- **SBS combat-coverage leg:** `SBS_AUTONAV=combat` (raw `getenv`, `sbs.cpp`'s own local-static
  pattern, matching this file's other `SBS_*` knobs) — an OPT-IN extension of the standard
  `SBS_AUTONAV=1` Nav state machine (runtime/recomp/sbs.cpp): once player control is reached, hold
  Right + jump periodically to walk Tomba into the seaside `ActorZonedAttacker`/`cull_substate_
  orchestrator` encounter, exercising `ActorMeleeEngage::doIt` (0x80112188) and
  `MeleeProximity::isAtApproachAnchor` (0x8001F9DC) — combat-cluster overrides the STANDARD gate's
  autonav never reaches (docs/findings/ai.md). Stays off by default so the standard gate command is
  unaffected; opt in explicitly when working the combat/AI cluster. Pair with `PSXPORT_DEBUG=
  combatnav` (below) to watch it navigate.
- **SBS field-frame forcing hooks** (raw `getenv`, `sbs.cpp` local-static pattern; all fire ONCE,
  identically on BOTH cores, after AUTO-NAV reaches free-roam). These exist to defeat the HOLLOW GATE
  (docs/findings/sbs.md): in free-roam, pc_faithful dispatches the field-frame chain to substrate, so
  native field-frame code (e.g. `AreaSlots::updateTail` @0x80075A80) never runs and a "0-diff" gate
  proves nothing. Forcing sm[0x4c]=3 routes through fieldFrameX, which DOES run the native method.
  - `PSXPORT_SBS_FORCES4C=<frame>:<val>` — write `val` to the GAME area-machine halfword sm[0x4c] on
    both cores at `frame` (resets sm[0x4e]=0). `val=3` is the only path that runs native
    `updateTail`. **Forcing caveat:** the forced transition does NOT reproduce the natural area
    transition's caller setup (callee-saved r22/r23/r30 etc.), so a divergence under forcing can be a
    forcing ARTIFACT — only actionable once shown to be in the function's OWN footprint, not entry
    conditions (see the finding's MEASURED block). The honest gate for #37 still needs a REAL
    area/hut transition (blocked by the A0X overlay residency gap).
  - `PSXPORT_SBS_WARP=<frame>:<area>[:<sub>]` — write a door-record (0x800BF83A=(area<<8)|sub,
    0x800BF839=3) into both cores at `frame`, identical to native_boot's `warp` REPL — exercises the
    cross-area transition + `updateTail`'s spawn arm during the load. Currently recomp-MISSes at the
    destination overlay's object-init 0x8010B37C (the A0X residency gap above).
  - `PSXPORT_SBS_ARMSLOT=<frame>:<slotidx>` — arm slot `slotidx` (kind=0xFF) on both cores at `frame`
    so `updateTail`'s action arm (spawn leaf 0x80092660) runs on both; its spilled caller-regs land at
    0x801FE900.., the region #37 targets. The spilled values come from updateTail's register mirror,
    not the slot, so any armed slot exercises the fix.
  - Pair with `PSXPORT_DEBUG=asent` (updateTail ENTER: counter/loopEntered/area/r22/r23/r30 on entry)
    and `as37` (per action-arm spawn: slot/r16/r19) — ALWAYS require `[asent]` count>0 before trusting
    any 0-diff on a field-frame native port.
- **Mirror TDD gate:** `MIRROR_VERIFY` = `all` or `0xADDR[,0xADDR...]` — the strict per-function
  equivalence gate for pc_faithful native mirrors (game/core/verify_harness.h `strictCheck` +
  `MV_CHECK` fork-site macro). When armed for a wired guest address, each invocation runs the
  native mirror AND replays the pure substrate body (EngineOverrides suppressed = SBS core B)
  from the same pre-state, byte-compares RAM + scratchpad + ABI regs with NO exemptions, and
  ABORTS on any diff with per-byte native-vs-substrate attribution. Yield-free mirrors only
  (a yield inside a check leg aborts with a message); host hw side effects run twice while armed
  (do not arm CD-advancing leaves — SBS covers them). USER 2026-07-08: mirrors must be verified
  by RUNNING this gate, never by assertion.
  - **`MIRROR_VERIFY=all` is GENERALIZED (2026-07-10, `VerifyHarness::mirrorSampleGate`):** it no
    longer needs a hand-placed `MV_CHECK` at every call site. The two CENTRAL native-dispatch
    injection points — `engine_override_thunk` (`runtime/recomp/engine_override_thunk.cpp`, the
    `engine_set_override_main`/`_a00` g_tab[] table) and `EngineOverrides::run`
    (`runtime/recomp/engine_overrides.cpp`, the rec_dispatch registration table) — call
    `mirrorSampleGate(addr)` before invoking the native handler, so `=all` mechanically covers
    EVERY address wired through EITHER mechanism in one run. An existing `MV_CHECK`-wrapped site
    (e.g. sequencer.cpp's `nat_frameTick`) is unaffected: the outer `strictCheck` sets `inCheck`
    before calling the native leg, so the inner `MV_CHECK`'s own `strictArmed` sees `inCheck` and
    is a no-op pass-through — no double-check, no double cost. Nested wired calls during the
    substrate-replay leg stay pure-gen all the way down because `inSubstrateLeg` is a Game-level
    flag consulted by BOTH injection points (not stack-scoped per-call) — `EngineOverrides::run`
    also self-checks (`abort()`) if ever reached with `inSubstrateLeg` set, defending the exact
    fake-green bug class the sequencer wiring pass found and fixed (docs/findings/audio.md).
  - **`MIRROR_VERIFY_EVERY=N`** (`cfg_int`, default **1** = check every invocation — correctness
    over speed by default, per the "never silently narrow the compared state without a knob"
    rule) — per-ADDRESS sampling: only every Nth invocation of a given address is actually
    checked (still runs the real call every time; only the verify-and-compare machinery is
    skipped on the other N-1). Raise `EVERY` (e.g. 50-200) only if you specifically want to trade
    coverage for speed; since the 2026-07-10 fast path below, `EVERY=1` is fast enough for a
    normal multi-thousand-frame soak and is the default for a reason — coverage over speed.
  - **FAST PATH (2026-07-10, `strictCheck`'s write-journal): `=all EVERY=1` now covers a full
    session soak, not just a targeted single-address gate.** `strictCheck` used to snapshot AND
    linear-scan-compare the FULL 2 MB main-RAM buffer per invocation (2x `memcpy(0x200000)` +
    a 2 MB compare loop) — with a hot render-dispatch address firing 1000s of times/frame this
    made `=all EVERY=1` unable to finish 3000 headless free-roam frames in 100s (measured: only
    ~570 frames). Root cause: comparing the WHOLE 2 MB when a typical leaf touches a few dozen to
    a few hundred bytes. Fix: copy-on-first-write JOURNALING, not a narrower compare — every
    guest store to main RAM (scratchpad stays a full 1 KB compare; already cheap) funnels through
    `Core::mem_w8/16/32` (`runtime/recomp/mem.cpp`, the audited SOLE write path reachable during
    an armed check — `game/*.cpp` only ever READS `c->ram[]` directly; the two raw-write sites,
    `boot.cpp`/`native_stub.cpp`'s EXE-load `memcpy`, run before any check is ever armed). Those
    hooks are a no-op unless `VerifyHarness::journalIsArmed()` (a single bool `strictCheck` sets
    for the duration of each leg) — zero overhead when `MIRROR_VERIFY` is unset. `strictCheck`
    then snapshots/rewinds/compares only the UNION of both legs' touched bytes instead of all
    2 MB; anything neither leg wrote can't have diverged, so skipping it is a proof by
    construction, not a narrowing. Same compared-state definition as before (RAM + scratchpad +
    ABI regs), computed incrementally instead of by full scan. Measured (same 3000-frame headless
    free-roam scenario, `PSXPORT_AUTO_SKIP=1`): `=all EVERY=1` old full-scan path reached only
    ~570 frames in a 95 s window; the journal fast path finishes all 3000 frames in ~9.5 s (a
    >250x wall-clock speedup on this scenario) with baseline (`MIRROR_VERIFY` unset) unaffected —
    identical boot log, ~3.4 s for the same 3000 frames.
  - **`MIRROR_VERIFY_FULL=1`** (`cfg_on`) — forces the OLD full-2MB-snapshot/linear-scan path
    (`VerifyHarness::strictCheckFull`), kept as the authoritative slow reference the fast journal
    path is validated against. Validated 2026-07-10: same scenario run under both modes produces
    the same set of MISMATCH addresses/bytes/values and the same overall verdict per invocation
    (byte-level report ORDER can differ — fast reports in first-touch/insertion order, full
    reports in ascending-address order — but the same bytes/values are found, nothing is missed).
    A deliberate sabotage (`Sequencer::frameTick`'s guest-stack spill XORed with a known constant,
    reverted before commit) was caught identically by both modes at the exact same invocation with
    the exact same byte values, proving the journal isn't just replaying pre-existing matches.
    Use `MIRROR_VERIFY_FULL=1` to re-validate the fast path after touching `strictCheck`/
    `journalTrack`/`mem_w8`/`mem_w16`/`mem_w32`, or when you specifically distrust the journal for
    a new address (e.g. a suspected unhooked write path) and want the brute-force ground truth.
  - **`MIRROR_VERIFY_CONTINUE=1`** (`cfg_on`) — log-and-continue on a mismatch instead of
    `abort()`ing (execution proceeds from the NATIVE result either way — the gate never corrupts
    state, matched or not). Needed to survey `=all` across a whole session instead of dying at
    the first hit; default is still fail-fast abort (per CLAUDE.md) for a targeted verify.
  - On mismatch the abort/log message includes: address, invocation number (per-address counter),
    guest `sp`/`ra` at entry, and the first ~16 differing RAM/scratchpad bytes or ABI registers
    (addr, native value, substrate value) — enough to root-cause without re-running under a
    debugger.
- **Valued diagnostics (still named, gfx-debug.md documents them):** `SCENEDUMP`, `PROVAT`, `GTEPROBE`,
  `POLYDUMP`/`POLYAT`, `FADEDBG`, `SEMIDUMP`, `VK_SHOT`/`VK_SHOTSEQ`, `VK_DIFF`, `GPUTRACE`,
  `VRAMDUMP`/`VRAMDUMP_AT`, `RAMDUMP`/`RAMDUMP_FRAME`, `CLOBBERDUMP`, `CLUTWATCH`, `WWATCH`, `CW`/`CW_BT`,
  `XA_DBG` (level), `BGMDBG` (level), `GPU_DUMP`, `WAV`, `SS`, `SBS`.
  - `WWATCH` lines print the exact GPU frame number (`[wwatch] f<N> ...`) — do NOT bracket by the
    30-frame `[native_boot] frame` prints, that aliasing has produced false "X frames early"
    conclusions. `PSXPORT_WWATCH_BT=1` adds a host backtrace per hit (names the gen_func_*/native
    chain even where guest pc/ra are stale under native execution) plus a `[wwatch-regs]` line with
    a0-a3/s0-s7 — the fastest way to attribute a write when pc/ra lie.
  - `PSXPORT_DEBUG=script` — one line per ScriptInterp::step opcode dispatch (obj, cursor, opword,
    ret). Native-step configs only; the oracle steps scripts in the substrate and logs nothing.
- **Dual-core render-diff harness (`PSXPORT_DUALCORE=1`, class DualCore, dualcore.cpp):** `DC_N`
  (frames after gameplay-start, default 180, `cfg_int`), `DC_LO`/`DC_HI` (focused guest region
  base/end, default 0x800B0000..0x80110000, `cfg_str`, hex OK), `DC_ALL` (include the render-only
  packet-pool/OT regions in the report, `cfg_on`).

## Adding a new flag
- A new on/off **diagnostic** → add a `cfg_dbg("yourchan")` call; document the channel in the table above.
  Do NOT add a new `PSXPORT_*_DBG` var.
- A new **config/feature** toggle → `cfg_on("PSXPORT_YOUR_FLAG")`; a valued one → `cfg_int`/`cfg_str`.
- Then list it here. The goal is to keep the count down — prefer a channel over a new variable.

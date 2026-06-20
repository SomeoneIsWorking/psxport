# Tomba2Engine — PORT PROGRESS TRACKER (the single source of truth, boot → gameplay)

**This file is the authoritative, ORDERED map of the engine port. Read it FIRST every session; UPDATE it in
the same commit whenever you own a function or learn a boundary fact. Goal: port the engine top-to-bottom in
EXECUTION ORDER — boot → menu → gameplay → per-frame — owning every ENGINE function as it runs, keeping only
game CONTENT as PSX.** Don't cherry-pick; advance the FRONTIER (§ "CURRENT FRONTIER" below).

Pointers (detail lives there, STATUS lives HERE): RE map `docs/engine_re.md`; findings/dead-ends
`docs/journal.md`; driving/automation `docs/driving-the-game.md`; render `docs/render-arch.md`; the boundary
& directives `CLAUDE.md`.

## How to own a function (the LOOP — same every time)
1. **Find it in EXECUTION ORDER** (this file's spine). Take the first node marked ☐ TODO at the frontier.
2. **RE it:** `tools/disas.py 0x<addr>` (function-scoped — it stops at `jr ra`; `--mem` = just loads/stores
   with resolved addr+WIDTH). Note args (a0=r[4], a1=r[5], …), every mem read/write (addr+width), the return
   (v0=r[2]), and any `jal` (callee = keep-as-reference or own-too). Record the RE in `engine_re.md`.
3. **Classify** (see § BOUNDARY): ENGINE → reimplement PC-native in `engine/`. CONTENT → leave PSX. PLATFORM
   → native platform service in `runtime/recomp/`.
4. **Reimplement** as `ov_<name>(Core* c)` (ABI `void(Core*)`). Read args from `c->r[]`, do the work via
   `c->mem_r*/w*`, set `c->r[2]`. Register with `rec_set_override(0x<addr>, ov_<name>)` (or
   `rec_set_interp_override_auto` for OVERLAY fns, flushed on unload). Add the .cpp to BOTH `tools/build_port.sh`
   and `run.sh` SRC lists.
5. **GATE (mechanical, never visual):**
   - **CONTENT-INTERFACE fns (the guest RAM the still-PSX content/cull/render reads) MUST RAM-match.** A/B:
     build override-ON, dump RAM; toggle it OFF, dump; `cmp -l` → **0**. Recipe in § VERIFY.
   - **Pure ENGINE result fns:** gate on observable result (world built / picture / interface state), not a
     byte-match — BUT if any retained PSX code reads the output, it's a content interface → RAM-match.
   - **ALWAYS confirm the override actually FIRES** (a `cfg_dbg` log on entry). A faithful override registered
     where it's never invoked passes the gate while doing nothing (later-170 trap).
6. **Update THIS file** (status ☐→✅ + file/symbol) and `journal.md` (later-NNN), commit + push.

## VERIFY — the standard A/B RAM-diff gate (deterministic; copy-paste)
Idle field exercises boot+steady systems; the **free-roam MOTION scene** exercises motion (camera/anim) the
idle field can't (it's static, A==B). Build is deterministic, so two runs compare byte-exact.
```
# override ON (current build):
PSXPORT_RAMDUMP_FRAME=650 PSXPORT_RAMDUMP=scratch/bin/on.bin \
  PSXPORT_VK_HEADLESS=1 PSXPORT_AUTO_GAMEPLAY=1 PSXPORT_AUTO_SKIP=500 PSXPORT_AUTO_WALK=r \
  PSXPORT_NATIVE_FRAMES=655 PSXPORT_NOAUDIO=1 scratch/bin/tomba2_port
# toggle the registration OFF (comment it / env-guard), rebuild that one file, dump scratch/bin/off.bin, then:
cmp -l scratch/bin/on.bin scratch/bin/off.bin | wc -l      # MUST be 0
```
Also dump a second frame (e.g. 900) and the idle field (no AUTO_SKIP/WALK). Confirm FIRES via a `PSXPORT_DEBUG`
log. **If the function's OUTPUT is in SCRATCHPAD (0x1F8000xx) the main-RAM dump is BLIND to it** — use a
per-call comparator instead (pattern: `engine_camera.cpp` `ov_cam_rotbuild_verify`, `PSXPORT_DEBUG=camverify`):
snapshot scratchpad+regs, run native, capture writes, restore, run the recomp oracle (`rec_interp(addr)`),
compare. Self-test the harness (oracle-vs-oracle = 0) first. And ensure the scene actually EXERCISES the output
(an accumulator only moves while the player MOVES — a stopped scene is a degenerate, false A==B gate). **Beetle oracle** (`runtime/wide60rt`, `make -C runtime`) is the cross-check when there's no override-OFF
reference. Tools: `tools/disas.py`, `PSXPORT_WWATCH=<ka_lo>,<ka_hi>` (find who writes an addr; ka=addr|0x80000000),
`PSXPORT_DEBUG=state` (task slots — is a menu/task alive?), `PSXPORT_DEBUG=cam` (camera pos), render gate
`PSXPORT_PRIMDUMP=<frame>`. Gotchas: `docs/driving-the-game.md §0`.

## THE BOUNDARY (one line; full text in CLAUDE.md)
PSX keeps ONLY game CONTENT: per-character/enemy **AI & behavior**, **physics/collision response**, **level
data**, **quest/event/progression rules**. EVERYTHING else is PC-owned (engine): boot, menus, loading, scene/
world/object model, placement/spawn, camera, projection, **render ordering & the renderer**, save/load, 60fps.
The recomp body is the behavioral ORACLE for engine fns (reimplement, don't transcribe) and the live RUNTIME
for content fns (call it). Do NOT mimic PSX hardware (GTE/GP0/OT) — remove Beetle by porting its CALLERS.

---

# THE EXECUTION SPINE — boot → gameplay  (✅ owned · ◐ partial/staged · ☐ todo · ⬛ keep-PSX content)

## A. Boot / init  (MAIN.EXE resident)
- ✅ `FUN_800896E0` crt0 (SP/gp/heap/BSS) — platform, via `native_boot.cpp`.
- ✅ `FUN_80050b08` main = `ov_game_main` — `native_boot.cpp`.
- Init prefix of ov_game_main (classified in engine_re.md table):
  - ✅ platform stubs: CdInit, ResetGraph, SetDispMask, VSync, SPU/sound, DMA/pad — native platform services.
  - ✅ `FUN_80050a0c` frame-state init = `eng_init_framestate` (engine_init.cpp).
  - ✅ `FUN_800509b4` display + GTE projection = `eng_init_display` (engine_init.cpp).
  - ✅ `FUN_80050a80` camera init = `eng_init_camera` (engine_init.cpp).
  - ☐ `FUN_80075130` **font / text system init** (ENGINE — front-end UI dependency).
  - ☐ `FUN_800520e0` **engine subsystem init** (ENGINE — FUN_8007b328, DAT_800ecf4x, …).
  - ◐ `FUN_80051e00` scheduler task-table init (rc0'd at boot, native_boot.cpp:371) + `FUN_80051f14` register
    task 0. The native frame loop replaces the per-frame stepper.
- ✅ Native cooperative frame loop `native_scheduler_step` (replaces `FUN_80051e60`) — `native_boot.cpp`.
  Thread/critical-section primitives owned: ov_open/close/change_thread, ov_switch (0x80080880).

## B. Stage sequencer / loader  (task 0)
- ✅ `FUN_800499e8` task-0 stage sequencer = `ov_task0_boot` (engine_level.cpp) — resolves START.BIN.
- ✅ `FUN_80052078` stage transition = `ov_stage_transition` (engine_level.cpp) — restart task at new stage.
- ✅ `FUN_800450bc` level/overlay LOADER = `ov_load_stage`/`eng_load_stage` (engine_level.cpp, later-162).
- ✅ CD loadfile path native (ov_cd_loadfile 0x8001DB8C synchronous) — `cd_override.cpp`.

## C. Stages (task 0 cycles START → DEMO → GAME)
- ☐ **START.BIN** boot splash (SCEA etc.) — un-ported PSX boot path (EXPECTED-broken until ported; don't debug).
- ☐ **DEMO stage `0x801062E4`** = TITLE / front-end MENU state machine (8 substates) — **the big front-end
  engine system, INTERPRETED.** Title→New Game flow dr(driving-the-game.md). The first large un-owned system
  in execution order between boot and gameplay.
- GAME stage `0x8010637C` (overlay GAME.BIN, AUTO-registered in engine_stage.cpp via stage_scan_overlay):
  - ✅ top-level prologue = `ov_game_stage_main`; ✅ sm[0x48] handlers 0/1/2 (`ov_game_s48_0/1/2`);
    ✅ running dispatcher sm[0x48]==2 (coro-redirect handshake, later-169).
  - ◐ `ov_game_s4c` (sm[0x4c] area machine `0x80106478`) — STAGED, UNVERIFIED (never entered on field/boot;
    needs a real area transition sm[0x4a]==2; see § OPEN). NOT registered.

## D. Per-frame GAMEPLAY systems (inside the GAME stage loop)
- ✅ `FUN_800788ac` frame update = `ov_frame_update` (pad read + present + audio kick) — game_tomba2.cpp.
- ✅ `FUN_8007a904` object/entity WALK = `ov_objwalk` (engine_tomba2.cpp) — the per-frame object driver.
- ✅ `FUN_8007712c` per-object CULL / LOD = `ov_object_cull` (engine_tomba2.cpp).
- ✅ `FUN_80051C8C` per-object TRANSFORM build = `ov_build_xform`.
- **Camera update (engine_camera.cpp):**
  - ✅ position X/Z `FUN_8006d960` = `ov_cam_track_xz`; ✅ position Y `FUN_8006da54` = `ov_cam_track_y` (later-174).
  - ✅ rotation / LOOK-AT builder `FUN_8006e464` = `ov_cam_rotbuild` (engine_camera.cpp, later-175) — 2 jump
    tables + common look-at tail; owns control flow + arithmetic native, CALLS libgte rsin/rcos/ratan2/isqrt
    via rec_dispatch. Output 0x1f8000d0/d8 is SCRATCHPAD (main-RAM diff is blind) → gated by per-call
    comparator `PSXPORT_DEBUG=camverify` (native vs recomp oracle, 0-diff, d0 accumulating on motion scene).
  - ✅ DISTANCE / ZOOM solver `FUN_8006d2ac` = `ov_cam_dist_solve` (engine_camera.cpp, later-176) — first
    sub-fn of orchestrator FUN_8006e0f0. Picks a target (X,Z) (13-entry jump table on G[+0x164] + cam[+0x72]&2
    path), derives the camera→look-point planar distance (isqrt) + angular error (ratan2), smooths cam[+0x14]
    toward ±0x280000 (±65536/frame), accumulates into cam[+0x58], places camera planar pos cam[+0x08]/[+0x10].
    Outputs are MAIN-RAM cam fields → gated by per-call comparator `PSXPORT_DEBUG=camverify` (0-diff over 1800+
    calls on the motion scene). GOTCHA: the far/near branch's DELAY SLOT (slti angd,2048) reloads v0, so the
    NEAR-path final test NEGATES s0d when angd<2048 — a missed delay-slot effect (caught by the trig-spy diff).
  - ☐ remaining sub-fns of `FUN_8006e0f0`: 0x8006d654, 0x8006c80c, 0x8006dcf4, 0x8006d02c, 0x8006e010 (TODO).
  - ☐ per-MODE orchestrators `FUN_8006e0f0` / `FUN_8006e228` / `FUN_8006e3f4` (call the smoothers; multi-mode).
- ✅ Terrain `FUN_8002AB5C` = `ov_terrain` (native_terrain.cpp, later-158).
- ✅ Render submit: geom GT3/GT4/gt4_bp, per-object render `0x8003CCA4`, render walk `0x8003C048` — engine_submit.
- ✅ Render ORDERING: engine RenderQueue + packet-pool walk (OT read RETIRED, later-172). DrawOTag `0x80081560`
  = `ov_draw_otag`. Native projection `proj_native_vertex` (float matrices, real D32 depth).

## E. PLATFORM services (native; the "remove Beetle by porting callers" axis — NOT game logic)
- ✅ CD/disc (cdc_native, cd_override, disc), ✅ XA stream (xa_stream), ✅ libsnd BGM (ov_bgm_*) + SPU voice.
- ⬛ Still-linked Beetle: `gte.c` (GTE — used by the still-PSX cull NCLIP + content collision; goes when those
  callers are ported), `mdec.c` (FMV), `spu.c`. These vanish only by porting their CALLERS, never by re-emulating.

## ⬛ CONTENT — keep as recompiled PSX (do NOT reimplement)
Per-enemy AI / per-character behavior; physics & collision response (Tomba move/jump/land); level data payloads;
quest / event / progression / game-rule logic.

---

# CURRENT FRONTIER (work these, in this order)
**USER REDIRECT 2026-06-20 (later-177) — supersedes the camera items below.** Port top-down boot→gameplay;
the ASSET PIPELINE must be fully PC-owned so assets reconstruct with the emulator OFF. Acceptance test:
reconstruct the sea backdrop bit-identical.
**STATUS: the texture asset pipeline is now FULLY PC-OWNED and the acceptance test PASSES (later-177).**
Owned this session: the unpack post-step (libgs DrawSync dropped from the synchronous native upload path), the
per-group LOADER ORCHESTRATION `FUN_80044F58` → `ov_load_texgroup` (header+archive CD-load, unpack, metadata,
terminal yield), and the OFFLINE reconstructor `tools/tex_reconstruct.c` that rebuilds the seaside VRAM atlas
from raw compressed bytes with NO PSX — 99.95% bit-identical to live VRAM, residual = runtime-animated CLUTs only.
**REMAINING under this redirect:** (a) the on-screen 288×576 backdrop COMPOSITION (engine-owned 2D layer/sort,
ties into render-ownership, NOT the asset pipeline); (b) the title/menu BG render regression (background not
submitted — see later-177) before the menu image can be exported. (Camera sub-fns below are DEFERRED behind this.)


1. **Camera per-MODE orchestrators `FUN_8006e0f0` / `FUN_8006e228` / `FUN_8006e3f4`** — call the owned position
   smoothers + the owned look-at builder; a camera-mode selector calls one per frame. Finish the camera system.
   (The look-at builder `FUN_8006e464` and the dist/zoom solver `FUN_8006d2ac` are now owned — later-175/176.)
   Remaining `FUN_8006e0f0` sub-fns to own next: 0x8006d654, 0x8006c80c, 0x8006dcf4, 0x8006d02c, 0x8006e010,
   then collapse the orchestrator dispatch itself. Gate each scratchpad/cam-output fn with a camverify-style
   per-call comparator (copy `ov_cam_dist_solve_verify` / `ov_cam_rotbuild_verify`).
2. **DEMO / front-end MENU stage `0x801062E4`** — the big un-owned system in execution order between boot and
   gameplay. Title→New Game. Own its substate machine PC-native.
3. **Init-prefix remainder:** `FUN_80075130` font/text init, `FUN_800520e0` engine subsystem init.

# OPEN / BLOCKED (not on the critical path)
- **ov_game_s4c verification** needs an in-game AREA transition (sm[0x4a]==2). Free-roam IS reachable headless
  (AUTO_SKIP=500 + AUTO_WALK) but the seaside exit isn't a plain walk/jump (hut door blocked by a barrel) —
  needs visual steering or RE of the exit trigger in GAME.BIN handler `0x801088d8`. (NB: there is NO pause menu
  — the reliable `PSXPORT_DEBUG=state` task-slot probe confirms 0 menu tasks; an earlier "pause menu" claim was
  a broken probe.) Controls (which button = jump) are not yet identified — leave it; it reveals itself in play.

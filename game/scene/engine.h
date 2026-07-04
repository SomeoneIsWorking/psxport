// class Engine — the PC-native GAME/STAGE driver of Tomba! 2.
//
// PROPER OOP: one instance per Core (embedded as `Core::engine`). Callers use it as
// `c->engine.method()` — non-static instance methods, back-pointer to Core stored once at Core
// construction time (same pattern as `Core::screenFade`). No `extern "C"` shims.
//
// SCOPE: the GAME-stage state machine (sm[0x48] handlers) and its per-frame field driver — the
// top of the field spine that was previously exposed as the free functions `ov_game_stage_prologue`
// / `ov_game_frame` / `ov_field_frame` / `ov_field_frame_x` in engine_stage.cpp. Those free
// functions are the implementation; this class is the public interface (called by the scheduler
// and by the state-machine handlers themselves once fully migrated).
//
// Migration is progressive: methods are added here as pieces of engine_stage.cpp are class-ified.
// The existing static helpers in engine_stage.cpp keep working meanwhile — an Engine method may
// currently be a thin forwarder to a static ov_* function in that TU. As each is class-ified in
// place, the forwarder collapses.
#pragma once
#include "scene_transition.h"    // Engine owns the SceneTransition subsystem instance
#include "transition_state3.h"   // Engine owns the TransitionState3 walker instance
#include "object_list.h"         // Engine owns the ObjectList entity-list walkers
#include "array8_dispatch.h"     // Engine owns the Array8Dispatch fixed-array dispatcher
#include "object_table.h"        // Engine owns the ObjectTable 40-slot dispatcher
#include "demo.h"                // Engine owns the Demo front-end MENU stage machine
#include "sop.h"                 // Engine owns the Sop intro-cutscene field stage machine
#include "bg_scene_transition_sm.h"  // Engine owns the BG scene-transition fade manager
#include "world/pool.h"          // Engine owns the Pool per-area init subsystem
#include "world/placement.h"     // Engine owns the Placement field-object driver
#include "world/graphics_bind.h" // Engine owns the GraphicsBind object render-bind subsystem
#include "ui/font.h"             // Engine owns the Font boot-time init subsystem
#include "object/animation.h"    // Engine owns the Animation per-object VM stepper
#include "core/asset.h"          // Engine owns the Asset loader subsystem
#include "audio/music_coord.h"   // Engine owns the MusicCoord dialog↔music coordination
#include "player/collision.h"    // Engine owns the Collision grid-family subsystem
#include "render/cull.h"         // Engine owns the Cull visibility subsystem
#include "math/mathlib.h"        // Engine owns the Bit game-flag bitmap subsystem
#include "world/spawn.h"         // Engine owns the Spawn entity-spawn/despawn subsystem
#include "world/verify_gate.h"   // Engine owns the shared VerifyGate A/B diff helper
#include "object/behavior_dispatch.h" // Engine owns the per-object BehaviorDispatch subsystem
#include "scene/scene_events.h"       // Engine owns the SceneEvents arm subsystem (FUN_80040B48)
#include "audio/sfx.h"                // Engine owns the Sfx trigger subsystem (FUN_80074590)
#include "scene/script_interp.h"      // Engine owns the ScriptInterp cutscene-script dispatcher
class Core;

class Engine {
public:
  // Back-pointer set once by Core's constructor (same pattern as ScreenFade::core).
  Core* core = nullptr;

  // One-shot debug camera-teleport (REPL `tp X Y Z`). CutsceneCamera is instantiated per-call so
  // its own members can't persist across the set/consume boundary; live on Engine (per-Core) instead.
  // Consumed + cleared by CutsceneCamera::trackXZ. Was file-scope s_tp_pending / s_tp_x/y/z; cross-Core
  // shared meant an `@a tp X Y Z` teleported BOTH SBS cores (deglobalize 2026-07-03).
  bool     mCamTpPending = false;
  int32_t  mCamTpX = 0, mCamTpY = 0, mCamTpZ = 0;

  // Slip #3 fix (docs/findings/sbs.md): submode1 case 0 must yield ONCE mid-body to match the recomp
  // coro cadence — recomp's FUN_80044BD4 (spawn-and-yield) yields between FUN_8005245C and the fall
  // through into case 1. Native's transitionAreaLoad is synchronous, so we manually defer the fall
  // through by one tick using this per-Engine flag. Set at the end of case 0's load body; consumed
  // on the next tick to execute the case 1 body.
  bool     mSubmode1LoadDeferred = false;

  // ── Scene subsystem instances owned by Engine ─────────────────────────────────────
  // Callers reach them as `c->engine.sceneTransition.method(args)`.
  SceneTransition sceneTransition;   // area-mask trigger + sub-scene swap handshake
  TransitionState3 transitionState3;  // mid-transition entity walker (guest FUN_8007B04C)
  ObjectList       objectList;        // per-frame entity-list walkers (guest FUN_8007A904 / FUN_80069B28)
  Array8Dispatch   array8Dispatch;    // 8-slot fixed array dispatcher   (guest FUN_80026368)
  ObjectTable      objectTable;       // 40-slot fixed object table       (guest FUN_80026C88)
  Demo             demo;              // front-end DEMO / MENU stage machine (docs/engine_re.md)
  Sop              sop;               // SOP intro-cutscene FIELD stage machine (guest 0x80109450)
  BgSceneTransitionSm bgSceneTransitionSm;  // BG scene-transition fade manager (guest FUN_8002655C)
  Pool             pool;              // per-area object-pool + control-block init (world subsystem)
  Placement        placement;         // field object-placement driver (guest FUN_80072A78/DDC)
  GraphicsBind     graphicsBind;      // per-object render-bind subsystem (guest FUN_8007AAE8 et al.)
  Font             font;              // boot-time font / text init subsystem (guest FUN_80075130)
  Animation        animation;         // per-object animation-VM stepper       (guest FUN_80076D68)
  Asset            asset;              // asset loader — LZ + texgroup + VRAM upload + boot preload
  MusicCoord       musicCoord;         // dialog ↔ ingame-music coordination (instant-CD-safe PC mod)
  Collision        collision;          // collision-grid family (list-scan + grid setup/query/resolve/step)
  Bit              bit;                // game progress-flag bitmap bit-test (FUN_8004D7EC / D868)
  Spawn            spawn;              // entity spawn/despawn dispatcher (FUN_8007A980 / A624 / 3116C)
  VerifyGate       verifyGate;         // A/B snapshot-rollback + diff harness (diag only)
  BehaviorDispatch behaviors;          // per-object handler dispatch registry (50 native behaviors)
  Cull             cull;               // per-object visibility cull / margin re-include (orphaned)
  SceneEvents      sceneEvents;        // field-wide scene-event arm primitive (FUN_80040B48)
  Sfx              sfx;                 // sound-FX trigger dispatcher (FUN_80074590)
  ScriptInterp     script;              // cutscene bytecode dispatcher (FUN_80041098 et al.)

  // ── GAME-stage entry points (called by the scheduler each frame) ────────────────────────────
  // stagePrologue: one-time prologue that runs when the GAME task enters — task-slot setup, first
  //   field-mode load, etc. (was `ov_game_stage_prologue`).
  // frame: one frame of the GAME stage — the sm[0x48] dispatcher + tail. Returns 0 if the current
  //   sm[0x48] state isn't owned natively yet (scheduler falls back to substrate); non-zero if
  //   handled here (was `ov_game_frame`).
  void stagePrologue();
  int  frame();
  // stageMain: OLD guest-loop entry (prologue + coro-redirect into 0x801063F4). Kept as reference /
  // fallback (native per-frame path calls stagePrologue + frame directly). Was ov_game_stage_main.
  void stageMain();

  // submode0 / submode1: the two sm[0x4a] state handlers under the GAME running sub-mode
  // (sm[0x48]==2). submode0 = SOP intro; submode1 = the walkable field area machine
  // (0x801088D8). Called by frame() when the corresponding sm[0x4a] is owned natively.
  // Formerly `ov_game_submode0` / `ov_game_submode1` free functions in engine_stage.cpp.
  void submode0();
  void submode1();

  // sm[0x48] state handlers (0=area INIT, 1=area RESUME-INIT, 2=RUNNING dispatcher) and the
  // sm[0x4c] area LOAD/TRANSITION machine. Formerly `ov_game_s48_0..2` / `ov_game_s4c` /
  // `ov_game_s48_2_frame` free statics in engine_stage.cpp. All operate on the sm task-state
  // block at *0x1F800138 that Engine already owns.
  void s48_0();
  void s48_1();
  void s48_2();
  void s4c();
  void s48_2_frame();

  // fieldFrame / fieldFrameX: the FIELD per-frame update body (guest 0x80108B0C) and its
  // mid-transition twin (0x80108BE4, sm[0x4a]==5 running-during-fade variant). Called by the
  // sm[0x4c] handlers and the transition drivers. Formerly ov_field_frame / ov_field_frame_x.
  void fieldFrame();
  void fieldFrameX();

  // fieldTransition + its 4 workers: the sm[0x4a]==5 sub-scene / door / area FADE transition
  // machine (guest FUN_80108A60 + FUN_80107xxx workers). fieldTransition dispatches on sm[0x4c]
  // into one of the 4 workers or the "done -> return to field" epilogue. Formerly the
  // ov_field_transition / ov_transition_main / ov_transition_d3c / ov_transition_e20 /
  // ov_transition_f3c free statics in engine_stage.cpp.
  void fieldTransition();
  void transitionMain();
  void transitionD3c();
  void transitionE20();
  void transitionF3c();

  // fieldRun / fieldRunX: the sm[0x4c]==2 field RUNNING sub-machine on sm[0x4e] (guest
  // FUN_80106B98) and its mid-transition twin (0x801070B4, sm[0x4c]==3). Formerly
  // ov_field_run / ov_field_run_x. Called by Engine::submode1 / fieldFrameX.
  void fieldRun();
  void fieldRunX();

  // submitPage810c: the sm[task+0x6b]==1 page-1 (pause-menu dim) fade branch of the master submit
  // dispatcher at guest 0x8010810C. Owns just the dim-fade shape (subtractive #808080 held on
  // page-1) + a substrate dispatch to the still-unowned menu draw at 0x801084F8; other pages
  // fall through to substrate 0x8010810C. Was ov_game_submit_810c in engine_stage.cpp.
  void submitPage810c();

  // fadeSequencer: the GAME-overlay a0l per-node screen-fade sequencer (guest FUN_8010957C).
  // Runs the multi-step ramp state machine at node+2 / node+3 / node+106 (level). Called by
  // fieldRun's sm[0x4e]==0xb branch with node = 0x800E8008. Was ov_scene_fade_seq.
  void fadeSequencer(uint32_t node);

  // frameUpdate: per-frame engine tick — the PC-driven game loop's frame body called directly
  //   from native_step_frame (native_boot.cpp). Runs the still-PSX per-frame update leaf, then
  //   owns the per-vblank audio (sequencer tick + SPU field advance), fps60 commit, and present
  //   + pace. Was the free function `ov_frame_update` in game_tomba2.cpp.
  void frameUpdate();

  // drawOTag: PC-native DrawOTag (libgpu FUN_80081560 equivalent) — the per-frame draw kick.
  //   Called directly from native_step_frame (top-down PC-driven, NOT an override). Owns the
  //   engine's decoupled render path: for the FIELD stage builds the world natively via
  //   Render::sceneNative (real depth); walks the guest OT for un-owned 2D/HUD prims (queued
  //   into the engine render queue); flushes the queue in engine order via rq_flush. Takes the
  //   OT head as an explicit parameter (taxi-parameter c->r[4] retired). Was the free function
  //   `ov_draw_otag` in game_tomba2.cpp.
  void drawOTag(uint32_t otHead);

  // startBinStage: task-0's START.BIN file-table builder — dispatcher between the two forks below.
  // On mPcSkip=false (pc_faithful) → startBinStageFaithful (byte-exact port of substrate 0x8010649C).
  // On mPcSkip=true  (pc_skip)     → startBinStageSkip     (collapsed PC path).
  void startBinStage();

  // startBinStageFaithful: byte-exact native port of substrate 0x8010649C. Reproduces every
  // guest-observable write in the same order so SBS byte-matches core B by construction. This
  // means: descend guest sp by 456 (matches substrate prologue), build the RECT on the guest
  // stack at sp+400, dispatch FUN_80081218 (LoadImage) so its libgs fn-ptr chain writes the
  // graphics-context state at 0x800AC5xx-0x800AC6xx, DrawSync, walk the three CdSearchFile
  // filename tables + XA singletons, restore sp, advance sm[0x48]=1, RNG-stamp task+0x56, spawn
  // task-1 via native_task_spawn (port of FUN_80051F14). Task-1's substrate wake at 0x80044F58
  // runs via the Coro-fiber stanza.
  void startBinStageFaithful();

  // startBinStageSkip: collapsed shortcut path for normal PC play. Skips PSX-only quirks: no
  // guest-sp descent, no libgs LoadImage substrate dispatch (uses native VRAM upload instead),
  // no task-1 spawn (asset.preloadTexgroup runs inline synchronously), uses native ISO9660 for
  // file lookups instead of libcd. Byte-diverges from substrate but works fine at runtime — the
  // resulting game state is equivalent to what the substrate produces on hardware.
  void startBinStageSkip();

  // stage0Advance: run ONE step of the native STAGE-0 preload state machine, matching the recomp
  // body of 0x8010649C's per-iteration yield loop (see docs/findings/sbs.md Slip #1). Called by
  // the scheduler on each subsequent tick after startBinStage() ran the file-table build (which
  // consumes step 0). Steps 1..5 spread preloadTexgroup + preloadStage1 + swap-to-DEMO across
  // ticks so A's cadence matches B's coro path. The FINAL step (5) calls native_start_stage(1)
  // which rewrites task+0xc to DEMO and yields; steps 1..4 each `scheduler_yield` to end the tick.
  // Returns 1 while more steps remain, 0 when the swap has landed.
  int stage0Advance(uint8_t& step);


  // task0Bootstrap: the boot-init entry that (a) resolves \BIN\START.BIN natively via disc_find_file,
  // (b) records its {LBA,size} into 0x800be1e0, and (c) enters stage 0 via startStage(0). Called once
  // from native_boot.cpp's game_init (was `native_task0_bootstrap(Core*)`).
  void task0Bootstrap();

  // startStage(stage): FUN_80052078 — switch task 0 to the given stage. Loads the stage overlay
  // (native_load_overlay), sets task state=3, hits the three BIOS EnterCS/B0F-reset/ExitCS leaves,
  // then yields the scheduler. Public wrapper (was the free fn `demo_start_stage(Core*, uint32_t)`)
  // plus the previous file-scope `native_start_stage` helper. Also the tail of task0Bootstrap +
  // stage0Advance's final step.
  void startStage(uint32_t stage);

  // ── ov_field_frame direct children (progressive class-ification) ──────────────────────────
  // areaModeDispatch: the 22-way area-mode jump-table dispatcher at guest 0x8001CAC0. Reads the
  //   area RENDER-MODE byte at 0x800BF870 and dispatches to the overlay handler that owns that
  //   mode (each mode = an entry in the resident table at 0x80010000; 10 of 22 slots are the
  //   "no-op default" stub 0x8001CB98, the other 12 jal one specific overlay leaf then return).
  //   Replaces `d0(c, 0x8001cac0u)` in the field-frame body.
  void areaModeDispatch();

  // sceneEventFifo: the field EVENT/COMMAND-QUEUE state machine at guest 0x80025588 (struct
  //   @0x800ed058). 3-state top switch on base[2]: state 0 arms + falls through to state 1
  //   (active body drains a small FIFO); state >=2 no-op. Was `d0(c, 0x80025588)`.
  void sceneEventFifo();

  // sceneRenderListBuilder: 2-phase scene/render-list builder driver at guest 0x8004FE84 (struct
  //   @0x800bf548). Phase 0 arms it (snapshot list ptr 0x800ecf64 into base+0x2b0..0x2b8, advance
  //   phase to 1); phase 1 dispatches a sub-state handler (base[1] 0..3 -> distinct overlays);
  //   flag @0x800bf822 bit 0 latched from (base[1]!=0 || base[0x0a]!=0). Was `d0(c, 0x8004fe84)`.
  void sceneRenderListBuilder();

  // sceneStateStep: the per-frame area SCENE-INIT / SCENE-RUN state machine at guest 0x80050DE4.
  //   Two overlay-handler tables of 22 entries each (indexed by the same render-mode byte
  //   0x800BF870 that areaModeDispatch uses), plus a scene-phase byte at 0x800F2418:
  //     phase == 0 -> INIT: call the INIT handler for the current area mode, set phase=1.
  //     phase == 1 -> RUN:  call the RUN handler for the current area mode (per-frame update).
  //     phase < 0 or >= 2 -> no-op.
  //   Both handler tables extracted verbatim from MAIN.EXE .text (@0x80015A40 init, @0x80015A98 run,
  //   21 overlay leaves + 1 default no-op each). Handlers take a0 = 0x800F2418 (the scene-state
  //   base). Replaces `d0(c, 0x80050de4u)` in the field-frame body.
  void sceneStateStep();

  // modePerFrameDispatch: the second per-frame render-mode dispatcher at guest 0x80022A80 (ov_field_frame
  //   calls it right after the object dispatcher). Keyed by the same render-mode byte 0x800BF870;
  //   reads a 24-entry function-pointer table at 0x8009D1D4 (MAIN.EXE .rodata, one entry per area
  //   mode, all pointing at overlay leaves in the 0x8010xxxx..0x80117xxx range that the currently-
  //   loaded overlay owns). Mode 3 (A00 fisherman village) is explicitly SKIPPED before the table
  //   read — the only special case. Replaces `d0(c, 0x80022a80u)` in the field-frame body.
  void modePerFrameDispatch();

  // postRenderTick: small 3-state machine on byte 0x800BF842 at guest 0x80077D8C, called after the
  //   per-frame render submit. `b42 & 0x7F` selects: 1 = trigger FX 41 then set b42=0x87; 2 = trigger
  //   FX 42 then clear b42; otherwise = decrement b42. Trigger call = FUN_80074590(id, 2, -65) — a
  //   sound/vibration fx queue leaf, still substrate. Replaces `d0(c, 0x80077d8cu)` in ov_field_frame.
  void postRenderTick();

  // frameStartTick: the first call in ov_field_frame's gameplay block (guest 0x80059D28). A per-frame
  //   prologue that (a) decrements the frame counter at 0x800BF819 and masks two 12-bit heading fields
  //   when it fires, (b) zeroes a bank of frame-scoped flags at G+0x177..0x17B and 0x1F80027A,
  //   (c) increments the per-frame stamp at 0x1F800247, (d) if 0x800BF841 == 0 dispatches a mode-keyed
  //   per-frame handler (modes 2/3/7/20 -> overlay-specific, else 0x8005950C) with G as a0 and clears
  //   0x1F800230, (e) seeds the master position + heading (G+0x2E/0x32/0x36/0x56/0x58) into scratchpad
  //   0x1F800160..0x1F80016A for the projection to read, (f) latches 0x800BF81E=1 when 0x800BF9C3&0x80,
  //   (g) ticks the sub-counter at G+0x180 when 0x1F800137 (pause flag) is 0, (h) advances the LFSR
  //   rand at 0x8009A450 every frame. Replaces `d0(c, 0x80059d28u)` in ov_field_frame.
  void frameStartTick();

  // areaUpdateTail: the last direct child of ov_field_frame's gameplay block — per-frame area-slot
  //   state machine at guest 0x80075A80. Iterates a 24-entry × 12-byte slot table at 0x800BE238
  //   keyed by the counter at 0x800BED78; per slot the kind byte drives one of three arms:
  //     kind == 0    -> skip to the buf[slot] post-check (nothing to do this frame).
  //     kind == 0xFF -> fire the action leaf FUN_80092660(slot_s16, hword_g, sub1, sub2, [3 stacked
  //                     bytes]) using either the g_a4f7e hword (top bit of sub2 set, arm-hi) or
  //                     the 0xBED84 hword (arm-lo), clear the "armed" bit in the 24-bit mask at
  //                     0x800BE358, then decrement kind.
  //     other        -> if slot[7] == 4, decrement kind; if it went to 0, SET the bit in 0x800BE358
  //                     and zero slot[1]/slot[2]. Else just decrement kind.
  //   Then a per-slot post-check reads buf[slot] filled by FUN_800998e4 at entry (0=zero slot[1];
  //   3=fall through unchanged; other=fall through). Tail: if 0x800BE358 nonzero call FUN_80098F90(0)
  //   + clear it; then FUN_80075824(0x800BE1F8), FUN_80099490(0x800BE1F8); if key2 = mem_r16s(0x800BED80)
  //   != -1, clear 0x800BE1F8, look up hword table[key2].w0 at 0x800BE368+key2*8, call FUN_8008E0C0
  //   with that hword and 0 — if it returns nonzero on the low16, fall to epilogue; else read the
  //   sub-object id at 0x800BE22A, if zero call FUN_80074E48 else call FUN_80074BF8(id) and clear
  //   both 0x800BED80 and 0x800BE22A. All 8 callees stay substrate. Replaces `d0(c, 0x80075a80u)`.
  void areaUpdateTail();

  // areaSlotAckIfMatch(arg): FUN_80074AF0 — SIGNATURE-MATCHED slot ack primitive against the same
  // 24-entry × 12-byte slot table at 0x800BE238 that areaUpdateTail iterates. The `arg` carries the
  // entry index in its low byte (arg & 0xFF) plus a 3-byte SIGNATURE in the high bytes; if the high
  // 3 bytes match the u32 stored at slot[idx].w0, this method sets the "armed" bit at
  // *(u32)0x800BE358 |= (1 << idx) AND clears the trigger-pending byte at slot[idx].b1.
  // Mismatched signatures are a no-op. RE'd from disas 0x80074AF0..0x80074B40.
  void areaSlotAckIfMatch(uint32_t arg);

  // ── Boot-time INIT (called from native_boot.cpp before the scheduler starts) ──────────────────
  // The engine's own PC-native init prefix (was the free functions `eng_init_*` in engine_init.cpp).
  // Each method reimplements the corresponding guest init leaf: frame/display/camera state, the
  // entity pool control block, the allocator + dispatch table, mode ctrl, input, and the orchestrator
  // that sequences them. Called via c->engine.initX() from native_boot.
  void initFrameState();               // FUN_80050A0C — vblank + double-buffer pacing state
  void initDisplay();                  // FUN_800509B4 — GTE projection CRs + display H
  void initCamera();                   // FUN_80050A80 — camera scratchpad matrices + state
  void initEntityPool();               // FUN_8007B328 — entity-pool control block + fixed-pt scales
  void initAlloc(uint32_t s1, uint32_t s2);  // FUN_80088B00 — allocator + 6-entry dispatch table
  void initInput();                    // FUN_80087A60 → 80086970 — input subsystem
  void initSubsystems();               // FUN_800520E0 — orchestrator (entity pool + alloc + mode + input)
};

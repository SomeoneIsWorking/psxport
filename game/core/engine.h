// class Engine — the PC-native GAME/STAGE driver of Tomba! 2.
//
// PROPER OOP: one instance per Core (embedded as `Core::engine`). Callers use it as
// `eng(c).method()` — non-static instance methods, back-pointer to Core stored once at Core
// construction time (same pattern as `Core::screenFade`). No `extern "C"` shims.
//
// SCOPE: the GAME-stage state machine (sm[0x48] handlers) and its per-frame field driver — the
// top of the field spine that was previously exposed as the free functions `ov_game_stage_prologue`
// / `ov_game_frame` / `ov_field_frame` / `ov_field_frame_x` in engine.cpp. Those free
// functions are the implementation; this class is the public interface (called by the scheduler
// and by the state-machine handlers themselves once fully migrated).
//
// Migration is progressive: methods are added here as pieces of engine.cpp are class-ified.
// The existing static helpers in engine.cpp keep working meanwhile — an Engine method may
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
#include "parallax_bg.h"         // Engine owns the SOP parallax-BG state machine
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
#include "object/behavior_dispatch.h" // Engine owns the per-object BehaviorDispatch subsystem
#include "scene/scene_events.h"       // Engine owns the SceneEvents arm subsystem (FUN_80040B48)
#include "audio/sfx.h"                // Engine owns the Sfx trigger subsystem (FUN_80074590)
#include "audio/audio_dispatch.h"     // Engine owns the AudioDispatch dispatch/settle cluster
#include "world/area_slots.h"         // Engine owns the AreaSlots slot-table state machine
#include "scene/mode_state_arm.h"     // Engine owns the ModeStateArm arm-primitive pair
#include "scene/script_interp.h"      // Engine owns the ScriptInterp cutscene-script dispatcher
#include "player/actor_tomba.h"       // Engine owns Tomba's per-frame logic + growth/movement
#include "ai/attack_orbit_substate.h" // Engine owns the A00-overlay AttackOrbitSubstate sub-behaviors
#include "ai/release_trigger_motion.h" // Engine owns the release-trigger sub-motion cluster
#include "ai/actor_melee_engage.h"     // Engine owns the ActorMeleeEngage AI leaf (FUN_80112188)
#include "ai/melee_proximity.h"        // Engine owns the MeleeProximity AI leaf (FUN_8001F9DC)
#include "audio/sequencer.h"            // Engine owns the Sequencer libsnd tick wrapper (FUN_800909C0, wide-RE draft, unwired)
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
  // Arm / disarm the one-shot teleport (REPL `tp X Y Z` / `tp`). Consumed by CutsceneCamera::trackXZ.
  void camTeleport(int x, int y, int z) { mCamTpX = x; mCamTpY = y; mCamTpZ = z; mCamTpPending = true; }
  void camTeleportOff() { mCamTpPending = false; }

  // `debug stage` change-detector for the RUNNING dispatcher's sm[0x4a]/sm[0x4c] log line (was a
  // function-local static pair in Engine::s48_2 — per-Core so SBS's two cores log independently).
  uint16_t mLast4a = 0xffff, mLast4c = 0xffff;

  // ── Scene subsystem instances owned by Engine ─────────────────────────────────────
  // Callers reach them as `eng(c).sceneTransition.method(args)`.
  SceneTransition sceneTransition;   // area-mask trigger + sub-scene swap handshake
  TransitionState3 transitionState3;  // mid-transition entity walker (guest FUN_8007B04C)
  ObjectList       objectList;        // per-frame entity-list walkers (guest FUN_8007A904 / FUN_80069B28)
  Array8Dispatch   array8Dispatch;    // 8-slot fixed array dispatcher   (guest FUN_80026368)
  ObjectTable      objectTable;       // 40-slot fixed object table       (guest FUN_80026C88)
  Demo             demo;              // front-end DEMO / MENU stage machine (docs/engine_re.md)
  Sop              sop;               // SOP intro-cutscene FIELD stage machine (guest 0x80109450)
  BgSceneTransitionSm bgSceneTransitionSm;  // BG scene-transition fade manager (guest FUN_8002655C)
  ParallaxBg       parallaxBg;        // SOP parallax-BG state machine (guest FUN_8010BFFC)
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
  BehaviorDispatch behaviors;          // per-object handler dispatch registry (50 native behaviors)
  Cull             cull;               // per-object visibility cull / margin re-include (orphaned)
  SceneEvents      sceneEvents;        // field-wide scene-event arm primitive (FUN_80040B48)
  Sfx              sfx;                 // sound-FX trigger dispatcher (FUN_80074590)
  AudioDispatch    audioDispatch;       // field-audio dispatch/settle cluster (FUN_800750D8 et al.)
  Sequencer        sequencer;            // libsnd per-VBlank tick wrapper (FUN_800909C0, wide-RE draft, unwired)
  AreaSlots        areaSlots;           // area-slot table state machine (FUN_80075A80 / FUN_80074AF0)
  ModeStateArm     modeStateArm;        // mode-state arm primitive pair (FUN_8005082C / FUN_800508A8)
  ScriptInterp     script;              // cutscene bytecode dispatcher (FUN_80041098 et al.)
  ActorTomba       actorTomba;          // Tomba's per-frame logic + growth/movement over G block
  AttackOrbitSubstate attackOrbit;      // A00 overlay: node[3]==0x80/0x81 sub-behaviors (FUN_80145AF0/801458E0)
  ReleaseTriggerMotion releaseTriggerMotion; // release-trigger sub-motion cluster (FUN_80123E9C family)
  ActorMeleeEngage actorMeleeEngage;    // A00-overlay melee-engage/reposition/arm leaf (FUN_80112188)
  MeleeProximity   meleeProximity;      // melee-proximity/approach-anchor leaf (FUN_8001F9DC)

  // ── GAME-stage entry points (called by the scheduler each frame) ────────────────────────────
  // stagePrologue: one-time prologue that runs when the GAME task enters — task-slot setup, first
  //   field-mode load, etc. (was `ov_game_stage_prologue`).
  // frame: one frame of the GAME stage — the sm[0x48] dispatcher + tail. Returns 0 if the current
  //   sm[0x48] state isn't owned natively yet (scheduler falls back to substrate); non-zero if
  //   handled here (was `ov_game_frame`).
  void stagePrologue();
  int  frame();
  // stageBodyFaithful: the whole ov_game_gen_8010637C arc as a native task body on a PcScheduler
  // fiber (faithful-execution model, same shape as Demo::stageBodyFaithful): stagePrologue (already
  // guest-frame-faithful), then the substate loop — frame() for natively-owned sm[0x48] states,
  // the guest handler dispatched for unowned ones, substrate tail yield each iteration. pc_skip
  // keeps the per-tick stagePrologue()/frame() pair via runGameStanza.
  void stageBodyFaithful();
  // stageMain: OLD guest-loop entry (prologue + coro-redirect into 0x801063F4). Kept as reference /
  // fallback (native per-frame path calls stagePrologue + frame directly). Was ov_game_stage_main.
  void stageMain();

  // submode0 / submode1: the two sm[0x4a] state handlers under the GAME running sub-mode
  // (sm[0x48]==2). submode0 = SOP intro; submode1 = the walkable field area machine
  // (0x801088D8). Called by frame() when the corresponding sm[0x4a] is owned natively.
  // Formerly `ov_game_submode0` / `ov_game_submode1` free functions in engine.cpp.
  void submode0();
  void submode1();
  void fieldTransitionCase5();        // FUN_8010766C (ov_game) — field-transition sub-machine case (PORT_GEN)
  void installFieldTransitions();     // wire the ov_game field-transition handlers on the override table
  // submode1Faithful: pc_faithful mirror of ov_game_gen_801088D8 — guest frame + jal-site ras;
  // case 0 dispatches the REAL 0x80044BD4 spawn-and-wait of the area-DATA loader 0x800452C0
  // (Asset::areaDataLoadAsTask on the task-1 fiber), parking the stage fiber organically. This
  // retires the pre-fiber Slip #3/#5 machinery (two-tick defer + RNG compensation) from the
  // faithful path; pc_skip keeps the one-tick shortcut (submode1Case0Skip + counter bumps).
  void submode1Faithful();
  bool submode1Case0Skip();

  // sm[0x48] state handlers (0=area INIT, 1=area RESUME-INIT, 2=RUNNING dispatcher) and the
  // sm[0x4c] area LOAD/TRANSITION machine. Formerly `ov_game_s48_0..2` / `ov_game_s4c` /
  // `ov_game_s48_2_frame` free statics in engine.cpp. All operate on the sm task-state
  // block at *0x1F800138 that Engine already owns.
  void s48_0();
  void s48_1();
  void s48_2();
  void s4c();
  void s48_2_frame();
  // areaLoadState(): guest FUN_80106478 — the 9-state (sm[0x4c]==0..8) area LOAD/TRANSITION body
  // that s4c() used to reach via rec_coro_redirect into the guest's own case-label addresses
  // (0x801064c4/106510/106580/1065b8/1066b8/106830/106930/10694c/1069b4). It is a plain
  // switch(sm[0x4c]) — Ghidra recovered it as ONE function, not 9 separate ones — so calling this
  // method fresh reaches the same case body the coro-redirect used to jump into directly (the
  // redirect skipped only the leading areaSlots.updateTail() call and the switch dispatch itself,
  // both idempotent / already-current here). Owns control flow + every sm/DAT_800bf84a/etc state
  // write; the pause/save-menu text-render leaves (FUN_8007E8DC/8007ED5C/8007EE74/8007EF60), the
  // quit-confirm dialog sub-machine (FUN_8007BF20, its own DAT_800bf84a-keyed SM), and the
  // audio-tick leaf (FUN_8001CF2C) stay substrate (font/pad-hold leaves with no PC-native
  // equivalent yet). Ghidra decomp scratch/decomp/game_all_list.c (FUN_80106478) +
  // scratch/decomp/area_load_leaves.c (the sub-leaves, RE'd to classify/rule them out as spawns).
  // NEGATIVE RESULT (walkable-Tomba spawn hunt): none of states 0-8 spawn anything — this machine
  // is entirely pause/save/quit menu UI + area audio-fade sequencing, driven by pad-edge bits at
  // 0x800E7E68. Rules out the last un-RE'd sibling of the sm[0x4c] area machine.
  void areaLoadState();

  // fieldFrame / fieldFrameX: the FIELD per-frame update body (guest 0x80108B0C) and its
  // mid-transition twin (0x80108BE4, sm[0x4a]==5 running-during-fade variant). Called by the
  // sm[0x4c] handlers and the transition drivers. Formerly ov_field_frame / ov_field_frame_x.
  void fieldFrame();
  // fieldFrameFaithful: pc_faithful mirror of ov_game_gen_80108B0C — guest frame (sp-24, r16@+16,
  // ra@+20, r16=0x1F800000 live), jal-site ras on every child, the render orchestrator dispatched
  // underneath, and the audio-command-queue tail 0x80075A80 dispatched substrate (f11 lib-fallback
  // recipe, same as the DEMO tail). No dualviewSnapshot capture/restore: under render-underneath
  // the substrate render's guest writes ARE faithful state and must not be rewound.
  void fieldFrameFaithful();
  void fieldFrameX();
  // fieldFrameXFaithful: pc_faithful mirror of ov_game_gen_80108BE4 — the mid-transition per-frame
  // twin of fieldFrameFaithful (guest 0x80108B0C). Same guest frame (sp-24, r16@+16, ra@+20,
  // r16=0x1F800000 live) + jal-site ras on every child; drops sceneStateStep/areaModeDispatch (not
  // in this variant) and dispatches the render orchestrator via mRender->frameX() (0x8003FA44)
  // instead of mRender->frame() (0x8003F9A8). No dualviewSnapshot here either — render-underneath
  // guest writes ARE faithful state.
  void fieldFrameXFaithful();

  // fieldTransition + its 4 workers: the sm[0x4a]==5 sub-scene / door / area FADE transition
  // machine (guest FUN_80108A60 + FUN_80107xxx workers). fieldTransition dispatches on sm[0x4c]
  // into one of the 4 workers or the "done -> return to field" epilogue. Formerly the
  // ov_field_transition / ov_transition_main / ov_transition_d3c / ov_transition_e20 /
  // ov_transition_f3c free statics in engine.cpp.
  void fieldTransition();
  void transitionMain();
  void transitionD3c();
  void transitionE20();
  void transitionF3c();
  // *Faithful: pc_faithful mirrors of ov_game_gen_80108A60 + the 4 ov_game_gen_80107xxx workers —
  // guest frame + jal-site ra discipline per worker, and (the one real behavior fix) the state-0
  // loader call in every worker now routes through rec_dispatch(c, 0x80044BD4u) — the literal guest
  // scheduler primitive, wired via the global override registry to PcScheduler::spawnAndWait —
  // instead of the
  // native_area_load_bd4() sync bypass the pc_skip bodies use. THESE MIRRORS CAN YIELD (spawnAndWait
  // parks the fiber), so the fork below calls them directly rather than through MV_CHECK.
  void fieldTransitionFaithful();
  void transitionMainFaithful();
  void transitionD3cFaithful();
  void transitionE20Faithful();
  void transitionF3cFaithful();

  // fieldRun / fieldRunX: the sm[0x4c]==2 field RUNNING sub-machine on sm[0x4e] (guest
  // FUN_80106B98) and its mid-transition twin (0x801070B4, sm[0x4c]==3). Formerly
  // ov_field_run / ov_field_run_x. Called by Engine::submode1 / fieldFrameX.
  void fieldRun();
  // fieldRunFaithful: pc_faithful mirror of ov_game_gen_80106B98 (12 states on sm[0x4e]) — guest
  // frame (sp-24, ra@+20, r16@+16) + jal-site ras; leaves are substrate dispatches at their RE'd
  // sites (core B proves them); ov_game_func_80108B0C runs the native Engine::fieldFrame owner.
  void fieldRunFaithful();
  void fieldRunX();
  // fieldRunXFaithful: pc_faithful mirror of ov_game_gen_801070B4 (mid-transition running
  // sub-machine, sm[0x4c]==3, sm[0x4e] states 0/1/2/other). Guest frame (sp-24, ra@+16) + jal-site
  // ras at every dispatch/native-call boundary, matching the reference shape of
  // Engine::fieldRunFaithful / Engine::submode1Faithful.
  void fieldRunXFaithful();

  // submitPage810c: the sm[task+0x6b]==1 page-1 (pause-menu dim) fade branch of the master submit
  // dispatcher at guest 0x8010810C. Owns just the dim-fade shape (subtractive #808080 held on
  // page-1) + a substrate dispatch to the still-unowned menu draw at 0x801084F8; other pages
  // fall through to substrate 0x8010810C. Was ov_game_submit_810c in engine.cpp.
  void submitPage810c();
  // submitPage810cFaithful: pc_faithful mirror of ov_game_gen_8010810C's page-1 branch. Guest frame
  // (sp-32, ra@+24, r17@+20, r16@+16 -- gen's shared prologue spills these on EVERY dispatch-table
  // branch, so they're spilled here too even though r17/r16 are unused on this branch) + jal-site ras
  // (0x801082B0 fade leaf, 0x801082B8 menu draw) + the L_801084CC/L_801084D0 common epilogue tail
  // (mem_w8 0x1F800232=0; mem_w8 0x800BF81E &= 2) that every branch falls through to and the pc_skip
  // shortcut was missing entirely. The fade leaf dispatches to substrate 0x8007E9C8 (byte-exact
  // packet-pool/scratchpad/OT writes -- same pattern as Sop::fieldModeFaithful) instead of the
  // host-state-only ScreenFade::set() the pc_skip=true path uses. Other pages delegate whole to
  // substrate (own frame + dispatch table).
  void submitPage810cFaithful();

  // (fadeSequencer moved to ScreenFade::sequence — see game/render/screen_fade.h;
  // callers reach it as `fade(c).sequence(node)`.)

  // frameUpdate: per-frame engine tick — the PC-driven game loop's frame body called directly
  //   from native_step_frame (native_boot.cpp). Runs the still-PSX per-frame update leaf, then
  //   owns the per-vblank audio (sequencer tick + SPU field advance), fps60 commit, and present
  //   + pace. Was the free function `ov_frame_update` in game_tomba2.cpp.
  void frameUpdate();

  // padEdgeFence: FUN_800788AC — the per-frame INPUT-EDGE FENCE (docs/engine_re.md "Per-frame
  //   fence FUN_800788ac"). Called exactly once per logic frame; `frameUpdate()`'s
  //   `rec_dispatch(c, 0x800788ACu)` routes here via the override (pad_edge_fence_install,
  //   §9-verified against gen 2026-07-16). Full RE in game/input/pad_edge_fence.cpp.
  void padEdgeFence();

  // drawOTag: PC-native DrawOTag (libgpu FUN_80081560 equivalent) — the per-frame draw kick.
  //   Called directly from native_step_frame (top-down PC-driven, NOT an override). Owns the
  //   engine's decoupled render path: for the FIELD stage builds the world natively via
  //   Render::sceneNative (real depth); walks the guest OT for un-owned 2D/HUD prims (queued
  //   into the engine render queue); flushes the queue in engine order via rq_flush. Takes the
  //   OT head as an explicit parameter (taxi-parameter c->r[4] retired). Was the free function
  //   `ov_draw_otag` in game_tomba2.cpp.
  void drawOTag(uint32_t otHead);

  // startBinStage: task-0's START.BIN file-table builder — dispatches to the pc_skip shortcut or
  // the pc_faithful hand-port of ov_start_gen_8010649C. See the two helper methods below.
  void startBinStage();
  // startBinStageSkip: pc_skip=true collapsed shortcut. Native VRAM upload (bypasses libgs), the
  // libcd dir cache populated by cdlibcd_* end-state (bypasses libcd), native ISO9660 file
  // lookups, inline asset.preloadTexgroup, task-1 slot closed with no body ever running.
  void startBinStageSkip();
  // startBinStageFaithful: pc_skip=false byte-exact port — the COMPLETE ov_start_gen_8010649C
  // task body, run on a PcScheduler fiber (runStage0FiberStanza). Guest-frame locals (sp-=456,
  // CdlFILE records at sp+16+i*24), live s-reg discipline, libcd file-table build via LibcdNative,
  // SM loop suspending inside PcScheduler::spawnAndWait/yieldPrim each frame. Never returns —
  // ends parked in the FUN_80052078 stage swap (the stanza cancels the fiber).
  void startBinStageFaithful();

  // stage0AdvanceSkip: pc_skip cadence — one step per scheduler tick after startBinStageSkip:
  // RNG stand-in, inline preloadStage1, sm advances, startStage(1). Collapses the substrate's
  // multi-tick spawn+wait cycles into inline calls. (The faithful path has no step machinery —
  // its cadence emerges from the fiber suspending in the ported primitives.)
  int stage0AdvanceSkip(uint8_t& step);


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
  // areaModeDispatchFaithful: pc_faithful mirror of gen_func_8001CAC0 + the 10 resident jump-table
  //   stubs (0x8001CB00..0x8001CB90) + the shared epilogue gen_func_8001CB98. Guest frame (sp-24,
  //   ra@+16) established unconditionally; for a valid area-mode index, r31 is set to that index's
  //   own stub jal-site constant (stub_addr+8) before dispatching the overlay handler — NOT the
  //   field-frame caller's ra — then the shared epilogue (0x8001CB98) restores ra/sp. pc_skip keeps
  //   the flattened direct-dispatch shortcut.
  void areaModeDispatchFaithful();

  // sceneEventFifo: the field EVENT/COMMAND-QUEUE state machine at guest 0x80025588 (struct
  //   @0x800ed058). 3-state top switch on base[2]: state 0 arms + falls through to state 1
  //   (active body drains a small FIFO); state >=2 no-op. Was `d0(c, 0x80025588)`.
  void sceneEventFifo();
  // sceneEventFifoFaithful: pc_faithful mirror of gen_func_80025588 — guest frame (sp-32,
  //   r16=B@+16, r17@+20, ra@+24) + jal-site ra set immediately before every dispatch
  //   (0x800255E0/0x80025610/0x80025630/0x8002569C/0x80025728/0x80025730). sceneEventFifo()
  //   below sets no ra at all, so its 5 non-leaf callees (which each spill their incoming r31
  //   onto their own guest stack frame) spill the WRONG byte under pc_faithful. Same control
  //   flow/store shape as sceneEventFifo(), just with the missing frame/ra discipline restored.
  void sceneEventFifoFaithful();
  void fieldSeqSchedulerTick();        // FUN_80075A80 — per-frame field sequence-scheduler tick
  void announcerCuePush();             // FUN_8004FA38 — announcer/message cue queue push
  void spawnType6Node();               // FUN_800310F4 — spawn a type-6 pool node with a param
  static void registerSpawnType6Node();
  static void registerAnnouncerCuePush();
  static void registerFieldSeqSchedulerTick();

  // fieldTargetCursor: guest FUN_800251F0 — the field TARGET-SELECT cursor state machine (operates on
  // the scene-event struct at a0). Called every field frame from sceneEventFifo's 0x800251F0 "default"
  // branch. Byte-faithful (gen_func_800251F0). registerFieldTargetCursor() installs it on the registry.
  void fieldTargetCursor();
  static void registerFieldTargetCursor();

  // sceneRenderListBuilder: 2-phase scene/render-list builder driver at guest 0x8004FE84 (struct
  //   @0x800bf548). Phase 0 arms it (snapshot list ptr 0x800ecf64 into base+0x2b0..0x2b8, advance
  //   phase to 1); phase 1 dispatches a sub-state handler (base[1] 0..3 -> distinct overlays);
  //   flag @0x800bf822 bit 0 latched from (base[1]!=0 || base[0x0a]!=0). Was `d0(c, 0x8004fe84)`.
  void sceneRenderListBuilder();
  // sceneRenderListBuilderFaithful: byte-exact mirror of gen_func_8004FE84 -- adds the guest frame
  //   push/pop (sp-=24, save/restore r16+r31 at sp+16/sp+20 with LIVE entry values) and the jal-site
  //   r31 constants (0x8004FF30/40/50/60) before the 4 sub-state dispatch leaves, which the plain
  //   sceneRenderListBuilder() body omits (a guest-stack scratch diff on every field frame under SBS).
  void sceneRenderListBuilderFaithful();

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
  void sceneStateStepFaithful();   // gen mirror of 0x80050DE4 (guest frame + jal-site ra discipline)

  // modePerFrameDispatch: the second per-frame render-mode dispatcher at guest 0x80022A80 (ov_field_frame
  //   calls it right after the object dispatcher). Keyed by the same render-mode byte 0x800BF870;
  //   reads a 24-entry function-pointer table at 0x8009D1D4 (MAIN.EXE .rodata, one entry per area
  //   mode, all pointing at overlay leaves in the 0x8010xxxx..0x80117xxx range that the currently-
  //   loaded overlay owns). Mode 3 (A00 fisherman village) is explicitly SKIPPED before the table
  //   read — the only special case. Replaces `d0(c, 0x80022a80u)` in the field-frame body.
  void modePerFrameDispatch();
  // modePerFrameDispatchFaithful: pc_faithful mirror of gen_func_80022A80. Guest frame descent
  // (sp-=24) + ra spill at sp+16 fire UNCONDITIONALLY (the gen `sw ra,16(sp)` is the beq's delay
  // slot, so it executes whether idx==3 or not) + jal-site ra 0x80022AB8u set before the indirect
  // dispatch. No null-target guard — matches gen (rec_dispatch(c,0) fail-fasts like the substrate
  // would on a null jalr target).
  void modePerFrameDispatchFaithful();

  // Small per-object leaves shared across many behavior handlers. Ghidra decomp
  // scratch/decomp/batch_leaves.c — see each method's body for the full RE. Kept as Engine
  // methods (not a new class) since they're each self-contained and only-recently-owned.

  // (FUN_80040CDC is ScriptInterp::init, not an anim leaf — a dead mis-named duplicate
  //  Engine::animEnvInit was removed here; see engine.cpp + docs/findings/scene.md.)

  // animTick(obj): guest FUN_8004190C. Ticks the animation VM (substrate FUN_80076D68) and
  //   stashes its returned byte into obj+0x79. Returns 1 (matches recomp v0).
  uint32_t animTick(uint32_t obj);

  // announcerCue(id, flag): guest FUN_8004ED94. Enqueues an announcer/UI cue by (id, flag) via
  //   the substrate FUN_8004FA38 leaf — table lookup at DAT_800BF7FC then DAT_800BF800 base.
  void announcerCue(uint32_t id, uint8_t flag);

  // (FUN_800518FC is NodeXform::buildWithOffset, not an Engine leaf — a dead duplicate
  //  Engine::objMatrixCompose was removed here; see engine.cpp + docs/findings/render.md.)

  // walkStart(obj, mode, subMode): guest FUN_80054D14. Transitions the object into anim `mode`.
  //   Returns 0 if already in that mode; else 1 (and delegates to FUN_80077C40 / FUN_80077CFC).
  uint32_t walkStart(uint32_t obj, uint32_t mode, int16_t subMode);

  // (playerGrowthStep moved to ActorTomba::growthStep — see game/player/actor_tomba.h; callers
  //  reach it as `eng(c).actorTomba.growthStep(mode)` since obj is always G.)

  // uploadModeSprites(): guest FUN_80067DA8. Uploads 5 sprite patterns (each a 16×1 BGR555 strip)
  //   to VRAM at fixed (X=0x1F0, Y=0x1E2/0x1E5/0x1C9/0x1D0/0x1B3) rects. The 5 source pointers
  //   are selected by mode byte DAT_800BF88D (0/1/2 → three distinct pattern sets in MAIN.EXE
  //   .rodata at 0x800A4800..0x800A49C0). Any other mode value returns without touching VRAM.
  //   Called from `beh_sop_intro_pilot`'s state-0 init; the guest passes a0 = master G but the
  //   function ignores it, so this method takes no args. Ghidra decomp scratch/decomp/fun_80067da8.c
  //   + disas.py of FUN_80081218 (= libgpu-style `LoadImage(RECT*, data)`; kept as substrate leaf).
  void uploadModeSprites();

  // gStateMutate(G, op): guest FUN_80058304. A 15-case dispatcher that flips flag bits on the
  //   master G block's byte pair (G+0x174, G+0xD) and fires an SFX/announcer cue via
  //   FUN_8004ED94(id, 0x41) for most cases (and Sfx::trigger for cases 8-9's alternate cue path).
  //   Cases 8/9/0xD/0xE also stash G+0x174 into DAT_800BF881 on the "already-set" fast path and
  //   otherwise pump obj[5]=<id-cue>, obj[6]=2 into a target obj (currently G). Case 0xC (called
  //   from Engine::fieldRun state 2) is the "clear G flags" body — zeros G+0x174 / G+0xD, then
  //   normalizes G+0x17E (clears the 0x8000 sign bit if set; force-sets to 0x10 when the 0x200
  //   flag is set, plus clears G+0x6F and mirrors 0x800BF89E / 0x800BF88F). Every case tail
  //   stamps DAT_800BF881 = G+0x174 (post-mutation snapshot).
  //   Ghidra decomp scratch/decomp/fieldrun_s2_init.c. `op` is the u8 selector (a1 in the recomp).
  void gStateMutate(uint32_t G, uint8_t op);

  // postRenderTick: small 3-state machine on byte 0x800BF842 at guest 0x80077D8C, called after the
  //   per-frame render submit. `b42 & 0x7F` selects: 1 = trigger FX 41 then set b42=0x87; 2 = trigger
  //   FX 42 then clear b42; otherwise = decrement b42. Trigger call = FUN_80074590(id, 2, -65) — a
  //   sound/vibration fx queue leaf, still substrate. Replaces `d0(c, 0x80077d8cu)` in ov_field_frame.
  void postRenderTick();
  void postRenderTickFaithful();   // pc_faithful mirror of gen_func_80077D8C: guest frame + jal-site ras

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
  // frameStartTickFaithful: byte-exact port of gen_func_80059D28 (guest 0x80059D28), used under
  //   pc_skip=false. Reproduces the gen prologue's frame descent (sp-=24) and r31/r16 spill/restore
  //   (r16 is reassigned to G in the guest register itself, not just held in a local constant, since
  //   callees are callee-saved on s0/r16 and spill it verbatim onto their own frames), sets the gen's
  //   per-case jal-site r31 constant before each mode-dispatch/default call so the callee's own
  //   guest-stack ra spill (e.g. func_8005950C's spill at its sp+28) matches core B, and dispatches
  //   the real gen_func_8009A450 (not the register-opaque native Rng class) for the rand advance so
  //   its trailing v0/v1/hi/lo side effects land bit-for-bit like core B.
  void frameStartTickFaithful();

  // ── Boot-time INIT (called from native_boot.cpp before the scheduler starts) ──────────────────
  // The engine's own PC-native init prefix (was the free functions `eng_init_*` in startup.cpp).
  // Each method reimplements the corresponding guest init leaf: frame/display/camera state, the
  // entity pool control block, the allocator + dispatch table, mode ctrl, input, and the orchestrator
  // that sequences them. Called via eng(c).initX() from native_boot.
  void initFrameState();               // FUN_80050A0C — vblank + double-buffer pacing state
  void initDisplay();                  // FUN_800509B4 — GTE projection CRs + display H
  void initCamera();                   // FUN_80050A80 — camera scratchpad matrices + state
  // FUN_80051794 (identity 3x3 rot + zero translation) is owned by `Mtx::identity` (game/math/mtx.h)
  // — call `mtxOf(c).identity(p)` directly (deduped 2026-07-08: this used to carry a redundant copy).
  void initEntityPool();               // FUN_8007B328 — entity-pool control block + fixed-pt scales
  void initAlloc(uint32_t s1, uint32_t s2);  // FUN_80088B00 — allocator + 6-entry dispatch table
  void initInput();                    // FUN_80087A60 → 80086970 — input subsystem
  void initSubsystems();               // FUN_800520E0 — orchestrator (entity pool + alloc + mode + input)

  // seedDirectionMasks(flipped): guest FUN_8007B2C0. Seeds the 4 fixed-point direction-mask words
  //   at 0x1F800170/172/174/176 (the same words initEntityPool's FUN_8007B2C0(0) call seeds at
  //   boot). flipped==0 -> 0x8000/0x4000/0x2000/0x1000; else the reverse order
  //   0x1000/0x2000/0x4000/0x8000 (a mirrored-facing swap). Ghidra decomp
  //   scratch/decomp/fun_8007b2c0.c.
  void seedDirectionMasks(bool flipped);
  // reloadEntityPool(): guest FUN_8007B3F4. Re-copies the staged per-area entity-pool control
  //   bytes (0x800BFE4C/4D/4E/4F + 0x800BF8A3/88A/88B) onto the live control header
  //   initEntityPool seeded at boot (0x800FB166/167/161/162/163/164/165 respectively), then
  //   reseeds the direction masks via seedDirectionMasks(staged byte 0x800BFE4C). Called by
  //   Engine::areaLoadState's state 7 confirm-branch (quit-confirm accepted -> resume). Ghidra
  //   decomp scratch/decomp/area_load_leaves.c.
  void reloadEntityPool();

  // setAreaStartPos(): guest FUN_80078824. Loads the player's spawn position + facing for the
  //   current (area,sub-area) from the per-area start-pos table 0x800A54A8[area] -> 8-byte record
  //   [sub]: writes X/Y/Z (<<16 fixed) to 0x800BF890/894/898 and the facing byte (masked 0x7F) to
  //   0x800BFE38. Leaf, no frame. Called from the area machine's spawn state.
  void setAreaStartPos();

  // activeModeCtx(): guest FUN_80086604. Returns the active mode/draw-env context pointer held at
  //   0x800ABE20. Leaf accessor, no frame.
  uint32_t activeModeCtx();

  // installModeHandlers(): guest FUN_80086738. Installs the 2-entry mode handler table at
  //   0x80102444 ([0]=0x800867CC, [1]=Engine::runModeEnter=0x80086764) and zeroes the guard slot
  //   0x80102440 + trailing slot 0x8010244C. Leaf, no frame. Called from initAlloc's post-init.
  void installModeHandlers();

  // runModeEnter(): guest FUN_80086764. If both bit0 flags in the mode ctx (*0x800ABE98)[+0]/[+4]
  //   are set, dispatches the mode-enter handler at 0x800ABE60 (when non-null) and returns 1; else
  //   returns 0. Ready-FRAME (sp-24, ra@16). One of the handlers installModeHandlers registers.
  uint32_t runModeEnter();
};

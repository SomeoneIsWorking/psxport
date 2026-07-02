// Game — the WHOLE machine as ONE per-instance object, so NOTHING is a file-scope global.
//
// Goal (user directive, 2026-06-19): run TWO cores in one process in lockstep (overrides-ON vs
// overrides-OFF) and diff their state to find the first divergence. That requires every piece of
// mutable machine state to live on an instance — no `static` file-scope variables anywhere in the
// runtime. `Game` is that instance: it OWNS the CPU/RAM `Core` plus every subsystem's state.
//
// Threading: `Core*` is already passed through the entire interpreter + generated shards, so we do
// NOT re-thread the CPU handle. Instead `Core` carries a back-pointer `core.game`, and any code that
// holds a `Core* c` reaches migrated subsystem state via `c->game->...`. New subsystem code may take
// a `Game*` directly. Each subsystem's former file-scope statics become a `*State` sub-struct member
// here, migrated ONE subsystem per phase, 0-diff-verified each step (see docs/game-deglobalize-plan.md).
#pragma once
#include "core.h"
#include "gte_state.h"             // GteRegs — per-instance GTE (COP2) register file (Beetle gte.c)
#include "cdc_state.h"             // CdcState — per-instance native CD-controller register model (cdc_native.c)
#include "xa_state.h"              // XaState  — per-instance native XA-ADPCM CD-audio streamer (xa_stream.c)
#include "spu_state.h"             // per-instance SPU state handle (Beetle spu.c) — SPU_NewState/Bind
#include "mdec_state.h"            // per-instance MDEC state handle (Beetle mdec.c) — MDEC_NewState/Bind
#include "gpu_native_internal.h"   // GpuState — the native GPU's per-instance render machine state
#include "gpu_gpu_internal.h"       // GpuGpuState — the Vulkan present backend's per-instance render state
#include "fps60_internal.h"        // Fps60 — the interpolated-60fps tier's per-instance state
#include "render_queue.h"          // RenderQueue — the engine-owned draw-order authority
#include <stdint.h>
#include <setjmp.h>

// ---- per-subsystem state structs (former file-scope statics), migrated one phase at a time ----

// timing.cpp — class Timing — native VBlank/VSync frame clock subsystem, owned by Game.
// PROPER OOP: one instance per Game (`c->game->timing`). Back-pointer `game` wired in Game(). Owns
// the libetc VSync counter mirror + the vsync/frame-tick behavior. Was the ov_vsync_callback /
// ov_vsync / timing_frame_tick / timing_vblank / timing_init free functions in timing.cpp.
class Game;
class Timing {
public:
  Game* game = nullptr;
  uint32_t vblank = 0;   // libetc VSync counter mirror (was g_vblank)

  // vsyncCallback(): 0x80085BB0 FUN_80085bb0 VSyncCallback(func) — no-op. Native frame loop
  //   owns pacing; the libapi per-vblank IRQ vector isn't modeled. Was ov_vsync_callback.
  void vsyncCallback();

  // vsync(): 0x80085900 FUN_80085900 = libetc VSync(mode). Currently unreachable — sync_overrides
  //   traps VSync (all pacing is PC-native). Kept for RE reference / future re-enable.
  void vsync();

  // frameTick(): advance the canonical libetc VSync counter once per native frame. Called from
  //   the PC-native frame loop (native_scheduler_step) so recomp code reading DAT_800abde0 for
  //   pacing/idle-timers keeps advancing.
  void frameTick();
};

// cd_override.cpp — deferred ingame-music state (suppressed during dialog, resumed after).
struct CdState {
  int      pending_music = 0;   // a looping ingame-music clip is deferred/remembered (was s_pending_music)
  uint8_t  pm_chan  = 0;        // was s_pm_chan
  uint32_t pm_start = 0;        // was s_pm_start
  uint32_t pm_end   = 0;        // was s_pm_end
};

// CdcState / XaState — native CD-controller register model + XA-ADPCM streamer, PER-INSTANCE so two
// cores (native vs PSX-recomp) keep SEPARATE CD state. Plain-C structs in their own headers (like
// gte_state.h's GteRegs) so cdc_native.c / xa_stream.c stay C; bound per frame-step via cdc_bind /
// xa_bind (those files), exactly like gte_bind/spu_bind/mdec_bind. See cdc_state.h / xa_state.h.

// hle.cpp — class Hle — BIOS HLE subsystem (event control blocks, native first-fit heap, IRQ /
// work-area flags), owned by Game (`c->game->hle`). Back-pointer `game` wired in Game(). Was
// struct HleState; the deliverEvent method promotes the former free function hle_deliver_event
// (called by timing/native_boot/memcard/asset for VBlank + memcard + sound-DMA event delivery)
// so callers do c->game->hle.deliverEvent(class, spec) — no Core* arg on the surface.
struct HleEvCB { int open, enabled, fired; uint32_t ev_class, spec, mode, func; };  // was EvCB
struct HleHeapBlock { uint32_t addr, size; int used; };                             // was HeapBlock
class Hle {
public:
  Game* game = nullptr;
  HleEvCB     ev[16]      = {};   // was s_ev[EVCB_MAX]
  HleHeapBlock blk[4096]  = {};   // was s_blk[HEAP_MAX_BLOCKS]
  int      nblk       = 0;        // was s_nblk
  uint32_t heap_base  = 0;        // was s_heap_base
  uint32_t heap_size  = 0;        // was s_heap_size
  int      heap_ok    = 0;        // was s_heap_ok
  int      work_ok    = 0;        // was s_work_ok
  uint32_t int_handler = 0;       // was s_int_handler (B0:0x19 HookEntryInt)
  int      irq_enabled = 1;       // was s_irq_enabled

  // deliverEvent(evClass, spec): mark every open+enabled event slot whose class matches evClass
  //   and whose spec masks against `spec` as fired. Called by the frame VBlank tick, memcard
  //   completion, and sound-DMA completion so guest waits (TestEvent/WaitEvent) advance.
  void deliverEvent(uint32_t evClass, uint32_t spec);
};

// pad_input.cpp — class Pad — native controller input subsystem, owned by Game (c->game->pad).
// Carries the current host button state + REPL drive control + all the pad_* behavior (host poll,
// per-VBlank fill buffer, REPL hold/tap/release). Was the pad_init/pad_set_buttons/pad_buttons/
// pad_fill_buffer/pad_poll_sdl/pad_overrides_init/pad_repl_hold/pad_repl_tap/pad_repl_release free
// functions in pad_input.cpp. (Test-hook / config-cache statics inside the SDL poll path stay
// shared per the plan policy — those are host-wide, not per-Core.)
class Pad {
public:
  Game* game = nullptr;
  uint16_t buttons    = 0xFFFF;  // current host button state, active-low (0 bit = pressed) (was s_buttons)
  uint16_t repl_hold  = 0xFFFF;  // REPL: bits cleared = held down (was s_repl_hold)
  uint16_t repl_tap   = 0xFFFF;  // REPL: active-low mask pressed for repl_tap_n frames (was s_repl_tap)
  int      repl_tap_n = 0;       // REPL: tap countdown frames (was s_repl_tap_n)
  int      repl_on    = 0;       // REPL drive active (was s_repl_on)

  void init();                              // was pad_init(Core*)
  void setButtons(uint16_t mask);           // was pad_set_buttons(Core*, mask) — feed the active-low mask
  void fillBuffer(uint8_t* buf);            // was pad_fill_buffer(Core*, buf) — per-VBlank guest read pad
  void pollSdl();                           // was pad_poll_sdl(Core*) — host SDL controller poll
  void overridesInit();                     // was pad_overrides_init(Core*) — install per-VBlank pad-read override
  void driveHold(uint16_t activeLowMask);   // was pad_repl_hold(c, mask) — REPL: hold down these bits
  void driveTap(uint16_t activeLowMask, int nframes);  // was pad_repl_tap(c, mask, n) — press for n frames
  void driveRelease();                      // was pad_repl_release(c) — clear REPL drive
  void serviceFrame();                      // was pad_service_frame(c) — per-frame native pad service
};

// native_fmv.cpp — native .STR movie player. Its only per-instance mutable state is the Start-skip
// edge flag; the SDL audio-sink handles (s_fmv_dev/freq) stay a shared host-output singleton (you
// can't open two host audio devices for one speaker; a lockstep RAM diff is unaffected by them).
struct FmvState {
  int start_prev = 0;   // Start was down on the previously polled frame (skip edge-detect) (was s_fmv_start_prev)
};

// native_boot.cpp — the native cooperative task scheduler (replaces the BIOS thread layer). A task
// context is ONLY the CPU register file (R3000) — guest RAM/scratchpad/DMA/peripherals are SHARED one
// memory across all tasks (saving a whole Core here would give each task its own RAM snapshot — the OOP
// regression where the loader task read a pre-fill file-table snapshot and stalled boot; see
// oop-regression-hunt). So task_ctx slices to the R3000 base on save/restore.
class Coro;          // runtime/recomp/coro.h — thread-fiber for full-PSX mid-function resume (later-264)
struct RecOverlay;   // generated overlay_table.h — descriptor of one recompiled overlay image
struct SchedulerState {
  jmp_buf yield_jmp;          // longjmp target = the setjmp in native_scheduler_step (was g_yield_jmp)
  R3000   task_ctx[3] = {};   // saved CPU register context per task slot, registers only (was g_task_ctx)
  int     in_stage = 0;       // 1 while inside a task run (gates the yield override) (was g_in_stage)
  int     cur_slot = 0;       // task slot currently running (for the yield capture) (was g_cur_slot)
  int     task_started[3] = {};  // slot has a live coroutine context (else fresh) (was g_task_started)
  int     demo_native[3] = {};   // slot runs the DEMO/front-end as a NATIVE per-frame dispatcher (no guest
                                 // coroutine): ov_demo_frame is called once per frame, state in guest RAM.
  int     game_native[3] = {};   // slot runs the GAME stage as a NATIVE per-frame dispatcher (ov_game_frame
                                 // once per frame; state in guest RAM). Mirrors demo_native.
  int     game_coop[3] = {};     // slot runs the GAME COOPERATIVE task loop (a GAME state not yet owned
                                 // natively). As a PC game the per-frame cooperative yield is just a frame
                                 // boundary: RE-ENTER the recompiled loop at its TOP (0x801063F4) every
                                 // frame with the loop's callee-saved regs, instead of resuming at the saved
                                 // mid-yield PC (which the substrate can't continue — the loop's C frame was
                                 // longjmp'd away at the yield). All loop state lives in guest RAM, so
                                 // re-entry == continue. (native_boot.cpp; the loop body is the seeded
                                 // ov_game_func_801063F4.)

  // ---- FULL-PSX (psx_fallback) thread-fiber coroutines (later-264) -----------------------------
  // The native path above re-enters at a loop top / runs synchronous dispatchers, so it never needs a
  // true mid-function resume. The FULL-PSX path (PSXPORT_SBS_MODE=gameplay/both core B) runs pure
  // recompiled task bodies that yield mid-function (ov_switch); the substrate can't re-enter mid-body,
  // so each such task runs on its OWN Coro thread that BLOCKS at a yield (preserving its C stack) and
  // CONTINUES on resume — recompiler-only, no interpreter (USER 2026-06-30). Active ONLY when
  // psx_fallback is on; the native path is untouched. cur_is_coro tells ov_switch to coro-yield (or
  // Coro::exit_now on task-end) vs longjmp; Coro owns its own unwind jmp_buf for end/cancel.
  Coro*   coro[3] = {};          // per-slot fiber (heap; nullptr = no live full-PSX task on this slot)
  int     cur_is_coro = 0;       // 1 while a Coro task is running -> ov_switch yields via the fiber

  // Resident overlay per OVERLAP SLOT (0x80106228 stage / 0x80108F9C mode / 0x8018A000 area), recorded
  // by overlay_note_load() at LOAD time — when the freshly-written image still matches its raw .BIN
  // signature, BEFORE the game mutates its header pointer table at runtime. The router routes by this
  // IDENTITY (robust), falling back to a content-signature scan only when unset. Fixes the overlay-router
  // miss when GAME's header pointer @+0x08 is swapped at runtime so the raw-bytes signature no longer
  // matches (later-264). Per-instance for SBS (each core has its own resident set).
  const RecOverlay* resident_ov[3] = {};
};

// native_stub.cpp — the SCEA boot-stub (SCUS_944.54) interpreter that draws SCEA + LoadExec's MAIN.
struct StubState {
  uint32_t    vblank    = 0;        // boot-stub VBlank counter during SCEA fades (was g_stub_vblank)
  const char* main_path = nullptr;  // MAIN.EXE path, reloaded at LoadExec hand-off (was g_main_path)
  jmp_buf     exit_jmp;             // longjmp target = native_stub_run's setjmp (was g_stub_exit)
};

class Game {
public:
  Core core;            // CPU registers + 2 MB main RAM + 1 KB scratchpad (was the sole instance object)

  // ---- migrated subsystem state (one member per migrated subsystem) ----
  Timing      timing;
  CdState     cd;
  CdcState    cdc;   // native CD-controller register model (per-instance; cdc_native.c, bound via cdc_bind)
  XaState     xa;    // native XA-ADPCM CD-audio/voice streamer (per-instance; xa_stream.c, bound via xa_bind)
  Hle         hle;
  Pad         pad;
  FmvState    fmv;
  StubState   stub;
  SchedulerState sched;  // native cooperative task scheduler (native_boot.cpp)
  GpuState    gpu;   // native GPU: VRAM + draw/display state + the rasterizer (gpu_native.cpp)
  GpuGpuState  gpu_gpu;// Vulkan present backend: per-frame batch/depth/dirty/present state (gpu_gpu.cpp)
  RenderQueue rq;    // engine-owned render queue: the single draw-ORDER authority (render_queue.cpp)
  Fps60  fps60; // interpolated-60fps tier: capture buffers + matcher + remap (fps60.cpp)
  GteRegs     gte{}; // GTE (COP2) register file — per-instance so two cores keep SEPARATE GTE state
                     // (Beetle gte.c bound to this via GTE_BindState; see gte_bind, gte_beetle.cpp)
  void* spu_state = nullptr;  // per-instance SPU state (Beetle spu.c), heap-allocated; bound via SPU_BindState
  int   spu_powered = 0;      // SPU_Power run on this instance's state yet? (lazy power on first bind)
  void* mdec_state = nullptr; // per-instance MDEC state (Beetle mdec.c), heap-allocated; bound via MDEC_BindState
  int   mdec_powered = 0;     // MDEC_Power run on this instance's state yet? (lazy power on first bind)

  // ---- PSX-fallback gate (diagnostic, user 2026-06-23) --------------------------------------------
  // ONE switch: keep BOOT (native crt0/FMV/init) and the FRAME LOOP skeleton native, but run EVERYTHING
  // the frame loop calls — the stage state machines, all asset/area LOADING, and content — as the PSX
  // RECOMP body instead of the native owners. CD reads still go through the platform CD layer (cd_override
  // ov_cd_loadfile/ov_cd_dc40/ov_cd_async_read), so the PSX loaders run SYNCHRONOUSLY (no busy-wait). This
  // restores a working PSX baseline to compare the native path against. Set by PSXPORT_GATE (nonzero) and
  // the REPL `gate on|off`. Default OFF = full native (shipped behavior). Wired in native_boot.cpp.
  int psx_fallback = 0;

  // ---- dual-core diff mode (dualcore.cpp) ----------------------------------------------------------
  // When set, the frame body runs ONLY the guest-state-mutating work (per-frame update + scheduler +
  // loaders) and SKIPS all host output — present, pace, audio device feed, render submit, FMV. This lets
  // two Game instances step in one process without the shared VK/SDL/Beetle output singletons fighting,
  // so we can diff their guest RAM (PSX-fallback core vs native core) to find what the native loader drops.
  int diff_mode = 0;

  // ---- SBS render-submit flag (sbs.cpp, PSXPORT_SBS) ----------------------------------------------
  // In the side-by-side dual-core debugger BOTH cores must EMIT their frame geometry into their own VK
  // batch (so the SBS composite can draw each into its pane), but NEITHER core may PRESENT to the window
  // (the SBS loop owns the single composite present). diff_mode=1 suppresses the per-core present (and
  // pace/audio) at the engine frame tail; sbs_render=1 RE-enables ONLY the render-submit block in
  // native_step_frame (native_boot.cpp) that diff_mode would otherwise skip. So SBS sets BOTH: present
  // off (diff_mode), geometry emit on (sbs_render). Default 0 (the shipped single-core game presents).
  int sbs_render = 0;

  // Dual-core diff control: the per-core override-neutralize flag. The terrain override (ov_terrain,
  // engine_submit.cpp) is in the SHARED dispatch table; each core decides ON vs neutralized by reading
  // THIS flag (not divergent override tables — see docs game-deglobalize-plan P7). The harness sets it on
  // the `b` core (terrain -> recomp super-call) so an a-vs-b core.ram diff isolates submit_terrain.
  int neutralize_terrain = 0;

  // core.game / gpu.game / gpu_gpu.game are back-pointers so a subsystem holding one of those handles can
  // reach the rest of the machine (e.g. blit_src -> gpu_gpu via gpu.game; frame_via_fb -> s_seen3d via
  // gpu_gpu.game->core). Set once here so no file-scope global is needed.
  Game() { core.game = this; gpu.game = this; gpu_gpu.game = this; timing.game = this; pad.game = this;
           hle.game = this;
           spu_state = SPU_NewState(); mdec_state = MDEC_NewState();
           cdc_state_init(&cdc); xa_state_init(&xa); }   // per-instance CD-controller + XA streamer defaults
  ~Game() { SPU_FreeState(spu_state); MDEC_FreeState(mdec_state); }
};

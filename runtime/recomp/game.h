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
#include "gpu_native_internal.h"   // GpuState — the native GPU's per-instance render machine state
#include "gpu_vk_internal.h"       // GpuVkState — the Vulkan present backend's per-instance render state
#include "fps60_internal.h"        // Fps60State — the interpolated-60fps tier's per-instance state
#include "render_queue.h"          // RenderQueue — the engine-owned draw-order authority
#include <stdint.h>
#include <setjmp.h>

// ---- per-subsystem state structs (former file-scope statics), migrated one phase at a time ----

// timing.cpp — native VBlank/VSync frame clock.
struct TimingState {
  uint32_t vblank = 0;   // libetc VSync counter mirror (was g_vblank)
};

// cd_override.cpp — deferred ingame-music state (suppressed during dialog, resumed after).
struct CdState {
  int      pending_music = 0;   // a looping ingame-music clip is deferred/remembered (was s_pending_music)
  uint8_t  pm_chan  = 0;        // was s_pm_chan
  uint32_t pm_start = 0;        // was s_pm_start
  uint32_t pm_end   = 0;        // was s_pm_end
};

// hle.cpp — BIOS HLE: event control blocks, native first-fit heap, IRQ/work-area flags.
struct HleEvCB { int open, enabled, fired; uint32_t ev_class, spec, mode, func; };  // was EvCB
struct HleHeapBlock { uint32_t addr, size; int used; };                             // was HeapBlock
struct HleState {
  HleEvCB     ev[16]      = {};   // was s_ev[EVCB_MAX]
  HleHeapBlock blk[4096]  = {};   // was s_blk[HEAP_MAX_BLOCKS]
  int      nblk       = 0;        // was s_nblk
  uint32_t heap_base  = 0;        // was s_heap_base
  uint32_t heap_size  = 0;        // was s_heap_size
  int      heap_ok    = 0;        // was s_heap_ok
  int      work_ok    = 0;        // was s_work_ok
  uint32_t int_handler = 0;       // was s_int_handler (B0:0x19 HookEntryInt)
  int      irq_enabled = 1;       // was s_irq_enabled
};

// pad_input.cpp — native controller input: current host button state + the REPL drive control.
// (Test-hook / config-cache statics inside pad_service_frame stay shared per the plan policy.)
struct PadState {
  uint16_t buttons    = 0xFFFF;  // current host button state, active-low (0 bit = pressed) (was s_buttons)
  uint16_t repl_hold  = 0xFFFF;  // REPL: bits cleared = held down (was s_repl_hold)
  uint16_t repl_tap   = 0xFFFF;  // REPL: active-low mask pressed for repl_tap_n frames (was s_repl_tap)
  int      repl_tap_n = 0;       // REPL: tap countdown frames (was s_repl_tap_n)
  int      repl_on    = 0;       // REPL drive active (was s_repl_on)
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
  TimingState timing;
  CdState     cd;
  HleState    hle;
  PadState    pad;
  FmvState    fmv;
  StubState   stub;
  SchedulerState sched;  // native cooperative task scheduler (native_boot.cpp)
  GpuState    gpu;   // native GPU: VRAM + draw/display state + the rasterizer (gpu_native.cpp)
  GpuVkState  gpu_vk;// Vulkan present backend: per-frame batch/depth/dirty/present state (gpu_vk.cpp)
  RenderQueue rq;    // engine-owned render queue: the single draw-ORDER authority (render_queue.cpp)
  Fps60State  fps60; // interpolated-60fps tier: capture buffers + matcher + remap (fps60.cpp)

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

  // Dual-core diff control: the per-core override-neutralize flag. The terrain override (ov_terrain,
  // engine_submit.cpp) is in the SHARED dispatch table; each core decides ON vs neutralized by reading
  // THIS flag (not divergent override tables — see docs game-deglobalize-plan P7). The harness sets it on
  // the `b` core (terrain -> recomp super-call) so an a-vs-b core.ram diff isolates submit_terrain.
  int neutralize_terrain = 0;

  // core.game / gpu.game / gpu_vk.game are back-pointers so a subsystem holding one of those handles can
  // reach the rest of the machine (e.g. blit_src -> gpu_vk via gpu.game; frame_via_fb -> s_seen3d via
  // gpu_vk.game->core). Set once here so no file-scope global is needed.
  Game() { core.game = this; gpu.game = this; gpu_vk.game = this; }
};

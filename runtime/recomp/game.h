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
#include "repl.h"                  // class Repl — REPL driver + auto-drive request state
#include "spu_audio.h"             // class SpuAudio — host audio output sink (SDL3 + WAV capture)
#include "audio/native_music.h"    // class NativeMusic — in-game real-time SEP/VAB music player
#include "audio/music_list.h"      // class MusicList — Sound Test catalogue + area BGM driver
#include "rmlui_overlay.h"         // class RmlOverlay — mod/debug HTML UI + world readout HUD
#include "native_gate.h"           // class NativeGates — PC-native-layer A/B GATE registry (REPL diag)
#include "platform_hle.h"          // class PlatformHle — HW-sync HLE table (VSync/CdSync/…)
#include "memcard.h"               // class Memcard — host-backed 128 KB memory card device
#include "dbg_server.h"            // class DbgServer — live TCP debug endpoint (127.0.0.1)
#include "verify_harness.h"        // class VerifyHarness — shared A/B verify scaffold (game/core)
#include "ffspan.h"                // class FfSpan — PSXPORT_BDTAG builder-span attribution (game/render)

class Sbs;                          // forward decl — Game holds `sbs` back-pointer set by Sbs::run
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
  uint32_t vblank = 0;      // libetc VSync counter mirror (was g_vblank)
  uint32_t logicFrame = 0;  // logic-frame counter, advanced by native_step_frame each iteration.
                            // Read by xa_audio_trace / [bgmreq]-style diags. Was global g_bgm_frame.

  // vsyncCallback(): 0x80085BB0 FUN_80085bb0 VSyncCallback(func) — no-op. Native frame loop
  //   owns pacing; the libapi per-vblank IRQ vector isn't modeled. Was ov_vsync_callback.
  void vsyncCallback();

  // vsync(): 0x80085900 FUN_80085900 = libetc VSync(mode). Currently unreachable — sync_overrides
  //   traps VSync (all pacing is PC-native). Kept for RE reference / future re-enable.
  void vsync();

  // frameTick(): advance the canonical libetc VSync counter once per native frame. Called from
  //   the PC-native frame loop (native_step_frame) so recomp code reading DAT_800abde0 for
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

  // ---- BIOS-side helpers (was file-scope free fns in hle.cpp) ------------------
  // heap: A0:0x33-0x39 native first-fit arena (bookkeeping outside PSX RAM).
  void     heapInit(Core* c, uint32_t addr, uint32_t size);
  uint32_t heapAlloc(Core* c, uint32_t size);
  void     heapFree (Core* c, uint32_t addr);
  uint32_t heapBlockSize(Core* c, uint32_t addr) const;
  // work area (B0:0x56/0x57 GetC0Table/GetB0Table): publish a self-consistent native page.
  void     workAreaInit(Core* c);
  // events: index-lookup for B0:0x08/0x09/0x0A/0x0B/0x0C/0x0D
  int      eventIndex(uint32_t id) const;
  // BIOS-call dispatch (A0/B0/C0). Returns true if handled (Core V0 set).
  bool     dispatchBios(char table, uint32_t fn, Core* c);

private:
  void     heapCoalesce();   // internal free-side merge pass; only touches this instance's blk[]
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

#include "pc_scheduler.h"          // class PcScheduler — the PC-native cooperative task scheduler
                                   // (per-task contexts, run flags, step-spread counters, stanza dispatch)

// native_stub.cpp — the native SCEA boot-stub (renders SCEA + LoadExec's MAIN.EXE). Only field
// still in use is main_path (2 reads in native_stub_run); the old boot-stub VBlank counter and
// LoadExec longjmp target are gone in the current PC-native boot path.
struct StubState {
  const char* main_path = nullptr;  // MAIN.EXE path, reloaded at LoadExec hand-off (was g_main_path)
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
  Repl        repl;  // interactive REPL driver + REPL-armed auto-drive requests (repl.cpp)
  FmvState    fmv;
  StubState   stub;
  PcScheduler pcSched;   // native cooperative task scheduler (game/core/pc_scheduler.cpp)
  GpuState    gpu;   // native GPU: VRAM + draw/display state + the rasterizer (gpu_native.cpp)
  GpuGpuState  gpu_gpu;// Vulkan present backend: per-frame batch/depth/dirty/present state (gpu_gpu.cpp)
  RenderQueue rq;    // engine-owned render queue: the single draw-ORDER authority (render_queue.cpp)
  Fps60  fps60; // interpolated-60fps tier: capture buffers + matcher + remap (fps60.cpp)
  SpuAudio    spu_audio;    // host audio output sink (SDL3 device + optional WAV capture)
  NativeMusic native_music; // real-time SEP/VAB synth mixed into the SPU sink each audio frame
  MusicList   music_list;   // Sound Test catalogue + in-game area BGM driver (uses native_music)
  RmlOverlay  rml_overlay;  // in-app mod/debug HTML UI + live world-position HUD
  NativeGates native_gates; // A/B gate registry: `native <name> on|off` diag (music/seqtick/…)
  PlatformHle platform_hle; // HW-sync HLE dispatch table (VSync/CdSync/MDEC/ChangeThread)
  Memcard     memcard;      // host-backed 128 KB memory card device (BIOS libcard/libmcrd)
  DbgServer   dbg_server;   // live TCP debug endpoint (PSXPORT_DEBUG_SERVER=<port>)
  VerifyHarness verify;     // shared A/B verify scaffold: snapshot buffers + per-check counters
  FfSpan      ffspan;       // PSXPORT_BDTAG per-frame builder-span attribution
  Sbs*        sbs = nullptr;// SBS harness back-pointer (nullptr in standalone; set by Sbs::run)
  GteRegs     gte{}; // GTE (COP2) register file — per-instance so two cores keep SEPARATE GTE state
                     // (Beetle gte.c bound to this via GTE_BindState; see gte_bind, gte_beetle.cpp)
  // (native-depth cache moved to `class ProjPrim` on Render — reach as `c->mRender->projprim`, 2026-07-03)
  void* spu_state = nullptr;  // per-instance SPU state (Beetle spu.c), heap-allocated; bound via SPU_BindState
  int   spu_powered = 0;      // SPU_Power run on this instance's state yet? (lazy power on first bind)
  void* spu_log = nullptr;    // per-Game SPU write log (SpuWriteLog*, spu_beetle.c) — Sbs compares A vs B at
                              // frame boundary to flag audio-relevant divergences (Issue #29). NULL when SBS off.
  void* mdec_state = nullptr; // per-instance MDEC state (Beetle mdec.c), heap-allocated; bound via MDEC_BindState
  int   mdec_powered = 0;     // MDEC_Power run on this instance's state yet? (lazy power on first bind)

  // ---- PSX-fallback gate (diagnostic, user 2026-06-23) --------------------------------------------
  // ONE switch: keep BOOT (native crt0/FMV/init) and the FRAME LOOP skeleton native, but run EVERYTHING
  // the frame loop calls — the stage state machines, all asset/area LOADING, and content — as the PSX
  // RECOMP body instead of the native owners. CD reads still go through the platform CD layer (cd_override
  // cd_loadfile/cd_dc40/cd_async_read), so the PSX loaders run SYNCHRONOUSLY (no busy-wait). This
  // restores a working PSX baseline to compare the native path against. Set by PSXPORT_GATE (nonzero) and
  // the REPL `gate on|off`. Default OFF = full native (shipped behavior). Wired in native_boot.cpp.
  int psx_fallback = 0;

  // ---- pc_skip: per-fork shortcut flag (see CLAUDE.md "The 5 paths") -----------------------------
  // Per-site fork bool. Every collapsed-multi-step-init in the native path is shaped:
  //     if (game->pc_skip) load_in_one_step();               // shortcut, end-state only
  //     else               load_in_multi_step_faithfully();  // byte-exact to recomp_path
  //
  //   pc_faithful path (pc_skip=false, psx_fallback=0):
  //       Native OOP that is byte-exact to recomp_path (PSX_GATE=1). What the SBS harness compares.
  //       This is Job#1 — the "faithful" branch of every fork must match the substrate exactly.
  //   pc_skip path (pc_skip=true, psx_fallback=0):
  //       Same code, shortcuts taken where they're safe (collapse recomp coroutine yields into one
  //       tick, skip redundant preloads, etc.). Default for `./run.sh`.
  //   recomp_path (psx_fallback=1, pc_skip ignored):
  //       Full substrate — the stage machines, loaders, and content run as the recompiled PSX body
  //       instead of the native owners. The oracle for byte-comparison.
  //
  // Default is pc_skip=true — the NATIVE shortcut path (Engine::startBinStage, Demo::stageMain,
  // etc.). Set PSXPORT_PC_SKIP=0 to route everything through the fiber substrate (audit mode:
  // no native handlers run, scheduler.cpp `has_native_handler_for_entry` returns false). Under
  // SBS the DEFAULT is also pc_skip=true because that's where real native bugs are — pc_skip=false
  // just makes A run the same substrate as B (trivial byte-match).
  bool pc_skip = true;

  // Field BGM director latch (game_tomba2.cpp field_bgm_director): a MusicList field song was
  // started and is still considered live (was a function-local static — wrong under two Games).
  int field_bgm_started = 0;

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
           hle.game = this; rq.game = this; pcSched.game = this;
           spu_audio.game = this; native_music.game = this; music_list.game = this;
           rml_overlay.game = this; platform_hle.game = this; memcard.game = this;
           dbg_server.game = this; verify.core = &core; ffspan.core = &core;
           spu_state = SPU_NewState(); mdec_state = MDEC_NewState();
           cdc_state_init(&cdc); xa_state_init(&xa); }   // per-instance CD-controller + XA streamer defaults
  ~Game() { SPU_FreeState(spu_state); MDEC_FreeState(mdec_state); }
};

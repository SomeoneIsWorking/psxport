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
#include "cd.h"        // class Cd — native CD subsystem (sync reads + libcd HLE + music state)
#include "cdc_state.h" // CdcState — per-instance native CD-controller register model (cdc_native.c)
#include "core.h"
#include "dbg_server.h"          // class DbgServer — live TCP debug endpoint (127.0.0.1)
#include "disc.h"                // DiscState — native by-LBA CHD disc backend (disc.c)
#include "fps60.h"               // Fps60 — the interpolated-60fps tier's per-instance state
#include "gpu_native_internal.h" // GpuState — the native GPU's per-instance render machine state
#include "gpu_perf.h"            // class GpuPerf — per-frame CPU phase profiler (`debug perf`)
#include "gpu_vk_device.h"       // class GpuDevice — the SDL3 GPU host window/device (first Game claims it)
#include "gpu_vk_internal.h"     // GpuVkState — the Vulkan present backend's per-instance render state
#include "gte_state.h"           // GteRegs — per-instance GTE (COP2) register file (Beetle gte.c)
#include "hle.h"                 // class Hle — BIOS HLE (events, heap, work area, A0/B0/C0 dispatch)
#include "mdec_device.h"         // class MdecDevice — per-instance MDEC state handle (Beetle mdec.c)
#include "memcard.h"             // class Memcard — host-backed 128 KB memory card device
#include "mods.h"                // class Mods — per-Game PC-native mod toggles (aspect/ires/ssao/light/fps60)
#include "native_fmv.h"          // class Fmv — native .STR movie player
#include "native_gate.h"         // class NativeGates — PC-native-layer A/B GATE registry (REPL diag)
#include "native_stub.h"         // class BootStub — SCEA splash + MAIN.EXE LoadExec hand-off
#include "pad_input.h"           // class Pad — native controller input + REPL drive
#include "platform_hle.h"        // class PlatformHle — HW-sync HLE table (VSync/CdSync/…)
#include "render_queue.h"        // RenderQueue — the engine-owned draw-order authority
#include "repl.h"                // class Repl — REPL driver + auto-drive request state
#include "rmlui_overlay.h"       // class RmlOverlay — mod/debug HTML UI + world readout HUD
#include "spu_audio.h"           // class SpuAudio — host audio output sink (SDL3 + WAV capture)
#include "spu_device.h"          // class SpuDevice — per-instance SPU state handle (Beetle spu.c)
#include "timing.h"              // class Timing — native VBlank/VSync frame clock
#include "verify_harness.h"      // class VerifyHarness — shared A/B verify scaffold (game/core)
#include "xa_state.h"            // XaState  — per-instance native XA-ADPCM CD-audio streamer (xa_stream.c)

class Sbs; // forward decl — Game holds `sbs` back-pointer set by Sbs::run
#include <setjmp.h>
#include <stdint.h>

// CdcState / XaState — native CD-controller register model + XA-ADPCM streamer, PER-INSTANCE so two
// cores (native vs PSX-recomp) keep SEPARATE CD state. Plain-C structs in their own headers (like
// gte_state.h's GteRegs) so cdc_native.c / xa_stream.c stay C. cdc_read/cdc_write take &game->cdc
// explicitly; the XA streamer is bound per frame-step via xa_bind (vendor SPU pull — see xa_state.h).

#include "pc_scheduler.h" // class PcScheduler — the PC-native cooperative task scheduler
                          // (per-task contexts, run flags, step-spread counters, stanza dispatch)

class Game {
public:
  Core core; // CPU registers + 2 MB main RAM + 1 KB scratchpad (was the sole instance object)

  // ---- migrated subsystem state (one member per migrated subsystem) ----
  Timing timing;
  Cd cd;          // native CD subsystem: sync reads + libcd HLE + deferred-music state (cd_override.cpp)
  DiscState disc; // native CHD disc backend: handle + hunk cache (per-instance; disc.c)
  CdcState cdc;   // native CD-controller register model (per-instance; cdc_native.c, explicit param)
  XaState xa;     // native XA-ADPCM CD-audio/voice streamer (per-instance; xa_stream.c, bound via xa_bind)
  Hle hle;
  Pad pad;
  Repl repl;           // interactive REPL driver + REPL-armed auto-drive requests (repl.cpp)
  Fmv fmv;             // native .STR movie player (native_fmv.cpp)
  BootStub stub;       // SCEA splash + MAIN.EXE LoadExec hand-off (native_stub.cpp)
  PcScheduler pcSched; // native cooperative task scheduler (game/core/pc_scheduler.cpp)
  GpuState gpu;        // native GPU: VRAM + draw/display state + the rasterizer (gpu_native.cpp)
  GpuVkState gpu_vk;   // Vulkan present backend: per-frame batch/depth/dirty/present state (gpu_vk.cpp)
  GpuDevice gpu_dev;   // SDL3 GPU host device/window/pipelines (ONE per process; first Game claims it)
  RenderQueue rq;      // engine-owned render queue: the single draw-ORDER authority (render_queue.cpp)
  // Tier-1 capture-target redirect (docs/fps60-rework.md "Object-tier attempt ... Why Tier 1 isn't
  // built"): non-null ONLY while Fps60::present_vk re-invokes Render::terrainRenderAll() at the interp
  // present under a lerped camera. native_terrain.cpp's drawWorldQuad call checks this and, when set,
  // writes into the ISOLATED sink instead of the live `rq` — the live queue (about to be reused by the
  // next real drawOTag) is never touched by the present-time re-render. Always null outside that window.
  RenderQueue *rqRedirect = nullptr;
  // The ACTIVE render queue every emit choke should target: the isolated sink while a present-time
  // re-render is in flight (rqRedirect set), else the live `rq`. Unifies the redirect so the OT-walk's
  // own emits (gpu_native.cpp emitOrQueue) can be captured into mSink too (fps60 unified-path step 2a) —
  // native_terrain already checked rqRedirect; this makes it the ONE mechanism. Byte-identical when
  // rqRedirect==null (every non-present-re-run path).
  RenderQueue &activeRq() {
    return rqRedirect ? *rqRedirect : rq;
  }
  Fps60 fps60;        // interpolated-60fps tier: capture buffers + matcher + remap (fps60.cpp)
  SpuAudio spu_audio; // host audio output sink (SDL3 device + optional WAV capture)
  // The game's SEP/VAB in-game music player + Sound Test catalogue live game-side on TombaCtx now;
  // the framework SPU sink + mod-UI HUD reach them through the audioMixFrame / audioNowPlayingName /
  // audioSoundTestPlay GameHooks (game_iface.h), so no framework member names a game audio type.
  RmlOverlay rml_overlay;   // in-app mod/debug HTML UI + live world-position HUD
  NativeGates native_gates; // A/B gate registry: `native <name> on|off` diag (music/seqtick/…)
  PlatformHle platform_hle; // HW-sync HLE dispatch table (VSync/CdSync/MDEC/ChangeThread)
  Memcard memcard;          // host-backed 128 KB memory card device (BIOS libcard/libmcrd)
  Mods mods;                // per-Game mod toggles + params (was the process-global g_mods, 2026-07-10)
  DbgServer dbg_server;     // live TCP debug endpoint (PSXPORT_DEBUG_SERVER=<port>)
  GpuPerf perf;             // per-frame CPU phase / frame-time profiler (REPL `debug perf`)
  VerifyHarness verify;     // shared A/B verify scaffold: snapshot buffers + per-check counters
  Sbs *sbs = nullptr;       // SBS harness back-pointer (nullptr in standalone; set by Sbs::run)
  GteRegs gte{};            // GTE (COP2) register file — per-instance so two cores keep SEPARATE GTE state
                            // (Beetle gte.c bound to this via GTE_BindState; see gte_bind, gte_beetle.cpp)
  // (native-depth cache moved to `class ProjPrim` on Render — reach as `c->rsub.projprim`, 2026-07-03)
  SpuDevice spu;   // per-instance SPU device (Beetle spu.c handle + SBS write log); bound via spu.bind()
  MdecDevice mdec; // per-instance MDEC device (Beetle mdec.c handle); bound via mdec.bind()

  // ---- PSX-fallback gate (diagnostic, user 2026-06-23) --------------------------------------------
  // ONE switch: keep BOOT (native crt0/FMV/init) and the FRAME LOOP skeleton native, but run EVERYTHING
  // the frame loop calls — the stage state machines, all asset/area LOADING, and content — as the PSX
  // RECOMP body instead of the native owners. CD reads still go through the platform CD layer (cd_override
  // cd_loadfile/cd_dc40/cd_async_read), so the PSX loaders run SYNCHRONOUSLY (no busy-wait). This
  // restores a working PSX baseline to compare the native path against. Set by PSXPORT_GATE (nonzero) and
  // the REPL `gate on|off`. Default OFF = full native (shipped behavior). Wired in native_boot.cpp.
  int psx_fallback = 0;

  // ---- PSXPORT_ORACLE / SBS core B: the pure PSX reference, PER-GAME ------------------------------
  // oracle=1 pins THIS Game to the trustworthy reference config: recomp gameplay + no native
  // enhancement may touch it. Seeded from PSXPORT_ORACLE in native_boot_run (standalone) or set by
  // the SBS harness on core B — per-instance, so one process can hold a user-config core and a pure
  // oracle core without leaking into each other (was the process-global oracle_mode() cfg predicate).
  // The render-side flag (Render::mode.setPsxRender) is set by the caller, which owns mRender.
  int oracle = 0;
  void setOracle() {
    oracle = 1;
    psx_fallback = 1;
    mods.forceNeutral();
  }

  // Product execution is always native + synchronous. This bit is retained only as an internal SBS
  // discriminator while the old byte-exact native mirrors are retired: ordinary Game construction
  // leaves it true and no user-facing option changes it. The explicit oracle is psx_fallback, which
  // runs the generated substrate rather than inventing another native cadence.
  bool native_sync = true;

  // Field BGM director latch (MusicCoord::fieldBgmDirector): a MusicList field song was
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
  // submit.cpp) is in the SHARED dispatch table; each core decides ON vs neutralized by reading
  // THIS flag (not divergent override tables — see docs game-deglobalize-plan P7). The harness sets it on
  // the `b` core (terrain -> recomp super-call) so an a-vs-b core.ram diff isolates submit_terrain.
  int neutralize_terrain = 0;

  // core.game / gpu.game / gpu_vk.game are back-pointers so a subsystem holding one of those handles can
  // reach the rest of the machine (e.g. blit_src -> gpu_vk via gpu.game; frame_via_fb -> s_seen3d via
  // gpu_vk.game->core). Set once here so no file-scope global is needed.
  Game() {
    core.game = this;
    gpu.game = this;
    gpu_vk.game = this;
    timing.game = this;
    pad.game = this;
    fps60.game = this;
    hle.game = this;
    rq.game = this;
    pcSched.game = this;
    cd.game = this;
    fmv.game = this;
    stub.game = this;
    spu_audio.game = this;
    rml_overlay.game = this;
    platform_hle.game = this;
    memcard.game = this;
    dbg_server.game = this;
    verify.core = &core;
    if (!GpuDevice::sInstance) {
      GpuDevice::sInstance = &gpu_dev; // first Game claims the host device
    }
    mods.init(); // per-Game mod state: factory defaults + the player's settings file
    disc_state_init(&disc);
    cdc_state_init(&cdc);
    timing.bindCdcClock(&cdc);
    xa_state_init(&xa);
    gte.dbg.sxhist_on = gte.dbg.gteprobe = gte.dbg.projprobe = gte.dbg.rtpcaller_on = -1;
    // core.cfg is already valid here: Core is a MEMBER, so its constructor (which snapshots
    // the installed GameConfig) runs before this body. Must follow disc_state_init, which
    // memsets the struct.
    disc.env_key = core.cfg ? core.cfg->discEnvVar : 0; // GameConfig::discEnvVar
    cdc.disc = &disc;
    xa.disc = &disc;
  } // per-instance disc backend + CD-controller + XA streamer
};

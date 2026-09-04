// Game — the WHOLE machine as ONE per-instance object, so NOTHING is a file-scope global.
//
// Game owns one complete runtime instance. Core carries a back-pointer so subsystem code can reach
// the machine state without process-global mutable state.
#pragma once
#include "cd.h"        // class Cd — native CD subsystem (sync reads + libcd HLE + music state)
#include "cdc_state.h" // CdcState — per-instance native CD-controller register model (cdc_native.c)
#include "core.h"
#include "dbg_server.h"                  // class DbgServer — live TCP debug endpoint (127.0.0.1)
#include "disc.h"                        // DiscState — native by-LBA CHD disc backend (disc.c)
#include "dma_callbacks.h"               // DmaCallbackRegistry — direct-runtime DMACallback state
#include "frame_presenter.h"             // FramePresenter — neutral current-frame capture/present/cadence fence
#include "game_runtime.h"                // GameRuntime + per-Game polymorphic behavior products
#include "gpu_native_internal.h"         // GpuState — the native GPU's per-instance render machine state
#include "gpu_perf.h"                    // class GpuPerf — per-frame CPU phase profiler (`debug perf`)
#include "gpu_vk_device.h"               // class GpuDevice — the SDL3 GPU host window/device (first Game claims it)
#include "gpu_vk_internal.h"             // GpuVkState — the Vulkan present backend's per-instance render state
#include "gte_state.h"                   // GteRegs — per-instance GTE (COP2) register file (Beetle gte.c)
#include "guest_widescreen_projection.h" // title-owned guest projection + latched presentation extent
#include "hle.h"                         // class Hle — BIOS HLE (events, heap, work area, A0/B0/C0 dispatch)
#include "mdec_device.h"                 // class MdecDevice — per-instance MDEC state handle (Beetle mdec.c)
#include "memcard.h"                     // class Memcard — host-backed 128 KB memory card device
#include "mods.h"                        // class Mods — per-Game PC-native mod toggles (aspect/ires/ssao/light/fps60)
#include "native_fmv.h"                  // class Fmv — native .STR movie player
#include "native_stub.h"                 // class BootStub — SCEA splash + MAIN.EXE LoadExec hand-off
#include "pad_input.h"                   // class Pad — native controller input + REPL drive
#include "platform_hle.h"                // class PlatformHle — HW-sync HLE table (VSync/CdSync/…)
#include "render_queue.h"                // RenderQueue — the engine-owned draw-order authority
#include "repl.h"                        // class Repl — REPL driver + title-owned request state
#include "rmlui_overlay.h"               // class RmlOverlay — mod/debug HTML UI + world readout HUD
#include "sio_pad.h"                     // class Sio0 — the controller port and the device on it
#include "spu_audio.h"                   // class SpuAudio — host audio output sink (SDL3 + WAV capture)
#include "spu_device.h"                  // class SpuDevice — per-instance SPU state handle (Beetle spu.c)
#include "timing.h"                      // class Timing — native VBlank/VSync frame clock
#include "xa_state.h"                    // XaState  — per-instance native XA-ADPCM CD-audio streamer (xa_stream.c)

class FrameLoopShell;
#include <setjmp.h>
#include <stdint.h>

// CdcState / XaState — native CD-controller register model + XA-ADPCM streamer, PER-INSTANCE so two
// cores (native vs guest) keep SEPARATE CD state. Plain-C structs in their own headers (like
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
  Sio0 sio; // controller port (SIO0) hardware: the pad protocol and its transfer/ack deadlines
  DmaCallbackRegistry dmaCallbacks;
  Pad pad;
  Repl repl;                           // interactive REPL driver + title-consumed requests (repl.cpp)
  Fmv fmv;                             // native .STR movie player (native_fmv.cpp)
  BootStub stub;                       // SCEA splash + MAIN.EXE LoadExec hand-off (native_stub.cpp)
  PcScheduler pcSched;                 // native cooperative task scheduler (game/core/pc_scheduler.cpp)
  GpuState gpu;                        // native GPU: VRAM + draw/display state + the rasterizer (gpu_native.cpp)
  GpuVkState gpu_vk;                   // Vulkan present backend: per-frame batch/depth/dirty/present state (gpu_vk.cpp)
  GpuDevice gpu_dev;                   // SDL3 GPU host device/window/pipelines (ONE per process; first Game claims it)
  RenderQueue rq;                      // engine-owned render queue: the single draw-ORDER authority (render_queue.cpp)
  FramePresenter presentation;         // non-temporal current-frame fence, present and pacing owner
  GuestPresentationState guestDisplay; // latched only when the title publishes matching guest projection
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
  SpuAudio spu_audio; // host audio output sink (SDL3 device + optional WAV capture)
  // The game's SEP/VAB in-game music player + Sound Test catalogue live game-side on TombaCtx now;
  // the framework SPU sink + mod-UI HUD reach them through the audioMixFrame / audioNowPlayingName /
  // audioSoundTestPlay GameHooks (game_iface.h), so no framework member names a game audio type.
  RmlOverlay rml_overlay;   // in-app mod/debug HTML UI + live world-position HUD
  PlatformHle platform_hle; // HW-sync HLE dispatch table (VSync/CdSync/MDEC/ChangeThread)
  Memcard memcard;          // host-backed 128 KB memory card device (BIOS libcard/libmcrd)
  Mods mods;                // per-Game mod toggles + params (was the process-global g_mods, 2026-07-10)
  DbgServer dbg_server;     // live TCP debug endpoint (PSXPORT_DEBUG_SERVER=<port>)
  GpuPerf perf;             // per-frame CPU phase / frame-time profiler (REPL `debug perf`)
  GteRegs gte{};            // GTE (COP2) register file — per-instance so two cores keep SEPARATE GTE state
                            // (Beetle gte.c bound to this via GTE_BindState; see gte_bind, gte_beetle.cpp)
  // (native-depth cache moved to `class ProjPrim` on Render — reach as `c->rsub.projprim`, 2026-07-03)
  SpuDevice spu;   // per-instance SPU device (Beetle spu.c handle + SBS write log); bound via spu.bind()
  MdecDevice mdec; // per-instance MDEC device (Beetle mdec.c handle); bound via mdec.bind()

  // Field BGM director latch (MusicCoord::fieldBgmDirector): a MusicList field song was
  // started and is still considered live (was a function-local static — wrong under two Games).
  int field_bgm_started = 0;

  // Declared after the subsystem state so the products are destroyed first. A derived driver or
  // scheduler may retain references to the fully wired subsystem members it receives at creation.
  GameRuntime *runtime = nullptr;
  std::unique_ptr<TemporalFramePresentation> temporalPresentation;
  std::unique_ptr<FrameDriver> frameDriver;
  std::unique_ptr<TaskScheduler> taskScheduler;

  // core.game / gpu.game / gpu_vk.game are back-pointers so a subsystem holding one of those handles can
  // reach the rest of the machine (e.g. blit_src -> gpu_vk via gpu.game; frame_via_fb -> s_seen3d via
  // gpu_vk.game->core). Set once here so no file-scope global is needed.
  Game();
  ~Game();

private:
  friend class FrameLoopShell;
  bool productFrameLoopPrepared_ = false;
};

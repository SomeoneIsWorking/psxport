// Game — the WHOLE machine as ONE per-instance object, so NOTHING is a file-scope global.
//
// Game owns one complete runtime instance. Core carries a back-pointer so subsystem code can reach
// the machine state without process-global mutable state.
#pragma once
#include "bug_report_session.h" // psx::debug::BugReportSession — the B-key in-app bug report
#include "cd.h"                 // class Cd — native CD subsystem (sync reads + libcd HLE + music state)
#include "cdc_state.h" // psx::cd::CdController — per-instance native CD-controller register model (cdc_native.c)
#include "core.h"
#include "dbg_server.h"        // class DbgServer — live TCP debug endpoint (127.0.0.1)
#include "disc.h"              // psx::cd::Disc — native by-LBA CHD disc backend (disc.c)
#include "display_presenter.h" // psx::present::DisplayPresenter — the guest display to the screen
#include "dma_callbacks.h"     // DmaCallbackRegistry — direct-runtime DMACallback state
#include "field_digest.h"
#include "frame_loop_shell.h" // psx::frame::FrameLoopShell — the per-field shell a title's own loop composes
#include "frame_pacer.h"
#include "frame_presenter.h" // psx::frame::FramePresenter — neutral current-frame capture/present/cadence fence
#include "game_runtime.h"    // GameRuntime + per-Game polymorphic behavior products
#include "gpu_perf.h"        // class GpuPerf — per-frame CPU phase profiler (`debug perf`)
#include "gte_state.h"       // GteRegs — per-instance GTE (COP2) register file (Beetle gte.c)
#include "guest_widescreen_projection.h" // title-owned guest projection + latched presentation extent
#include "hle.h"                         // class Hle — BIOS HLE (events, heap, work area, A0/B0/C0 dispatch)
#include "host_input.h"                  // psx::input::HostInput — the host keyboard/controllers/event drain
#include "mdec_device.h"                 // class MdecDevice — per-instance MDEC state handle (Beetle mdec.c)
#include "memcard.h"                     // class Memcard — host-backed 128 KB memory card device
#include "mods.h"                        // class Mods — per-Game PC-native mod toggles
#include "native_fmv.h"                  // class Fmv — native .STR movie player
#include "pad_input.h"                   // class Pad — native controller input + REPL drive
#include "platform_hle.h"                // class PlatformHle — HW-sync HLE table (VSync/CdSync/…)
#include "repl.h"                        // class Repl — REPL driver + title-owned request state
#include "rmlui_overlay.h"               // class RmlOverlay — mod/debug HTML UI + world readout HUD
#include "session_control.h"             // psx::frame::SessionControl — "return to the host's selector" request
#include "session_run.h"                 // psx::host::SessionRun — a title's request to end its run
#include "sio_pad.h"                     // class Sio0 — the controller port and the device on it
#include "snapshot.h"                    // psx::debug::Snapshotter — on-demand guest RAM captures
#include "spu_audio.h"                   // class SpuAudio — host audio output sink (SDL3 + WAV capture)
#include "spu_device.h"                  // class SpuDevice — per-instance SPU state handle (Beetle spu.c)
#include "timing.h"                      // class psx::frame::Timing — native VBlank/VSync frame clock
#include "typed_config.h"                // psx::config::Config — the immutable launch configuration
#include "xa_state.h"                    // psx::audio::XaStream — the XA-ADPCM CD-audio streamer

class GpuDevice;
struct GpuVkState;

class Game {
public:
  const psx::config::Config config; // the launch configuration, fixed for this Game's life
  Core core;                        // CPU registers + 2 MB main RAM + 1 KB scratchpad (was the sole instance object)

  // ---- migrated subsystem state (one member per migrated subsystem) ----
  psx::frame::Timing timing;
  psx::frame::FramePacer framePacer;
  psx::diag::FieldDigest fieldDigest; // per-field determinism trace (`PSXPORT_DEBUG=fielddigest`)
  Cd cd;                              // native CD subsystem: the stock libcd state (stock_libcd.cpp)
  psx::cd::Disc disc;                 // the title's CHD: handle, hunk cache and TOC
  psx::cd::CdController cdc;          // the CD-controller register model
  psx::audio::XaStream xa;            // XA-ADPCM CD-audio/voice streamer, bound per frame step for the SPU pull
  psx::debug::Snapshotter snapshots;  // scheduled and SIGUSR1 guest RAM captures
  Hle hle;
  Sio0 sio; // controller port (SIO0) hardware: the pad protocol and its transfer/ack deadlines
  DmaCallbackRegistry dmaCallbacks;
  // The host input owner, constructed before the pad it feeds: the SDL event drain, the delivered
  // key state, the gamepads and the active-low mask they add up to. Every pump site — the pad frame
  // service, the SCEA splash, a blocking movie, the debug-server pause wait, and the present path's
  // own window drain — reaches the host through this one object.
  psx::input::HostInput hostInput;
  Pad pad{hostInput};
  Repl repl;                                    // interactive REPL driver + title-consumed requests (repl.cpp)
  Fmv fmv;                                      // native .STR movie player (native_fmv.cpp)
  psx::present::DisplayPresenter display{core}; // the guest display to the screen, once per present
  std::unique_ptr<GpuVkState> gpuVk_;           // this Game's present targets on the SDL GPU device
  GpuVkState &gpuVk() {
    return *gpuVk_;
  }

  // The SDL3 GPU host window/device/pipelines: the ONE presentation device of the process. A Game
  // presents through it; it does not have to OWN it. A host that runs several Games in one process — a
  // title selector, then a title, then the selector again — passes the same device to every one of them
  // (see psxport::HostPresentation), because a window destroyed and re-created between sessions is what
  // made the selector look like a separate program. `Game(GameRuntime&)` owns a device
  // of its own, and `Game(GameRuntime&, GpuDevice&)` presents through one the caller owns and outlives this Game.
  //
  // `ownedGpuDevice` is declared FIRST so it is constructed before the reference binds to it, and LAST
  // so it is destroyed last: a self-owning Game's device outlives every per-Game GPU target rather than
  // the other way round.
  std::unique_ptr<GpuDevice> ownedGpuDevice;
  GpuDevice &gpu_dev;

  psx::frame::FramePresenter presentation; // non-temporal current-frame fence, present and pacing owner
  GuestPresentationState guestDisplay;     // latched only when the title publishes matching guest projection
  // A resume replay or a host pre-roll: the pacer does not sleep, movies run uncapped, audio is dropped.
  bool fastForwarding() const {
    return pad.fastForwarding() || session.preRolling();
  }
  SpuAudio spu_audio;                 // host audio output sink (SDL3 device + optional WAV capture)
  RmlOverlay rml_overlay;             // in-app mod/debug HTML UI + live world-position HUD
  PlatformHle platform_hle;           // HW-sync HLE dispatch table (VSync/CdSync/MDEC)
  Memcard memcard;                    // host-backed 128 KB memory card device (BIOS libcard/libmcrd)
  Mods mods;                          // per-Game mod toggles + params (was the process-global g_mods, 2026-07-10)
  psx::host::SessionRun run;          // whether the title asked to end its run
  psx::frame::SessionControl session; // host-level request to end this Game and show the selector (session_control.h)
  DbgServer dbg_server;               // live TCP debug endpoint (PSXPORT_DEBUG_SERVER=<port>)
  psx::debug::BugReportSession bugReport; // B key: freeze, capture, write and save a bug report
  GpuPerf perf;                           // per-frame CPU phase / frame-time profiler (REPL `debug perf`)
  SpuDevice spu;   // per-instance SPU device (Beetle spu.c handle + SBS write log); bound via spu.bind()
  MdecDevice mdec; // per-instance MDEC device (Beetle mdec.c handle); bound via mdec.bind()

  // The title's runtime, shared with core.runtime; the caller owns it and it outlives this Game.
  psx::game::GameRuntime &runtime;

  // The title's card and user-data names; fields are null when the title declares none.
  psx::game::HostIdentity hostIdentity() const;
  // Declared after the subsystem state so the products are destroyed first. A derived driver may
  // retain references to the fully wired subsystem members it receives at creation.
  std::unique_ptr<psx::game::TitleSessionState> sessionState;
  std::unique_ptr<psx::game::FrameDriver> frameDriver;

  // core.game / gpuVk().game are back-pointers so a subsystem holding one can reach the rest of the machine.
  Game(psx::game::GameRuntime &runtime, const psx::config::Config &config);
  // Present through a presentation device the CALLER owns and outlives this Game; this Game never
  // releases it, so every Game in a host-lifetime-window product shares one window and one device.
  Game(psx::game::GameRuntime &runtime, GpuDevice &presentation, const psx::config::Config &config);
  ~Game();
  // Present nothing: no window or GPU device is brought up.
  void disablePresentation();
  // Return every process-wide peripheral bind point that still names this Game to its default.
  void releaseHardwareBindings();

private:
  // The body both constructors share: the wiring that does not depend on which device this Game uses.
  void wireRuntimeMembers();
  friend class psx::frame::FrameLoopShell;
  bool productFrameLoopPrepared_ = false;
};

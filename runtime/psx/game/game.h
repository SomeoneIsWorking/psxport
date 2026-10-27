// Game: one complete machine instance; Core carries a back-pointer to it.
#pragma once
#include "bug_report_session.h"
#include "cd.h"
#include "cdc_state.h"
#include "core.h"
#include "dbg_server.h"
#include "debug_commands.h"
#include "disc.h"
#include "display_presenter.h"
#include "dma_callbacks.h"
#include "field_digest.h"
#include "frame_loop_shell.h"
#include "frame_pacer.h"
#include "frame_presenter.h"
#include "game_runtime.h"
#include "gpu_perf.h"
#include "gte_state.h"
#include "guest_widescreen_projection.h"
#include "hle.h"
#include "host_input.h"
#include "mdec_device.h"
#include "memcard.h"
#include "mods.h"
#include "native_fmv.h"
#include "owner_journal.h"
#include "pad_frame_probes.h"
#include "pad_input.h"
#include "platform_hle.h"
#include "repl.h"
#include "rmlui_overlay.h"
#include "session_control.h"
#include "session_run.h"
#include "sio_pad.h"
#include "snapshot.h"
#include "spu_audio.h"
#include "spu_device.h"
#include "timing.h"
#include "title_catalog.h"
#include "typed_config.h"
#include "xa_state.h"

namespace psx::render {
class RenderDevice;
class RenderState;
} // namespace psx::render

class Game {
public:
  const psx::config::Config config; // the launch configuration, fixed for this Game's life
  // The process's one window and device. A host running several Games passes the same one to each; a
  // Game made without one owns it. Declared before every user so it is destroyed after them.
  std::unique_ptr<psx::render::RenderDevice> ownedRenderDevice;
  psx::render::RenderDevice &renderDevice;
  // The window's host input, shared the same way and declared before the pad that reads it.
  std::unique_ptr<psx::input::HostInput> ownedHostInput;
  Core core; // CPU registers + 2 MB main RAM + 1 KB scratchpad (was the sole instance object)

  // ---- migrated subsystem state (one member per migrated subsystem) ----
  psx::frame::Timing timing;
  psx::frame::FramePacer framePacer;
  psx::diag::FieldDigest fieldDigest; // per-field determinism trace (`PSXPORT_DEBUG=fielddigest`)
  Cd cd;                              // native CD subsystem: the stock libcd state (stock_libcd.cpp)
  psx::cd::Disc disc;                 // the title's CHD: handle, hunk cache and TOC
  psx::cd::CdController cdc;          // the CD-controller register model
  psx::audio::XaStream xa;            // XA-ADPCM CD-audio/voice streamer, bound per frame step for the SPU pull
  psx::debug::Snapshotter snapshots;  // scheduled and SIGUSR1 guest RAM captures
  psx::debug::PadFrameProbes padProbes;
  Hle hle;
  Sio0 sio; // controller port (SIO0) hardware: the pad protocol and its transfer/ack deadlines
  DmaCallbackRegistry dmaCallbacks;
  // The process's host input: the SDL event drain, the delivered key state, the gamepads and the
  // active-low mask they add up to. Every pump site — the pad frame service, a blocking movie, the
  // debug-server pause wait, and the present path's own window drain — reaches the host through it.
  psx::input::HostInput &hostInput;
  Pad pad{hostInput};
  Repl repl;                                         // interactive REPL driver + title-consumed requests (repl.cpp)
  Fmv fmv;                                           // native .STR movie player (native_fmv.cpp)
  psx::present::DisplayPresenter display{core};      // the guest display to the screen, once per present
  std::unique_ptr<psx::render::RenderState> render_; // this Game's present targets on renderDevice
  psx::render::RenderState &render() {
    return *render_;
  }

  psx::frame::FramePresenter presentation;           // non-temporal current-frame fence, present and pacing owner
  psx::present::OwnerJournal ownerJournal;           // draws with no owner and held intervals, per screen
  psx::present::GuestPresentationState guestDisplay; // latched only when the title publishes matching guest projection
  // A resume replay or a host pre-roll: the pacer does not sleep, movies run uncapped, audio is dropped.
  bool fastForwarding() const {
    return pad.fastForwarding() || session.preRolling();
  }
  SpuAudio spu_audio;       // host audio output sink (SDL3 device + optional WAV capture)
  RmlOverlay rml_overlay;   // in-app mod/debug HTML UI + live world-position HUD
  PlatformHle platform_hle; // HW-sync HLE dispatch table (VSync/CdSync/MDEC)
  Memcard memcard;          // host-backed 128 KB memory card device (BIOS libcard/libmcrd)
  Mods mods;
  psx::host::SessionRun run;          // whether the title asked to end its run
  psx::frame::SessionControl session; // host-level request to end this Game and show the selector (session_control.h)
  DbgServer dbg_server;               // live TCP debug endpoint (PSXPORT_DEBUG_SERVER=<port>)
  psx::debug::DebugCommands debugCommands; // the verbs the REPL and the debug server share
  psx::debug::BugReportSession bugReport;  // B key: freeze, capture, write and save a bug report
  GpuPerf perf;                            // per-frame CPU phase / frame-time profiler (REPL `debug perf`)
  SpuDevice spu;   // per-instance SPU device (Beetle spu.c handle + SBS write log); bound via spu.bind()
  MdecDevice mdec; // per-instance MDEC device (Beetle mdec.c handle); bound via mdec.bind()

  // The title's runtime, shared with core.runtime; the caller owns it and it outlives this Game.
  psx::game::GameRuntime &runtime;

  // The product this Game runs in and the catalog title it runs; null for the selector.
  std::string_view productName;
  const psx::host::TitleIdentity *title = nullptr;
  // The authenticated executable the title booted from; empty for the selector.
  std::filesystem::path executable;

  // The title's card and user-data names; fields are null when the title declares none.
  psx::game::HostIdentity hostIdentity() const;
  // Declared after the subsystem state so the products are destroyed first. A derived driver may
  // retain references to the fully wired subsystem members it receives at creation.
  std::unique_ptr<psx::game::TitleSessionState> sessionState;
  std::unique_ptr<psx::game::FrameDriver> frameDriver;
  // The installed driver as the title's own type; aborts when none is installed.
  template <class T> T &frameDriverAs() {
    return static_cast<T &>(requireFrameDriver());
  }
  template <class T> const T &frameDriverAs() const {
    return static_cast<const T &>(requireFrameDriver());
  }
  // The installed driver as `T`, or null when none of that type is installed.
  template <class T> T *frameDriverIf() {
    return dynamic_cast<T *>(frameDriver.get());
  }
  template <class T> const T *frameDriverIf() const {
    return dynamic_cast<const T *>(frameDriver.get());
  }
  psx::game::FrameDriver &requireFrameDriver() const;

  // core.game is a back-pointer so a subsystem holding one can reach the rest of the machine.
  Game(psx::game::GameRuntime &runtime, const psx::config::Config &config);
  // Present through a device and read a host input the caller owns and keeps alive past this Game.
  Game(psx::game::GameRuntime &runtime,
       psx::render::RenderDevice &presentation,
       psx::input::HostInput &input,
       const psx::config::Config &config);
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

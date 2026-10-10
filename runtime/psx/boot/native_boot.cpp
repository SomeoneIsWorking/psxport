// PC-PSX hybrid native boot and host-loop orchestration.
//
// Architecture: the host owns product iteration and delegates exactly one finite frame through
// psx::frame::FrameLoopShell to the title-created FrameDriver. Title state-machine, input, audio, render, and
// present ordering does not live here. This file retains generic crt0/boot, diagnostics, REPL pause,
// watchdog, and frame-budget scaffolding around that delegation.
#include "c_subsys.h"
#include "cfg.h"
#include "config_vars.h" // psx::config::render_path() / cv_repl — knobs through the CVar ladder
#include "core.h"
#include "dbg_server.h" // debug_server_port — the one reading of PSXPORT_DEBUG_SERVER
#include "field_turn.h" // psx::frame::FieldTurn — the per-field services this loop owes
#include "frame_loop_shell.h"
#include "game.h"
#include "game_iface.h"
#include "gpu_vk.h" // gpu_vk_windowed — the windowed/headless discriminator
#include "guest_call.h"
#include "hw_bind.h" // spu_bind/mdec_bind/xa_bind (per-instance HW-peripheral binders)
#include "machine.h" // psx::Machine — the one boot composition this spine and every title share
#include "mods.h"
#include "ot_attr.h" // OtAttr — the producer-census tables (armed by Game's ctor, game.cpp)
#include "repl.h"
// (the REPORT is emitted by ~LightrecExecutor, on every exit path)
#include <lucent/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h> // usleep (debug-server pause/step idle wait)
                    // class Repl — REPL driver + title-owned request state (per-Core, on Game)

static void game_main(Core *c);

// Native crt0 implementation of recovered FUN_800896E0 behavior: clear BSS, initialize the heap,
// then call game_main. The libc/heap initialization at 0x80089860 remains a guest call.
static void native_crt0(Core *c) {
  psx::Machine{*c->game}.setupGuestBoot();
  game_main(c);
}

// Init prefix + task-0 bootstrap (everything FUN_80050b08 does before its scheduler loop). Factored out
// of game_main so the dual-core harness can init two cores then drive the frame loop itself.
static void game_init(Core *c) {
  // The game's boot-init prologue (FUN_80050b08 init prefix + task-0 bootstrap) is GAME behaviour: it
  // moved WHOLE to the game side (game/core/game_hooks.cpp tomba_bootInit) via GameRuntime, so
  // this framework file no longer bakes in the game's guest boot-prologue addresses or c->engine.* init
  // calls. It moves as ONE unit (not just the engine calls) because the engine calls are interleaved
  // with guest leaves and task0Bootstrap depends on the scheduler-table init between
  // them — the order is load-bearing and cannot be split. crt0_setup + the per-core binds (this file)
  // stay framework scaffolding.
  c->runtime->bootInit(*c);
}

// Dual-core harness hooks (dualcore.cpp / selftest.cpp / sbs.cpp): boot a core to the start of the
// frame loop, then step it one frame at a time. dc_boot_init = crt0 setup + the init prefix/bootstrap;
// dc_step_frame = one frame.
//
// Register native overrides before crt0 or title initialization because either may dispatch guest
// calls. Every boot route constructs and initializes its own Game, so per-Game native and
// hardware-service tables are populated here. Render policy is likewise resolved at the shared Core
// setup boundary; a harness may deliberately replace it after this call.
void dc_boot_init(Core *c) {
  psx::Machine machine{*c->game};
  machine.reportConfigurationOnce();
  machine.bindSession();
  c->runtime->registerOverrides(*c->game);
  // Harnesses construct their own Game objects, so every per-Game hardware-service table must be
  // populated here as well as on the standalone main() path. Register the CD command/read seams
  // before the generic BIOS-library waits, matching the standalone boot order.
  c->game->cd.overridesInit();
  machine.prepareProduct();
  machine.setupGuestBoot();
  game_init(c);
}
void dc_step_frame(Core *c, uint32_t f) {
  psx::frame::FrameLoopShell{}.step(*c, f);
}

static void game_main(Core *c) {
  // The boot composition is `psx::Machine`'s, step for step, exactly as a title-owned spine calls it.
  // This spine adds no step of its own: the ones it needs and the ones a title needs are the same ones,
  // which is what makes "the product that boots natively" and "the product that owns its loop" the
  // same sequence rather than two that agree today.
  psx::Machine machine{*c->game};
  // Arm the config audit before title initialization. report_once() is idempotent; the complete dump
  // remains after the loop for bounded diagnostic runs.
  machine.reportConfigurationOnce();
  machine.bindSession(); // this core's GTE / depth-cache / SPU / MDEC / XA, before the init prefix
  game_init(c);
  // PSXPORT_LOAD_STATE: resume a whole-machine state BEFORE the first field, so a headless tool
  // starts inside a level instead of replaying the thousands of fields between power-on and it.
  //
  // This is FATAL on failure rather than a fall back to booting from scratch, and the reason is
  // specific: a run that silently booted from power-on after a broken state path would spend its
  // entire budget re-deriving the state it was asked to start from, produce a plausible-looking
  // trace, and be reported as a working run. The path is resolved through the configuration owner,
  // so nothing here reads the environment itself.
  machine.applyConfiguredState();
  // the measured input/audio/simulation/render/present order and any cooperative task service.

  // Frame budget: an explicit PSXPORT_NATIVE_FRAMES always wins (headless tests). Otherwise, when
  // a window is up this is the real interactive game loop — run until the user closes the window
  // (SDL_QUIT -> exit(0) in present_window); headless with no cap defaults to 120 (CI/smoke).
  uint32_t nframes = 0; // 0 == run until window close / REPL quit
  // PSXPORT_REPL through the CVar ladder (config_vars.h cv_repl). THIS loop is the one and only
  // Repl::read() pump in the framework — repl_service.h names it, and every other loop that owns the
  // process refuses the knob rather than ignoring it.
  int repl_mode = psx::config::cv_repl.get() ? 1 : 0;
  if (repl_mode) {
    nframes = 0; // REPL drives frame count via `run N`
  } else {
    if (!gpu_vk_windowed()) {
      nframes = 120;
    }
  } // headless smoke default
  // PSXPORT_NATIVE_FRAMES: the comment above (and docs/driving-the-game.md) promised "an explicit
  // NATIVE_FRAMES always wins" but nothing ever READ the var — every headless PAD_REPLAY/no-REPL run
  // silently hit the smoke cap regardless. Explicit request now wins over every default above.
  if (!repl_mode) {
    int nf = cfg_int("PSXPORT_NATIVE_FRAMES", 0);
    if (nf > 0) {
      nframes = (uint32_t)nf;
    }
  }
  // A PAD REPLAY / RESUME OUTRANKS THE SMOKE CAP. Measured 2026-08-20: a headless
  // PSXPORT_PAD_RESUME of a 30,612-frame recording ran 120 frames and exited with "frame loop done",
  // having never left the title screen — and said nothing about it. Every measurement taken from such
  // a run described a scene the recording never reached, and it read as a code regression for most of
  // a session. The recording states how many frames the run needs; honour it. An EXPLICIT
  // PSXPORT_NATIVE_FRAMES still wins (above), because asking for N frames of a replay is legitimate —
  // but then the truncation is the caller's choice, and the run-end line below still reports it.
  // Keyed on the KNOB, not on the loaded recording: the .pad is opened lazily on the first serviced
  // frame, which is after this cap is decided, so asking pad.replayPending() here always answered
  // "no" and the uncap silently did nothing (measured: still 120 of 1118).
  if (!repl_mode && cfg_int("PSXPORT_NATIVE_FRAMES", 0) <= 0 &&
      (!psx::config::cv_pad_resume.get().empty() || !psx::config::cv_pad_replay.get().empty())) {
    nframes = 0;
    lucent::info("native_boot",
                 "frame cap LIFTED: a pad recording is being replayed, and the headless "
                 "smoke cap would have cut it off mid-recording");
  }
  // When the debug server is up (headless, no REPL), the run is INTERACTIVELY DRIVEN over the socket
  // (rw/w16/press/shot/dumpram, step/play) — do NOT cap it, or it exits before we can drive. The
  // server's `quit` command (or SIGINT) ends it.
  if (!repl_mode && !gpu_vk_windowed() && debug_server_live()) {
    nframes = 0;
  }
  // The live control channel, the store observer and the cap THIS run must use, armed together by the
  // same owner a title-owned spine calls: a surface nobody opens is not a surface. `attach` answers 0
  // (uncapped) when a client is going to drive, which is exactly the rule the block above applied by
  // hand — the run must not end before the client that is meant to steer it connects.
  const std::uint32_t clientCap = machine.attachControlChannel(nframes);
  nframes = clientCap;
  lucent::info(
      "native_boot", "entering native frame loop ({})", nframes ? "capped" : "interactive (until window close)");
  // The per-field services this loop owes, in the measured order, shared with `psx::Machine` and every
  // title-owned loop. The REPL budget below and the prompt consume inside the loop stay here: they are
  // this loop's own mode, not a field's obligation.
  const psx::frame::FieldTurn fieldTurn;
  long repl_budget = 0; // frames remaining in the current REPL `run N`
  for (uint32_t f = 0; nframes == 0 || f < nframes; f++) {
    // REPL: when the run-budget is exhausted, block reading stdin commands until a `run N` refills
    // it (immediate commands — r/w/watch/input/regs/seq — execute between frames). Quit/EOF breaks.
    if (repl_mode) {
      while (repl_budget <= 0) {
        repl_budget = c->game->repl.read(c, f);
        if (repl_budget < 0) {
          break;
        }
      }
      if (repl_budget < 0) {
        break;
      }
      repl_budget--;
    }
    // PSXPORT_DEBUG_SERVER pause/step: when frozen, do NOT advance the game — just pump host input
    // (keeps the window alive) and service debug commands so `step`/`play` can arrive. A `step` runs
    // exactly one real frame then re-freezes, so transient bad frames can be inspected one at a time.
    // The pause policy lives in DbgServer::honourPause because a title's own frame driver owes the same
    // behaviour, and two copies of "what a pause does" would be free to disagree.
    fieldTurn.beginField(*c);
    psx::frame::FrameLoopShell{}.step(*c, f);
    if (c->game->repl.consumePromptRequest()) {
      repl_budget = 0;
    }
    // The title FrameDriver owns its measured present, pace, and audio order, so this shell loop
    // performs none of those services around step(). What it DOES owe each field — the pause, the
    // watchdog re-arm, the mid-run RAM dump, one serviced command — is `psx::frame::FieldTurn`'s, shared with
    // `psx::Machine` and every title-owned loop, so a loop cannot silently omit one of them.
    fieldTurn.endField(*c, f);
  }
  machine.reportRunEnd();
}

// Wired from the title bootstrap when native boot is selected. Enters framework crt0 and then the
// host-owned product loop.
void native_boot_run(Core *c) {
  // The whole spine composes `psx::Machine`, the same owner a title-owned loop composes, so there is one
  // boot sequence with one set of obligations rather than a framework spine and a title spine that
  // agree today. Each step below is the same call, in the same order, as before this existed.
  psx::Machine machine{*c->game};
  // Refuse before diagnostics, FMVs, or a title boot hook can dispatch a non-returning guest main.
  // Product execution has exactly one frame owner: the title's finite native FrameDriver.
  // NOTE: the preflight only. Standalone game mains install their override clusters immediately before
  // entering here, so this spine must NOT register them; `Machine::prepare` (both halves) is the
  // composition for a product that has not installed them yet.
  machine.prepareProduct();

  // Standalone game mains install their override clusters immediately before entering here. Developer
  // diagnostics must install last or a working game override can silently displace them. dc_boot_init
  // performs the same ordering for dual-core and selftest boot paths.
  //
  // The HOST sampling profiler (PSXPORT_PROF). It was written, documented, given a companion report
  // tool, compiled into the library — and CALLED FROM NOWHERE, so `PSXPORT_PROF=1` came back from the
  // exit audit as "set for this whole run and NOTHING ever read it". An instrument that cannot produce
  // evidence is worse than none, because its existence answers "can we measure this?" with a yes.
  // Here is where it belongs: once, at boot, before any frame runs.
  // WHICH CALL SITES MOVE THE BYTES (PSXPORT_MEMCENSUS). hostprof answers "the PC is inside memmove",
  // which has now produced two wrong conclusions on kanban #118 because it cannot name the CALLER.
  // Armed here, beside the profiler, for the same reason.
  // The host census that names the CALLER of a host PC, and the active-config dump (docs/config.md).
  machine.armHostDiagnostics();
  machine.playBootMovies();
  machine.clearDisplayForFrontEnd();
  lucent::info("native_boot", "entering native crt0 (PC-driven)");
  native_crt0(c);
  lucent::info("native_boot", "returned from native crt0");
}

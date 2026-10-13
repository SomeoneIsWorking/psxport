// PC-PSX hybrid native boot and host-loop orchestration.
//
// Architecture: the host owns product iteration and delegates exactly one finite frame through
// psx::frame::FrameLoopShell to the title-created FrameDriver. Title state-machine, input, audio, render, and
// present ordering does not live here. This file retains generic crt0/boot, diagnostics, REPL pause,
// watchdog, and frame-budget scaffolding around that delegation.
#include "c_subsys.h"
#include "config_vars.h" // psx::config::render_path() / cv_repl — knobs through the CVar ladder
#include "core.h"
#include "dbg_server.h" // debug_server_port — the one reading of PSXPORT_DEBUG_SERVER
#include "field_turn.h" // psx::frame::FieldTurn — the per-field services this loop owes
#include "frame_loop_shell.h"
#include "game.h"
#include "game_iface.h"
#include "guest_call.h"
#include "machine.h" // psx::Machine — the one boot composition this spine and every title share
#include "mods.h"
#include "ot_attr.h" // OtAttr — the producer-census tables (armed by Game's ctor, game.cpp)
#include "repl.h"
#include "run_budget.h"
#include "run_end_report.h"
// (the REPORT is emitted by ~LightrecExecutor, on every exit path)
#include <lucent/log.h>

static void game_main(Core *c);

// Native crt0 implementation of recovered FUN_800896E0 behavior: clear BSS, initialize the heap,
// then call game_main. The libc/heap initialization at 0x80089860 remains a guest call.
static void native_crt0(Core *c) {
  psx::Machine{*c->game}.setupGuestBoot();
  game_main(c);
}

// Init prefix + task-0 bootstrap (everything FUN_80050b08 does before its scheduler loop).
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

  // The live control channel and the store observer, armed by the same owner a title-owned spine
  // calls: a surface nobody opens is not a surface.
  machine.attachControlChannel();
  const bool repl_mode = psx::config::cv_repl.get();
  psx::host::RunBudget budget = psx::host::RunBudget::fromConfig();
  lucent::info("native_boot",
               "entering native frame loop ({})",
               budget.cap() != 0 ? "capped" : "interactive (until window close)");
  // The per-field services this loop owes, in the measured order, shared with `psx::Machine` and every
  // title-owned loop.
  const psx::frame::FieldTurn fieldTurn;
  while (!budget.exhausted()) {
    const auto f = static_cast<std::uint32_t>(budget.steps());
    if (repl_mode && !c->game->repl.beforeStep(c, f)) {
      break;
    }
    fieldTurn.beginField(*c);
    psx::frame::FrameLoopShell{}.step(*c, f);
    budget.step();
    c->game->repl.afterStep();
    fieldTurn.endField(*c, f);
  }
  psx::RunEndReport{*c->game}.emit(budget.steps());
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
  // diagnostics must install last or a working game override can silently displace them.
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
  machine.clearDisplayForFrontEnd();
  lucent::info("native_boot", "entering native crt0 (PC-driven)");
  native_crt0(c);
  lucent::info("native_boot", "returned from native crt0");
}

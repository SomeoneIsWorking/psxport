// PC-PSX hybrid native boot and host-loop orchestration.
//
// Architecture: the host owns product iteration and delegates exactly one finite frame through
// FrameLoopShell to the title-created FrameDriver. Title state-machine, input, audio, render, and
// present ordering does not live here. This file retains generic crt0/boot, diagnostics, REPL pause,
// watchdog, and frame-budget scaffolding around that delegation.
#include "c_subsys.h"
#include "cfg.h"
#include "config.h"      // psx::config::report_once — arms the exit audit at BOOT, for every port
#include "config_vars.h" // psx::config::render_path() / cv_repl — knobs through the CVar ladder
#include "core.h"
#include "crt0_boot.h"   // crt0_plan/crt0_apply — THE crt0 derivation + the required/ABSENT decision
#include "crt0_verify.h" // crt0_audit — diffs the SHIPPED crt0 constants against the guest's own bytes
#include "fntrace.h"
#include "frame_loop_shell.h"
#include "game.h"
#include "game_iface.h"
#include "hostprof.h"
#include "hw_bind.h" // spu_bind/mdec_bind/xa_bind (per-instance HW-peripheral binders)
#include "memcensus.h"
#include "mods.h"        // oracle_mode — forces the pure guest reference presentation path
#include "ot_attr.h"     // OtAttr — the producer-census tables (armed by Game's ctor, game.cpp)
#include "producer_db.h" // producer_db_begin/_finish — the producer-DB run lifecycle
#include "repl.h"
#include <lucent/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h> // usleep (debug-server pause/step idle wait)
                    // class Repl — REPL driver + title-owned request state (per-Core, on Game)

static void game_main(Core *c);

// PC-native crt0 (faithful reimplementation of FUN_800896E0): BSS-zero, heap setup, then a DIRECT
// call to game_main — the top of the top-down PC-driven spine. Replaces the old bootstrap flip
// (crt0 jal main -> override). The libc/heap init at 0x80089860 stays a dispatched PSX leaf.
// crt0 register/heap setup only (no main call) — shared by native_crt0 and the dual-core harness.
// THIS FUNCTION PERFORMS NO ARITHMETIC. Every value it applies comes from crt0_plan (crt0_boot.h),
// which is the one place the derivation, the required/absent decision and the refusal live — so the
// hermetic test (tests/test_crt0_boot_group.cpp) exercises the code that SHIPS rather than a helper
// beside it. Keep it that way: a computation added here is a second copy by definition.
static void crt0_setup(Core *c) {
  const GuestProgramImage *image = c->guestProgramImage;
  // The two words the guest crt0 loads. Read before the .bss clear, exactly as the guest does — and
  // read through the plan's inputs rather than inside it, so the plan stays pure and testable.
  const uint32_t stackTopWord = image ? c->mem_r32(image->stackTopWordAddress) : 0u;
  const uint32_t reserveWord = image ? c->mem_r32(image->stackReserveWordAddress) : 0u;
  const Crt0Plan p = crt0_plan(image, stackTopWord, reserveWord, "crt0_setup");
  if (!p.ok) {
    // crt0_plan has already said WHICH fields are missing and how many it checked. Exiting is the
    // only honest continuation: the previous behaviour was to clear whatever span the zero fields
    // described, build sp out of mem[0], write the heap base and size to GUEST 0x00000000, and hand
    // InitHeap a garbage capacity — a boot that looks like a boot and is not one.
    lucent::error("crt0",
                  "boot ABORTED: the game's crt0 boot group is incomplete (see above). No "
                  "guest state has been modified.");
    fflush(stderr);
    exit(1);
  }
  // CROSS-CHECK THE SHIPPED CONSTANTS AGAINST THE GUEST'S OWN crt0 BYTES, before applying any of them.
  // This is the gate that was missing: every field above is a MEASURED value hand-copied into the game's
  // derived runtime, and nothing compared the copy to the measurement. crt0_audit re-derives the group
  // from the instruction stream at image->crt0Entry and refuses a CONFIRMED disagreement (crt0_verify.h).
  if (!crt0_audit(
          image,
          p,
          [c](uint32_t a) {
            return c->mem_r32(a);
          },
          "crt0_setup")) {
    lucent::error("crt0",
                  "boot ABORTED: the shipped crt0 boot group DISAGREES with the guest's own "
                  "crt0 (see above). No guest state has been modified.");
    fflush(stderr);
    exit(1);
  }
  // a1 = heap size USED TO BE MISSING: libcInit is the BIOS A(39h) InitHeap(ptr, size) thunk in every
  // consumer measured so far, and hle.cpp's `case 0x39` copies a1 straight into Hle::heap_size — the
  // capacity every BIOS malloc is checked against. The old code set only r[4], so the arena length was
  // whatever the register file happened to hold. The PRIOR value is logged next to the correct one
  // because that difference IS the blast radius of the bug, and it is only observable here: reading
  // c->r[5] after crt0 has set it tells you nothing.
  lucent::info("crt0",
               "libcInit 0x{:08X}: a0=0x{:08X} a1=0x{:X} (a1 held 0x{:08X} = {} before crt0 set "
               "it — that stale value is the heap capacity this port passed to InitHeap before "
               "the r[5] fix; a difference here is the bug's blast radius)",
               p.libcInit,
               p.a0,
               p.a1,
               c->r[5],
               c->r[5]);
  // The write sequence itself comes from crt0_apply, NOT from lines here: the defect was in the
  // application (an unconditional store through a zero pointer), so the sequence lives in the tested
  // header and this is only the adapter that binds it to a Core.
  struct CoreWriter {
    Core *c;
    void w32(uint32_t a, uint32_t v) {
      c->mem_w32(a, v);
    }
    void reg(int i, uint32_t v) {
      c->r[i] = v;
    }
    void call(uint32_t entry) {
      rec_dispatch(c, entry);
    }
  } w{c};
  crt0_apply(p, w);
}

static void native_crt0(Core *c) {
  crt0_setup(c);
  game_main(c); // DIRECT PC call (was: crt0 jal main -> flip)
}

// Init prefix + task-0 bootstrap (everything FUN_80050b08 does before its scheduler loop). Factored out
// of game_main so the dual-core harness can init two cores then drive the frame loop itself.
static void game_init(Core *c) {
  // The game's boot-init prologue (FUN_80050b08 init prefix + task-0 bootstrap) is GAME behaviour: it
  // moved WHOLE to the game side (game/core/game_hooks.cpp tomba_bootInit) via GameRuntime, so
  // this framework file no longer bakes in the game's guest boot-prologue addresses or c->engine.* init
  // calls. It moves as ONE unit (not just the engine calls) because the engine calls are interleaved
  // with the rc-dispatched guest leaves and task0Bootstrap depends on the scheduler-table init between
  // them — the order is load-bearing and cannot be split. crt0_setup + the per-core binds (this file)
  // stay framework scaffolding.
  c->runtime->bootInit(*c);
}

// Dual-core harness hooks (dualcore.cpp / selftest.cpp / sbs.cpp): boot a core to the start of the
// frame loop, then step it one frame at a time. dc_boot_init = crt0 setup + the init prefix/bootstrap;
// dc_step_frame = one frame.
//
// GameRuntime::registerOverrides HAS to run here (2026-07-08 fix, docs/findings/tooling.md
// "SBS/DualCore/Selftest never populate their own Game's override table"): every harness that boots
// via dc_boot_init constructs its OWN Game, and the registration previously lived ONLY inline in
// boot.cpp's main(), against a single throwaway Game these harnesses never touch. Without calling it
// here, the process-global override registry (overrides::install, override_registry.h) never gets
// entries for anything wired ONLY through this path (Animation, ActorZonedAttacker, Spawn,
// ReleaseTriggerMotion) — both SBS cores would silently run the identical substrate body, and SBS's
// byte-exact compare would "pass" without the native port ever running. See boot.cpp for the shared
// registration function + full writeup.
// Runs BEFORE crt0_setup/game_init: game_init's init prefix can itself reach a direct-substrate
// g_override[] call (e.g. FUN_80050b08's task0 bootstrap arming a scheduler slot via spawnPrim) —
// the registry must already carry these entries before any guest code runs, or a call through an
// unregistered thunk aborts on "unregistered" (found live: SBS core A crashed here before this line
// was moved above the init calls).
// dc_boot_init — the per-Core boot chokepoint EVERY port and harness funnels through (standalone
// main(), SBS, dualcore, the selftests). The render path is installed HERE, not in one boot spine's
// tail, because that is the mistake this replaces: it used to be resolved inside native_boot_run, so a
// port that boots past it (spyro: main -> dc_boot_init -> GameRuntime::bootInit) had the path stuck at its
// default and had to re-parse the flag game-side to get it back. A knob read at Core setup belongs at
// Core setup. Harnesses that own the path per leg (Sbs::Impl::applyMode) set it after this and win.
void dc_boot_init(Core *c) {
  psx::config::report_once();
  void gte_bind(Core *);
  gte_bind(c);
  c->rsub.projprim.bind(c);
  spu_bind(c);
  mdec_bind(c);
  xa_bind(c);
  c->runtime->registerOverrides(*c->game);
  // Harnesses construct their own Game objects, so every per-Game hardware-service table must be
  // populated here as well as on the standalone main() path.  The generated override setters are
  // process-global and made the native leg appear healthy even when this table was empty; the pure
  // interpreter oracle consults only its own PlatformHle and consequently fell into libcd's real
  // CdlSync/CdlSetloc IRQ waits forever.  Register the CD command/read seams first, then the generic
  // BIOS-library waits, matching the standalone boot order.
  c->game->cd.overridesInit();
  FrameLoopShell{}.prepareProduct(*c->game);
  fntrace_init();
  render_path_install(c);
  crt0_setup(c);
  game_init(c);
}
void dc_step_frame(Core *c, uint32_t f) {
  FrameLoopShell{}.step(*c, f);
}

static void game_main(Core *c) {
  // Arm the config audit before title initialization. report_once() is idempotent; the complete dump
  // remains after the loop for bounded diagnostic runs.
  psx::config::report_once();
  void gte_bind(Core *);
  gte_bind(c);              // bind this core's GTE before the init prefix / frame loop
  c->rsub.projprim.bind(c); // and this core's native depth-cache (class ProjPrim on Render)
  spu_bind(c);              // and this core's SPU
  mdec_bind(c);             // and this core's MDEC
  xa_bind(c);               // and this core's XA streamer
  game_init(c);
  // The host owns iteration; FrameLoopShell delegates one finite frame. The title FrameDriver owns
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
    if (!gpu_windowed()) {
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
      (cfg_str("PSXPORT_PAD_RESUME") || cfg_str("PSXPORT_PAD_REPLAY"))) {
    nframes = 0;
    lucent::info("native_boot",
                 "frame cap LIFTED: a pad recording is being replayed, and the headless "
                 "smoke cap would have cut it off mid-recording");
  }
  // When the debug server is up (headless, no REPL), the run is INTERACTIVELY DRIVEN over the socket
  // (rw/w16/press/shot/dumpram, step/play) — do NOT cap it, or it exits before we can drive. The
  // server's `quit` command (or SIGINT) ends it.
  if (!repl_mode && !gpu_windowed() && cfg_on("PSXPORT_DEBUG_SERVER")) {
    nframes = 0;
  }
  producer_db_begin(c); // the DB's claim set — see producer_db.h (a port owning its loop calls this itself)
  lucent::info(
      "native_boot", "entering native frame loop ({})", nframes ? "capped" : "interactive (until window close)");
  c->game->dbg_server.start(c); // PSXPORT_DEBUG_SERVER: non-blocking live TCP debug server (dbg_server.cpp)
  long repl_budget = 0;         // frames remaining in the current REPL `run N`
  for (uint32_t f = 0; nframes == 0 || f < nframes; f++) {
    // REPL: when the run-budget is exhausted, block reading stdin commands until a `run N` refills
    // it (immediate commands — r/w/watch/input/regs/seq — execute between frames). Quit/EOF breaks.
    if (repl_mode) {
      // Blocking on stdin for the next command is an intentional idle, not a hang — suspend the
      // frame-progress watchdog while waiting so it doesn't fire at a paused REPL prompt.
      if (repl_budget <= 0) {
        watchdog_suspend();
      }
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
    {
      DbgServer &dbg = c->game->dbg_server;
      if (dbg.isPaused()) {
        watchdog_suspend(); // a debug pause is intentional idle, not a hang
      }
      while (dbg.isPaused()) {
        if (dbg.stepPending()) {
          dbg.consumeStep();
          break;
        } // run exactly one frame
        c->game->pad.pumpHostInput(); // host input ONLY — must not tick the pad-frame clock here
        // Re-SHOW the last real frame (no rebuild) so the window stays live and the readback target keeps
        // holding it — see GpuState::gpu_repaint. A pause must never re-render: this loop spins at ~66 Hz,
        // and at fps60 there is no geometry batch left to re-render at all (kanban #20's black screen).
        c->game->gpu.gpu_repaint();
        dbg.service(c); // receive step/play/capture commands
        usleep(15000);
      }
    }
    watchdog_resume(); // re-arm after idle without falsely claiming this frame completed; the
                       // completed present switches first-frame grace to the steady budget
    FrameLoopShell{}.step(*c, f);
    if (c->game->repl.consumePromptRequest()) {
      repl_budget = 0;
    }
    // The title FrameDriver owns its measured present, pace, and audio order, so this shell loop
    // performs none of those services around step().
    // PSXPORT_RAMDUMP_FRAME=N — dump RAM mid-run at native frame N (overlay state during gameplay
    // differs from end-of-run; needed to disasm the LIVE level/stage overlay at 0x8010/0x8011xxxx).
    {
      const char *rdf = cfg_str("PSXPORT_RAMDUMP_FRAME");
      if (rdf && f == (uint32_t)strtoul(rdf, 0, 0)) {

        const char *rd = cfg_str("PSXPORT_RAMDUMP");
        if (!rd) {
          rd = "scratch/bin/midrun_ram.bin";
        }
        FILE *mf = fopen(rd, "wb");
        if (mf) {
          fwrite(c->ram, 1, 0x200000, mf);
          fclose(mf);
          lucent::info("native_boot", "mid-run RAM dump @frame {} -> {}", f, rd);
        }
      }
    }
    c->game->dbg_server.service(c); // service one queued live-debug-server command (non-blocking)
  }
  // The GUEST leg's denominator, printed next to the census so "0 guest prims" can be told apart from
  // "the span feed recorded nothing". Without it, an armed feed that silently did no work measures as
  // free and reads as working.
  lucent::info("producers",
               "run-end: OtAttr spans recorded {} (overflow {}) — the guest leg's feed",
               c->rsub.otAttr.spanCount(),
               c->rsub.otAttr.spanOverflow());
  // THE JOIN RATE, printed next to the census so a row list can never be read as a comparison it is not.
  // Resolved = the guest prim landed in the row a native producer keys; unresolved = no frame in the
  // searched window is claimed, i.e. THIS EFFECT HAS NO NATIVE PRODUCER (the DB's actual answer, not a
  // failure); too-early = the claim set was still empty, so the prim could not be resolved either way and
  // must not be counted as "no native producer".
  producer_db_finish(c); // report + JSONL + claim append — see producer_db.h
  // THE REPLAY'S OWN DENOMINATOR. "frame loop done" alone cannot distinguish a run that played the
  // whole recording from one the frame cap cut off at 0.4% of it — and those mean opposite things
  // about every number the run produced. Print it whenever a recording was loaded, consumed or not.
  if (c->game->pad.replayTotal()) {
    const size_t total = c->game->pad.replayTotal();
    const uint32_t used = c->game->pad.replayConsumed();
    if (used < total) {
      lucent::warn("padrec",
                   "run-end: replay TRUNCATED — consumed {} of {} pad frame(s) ({:.1f}%). "
                   "The run ended before the recording did, so it did NOT reach the scene the "
                   "recording was cut for. Anything measured here describes an earlier scene.",
                   used,
                   total,
                   100.0 * used / (double)total);
    } else {
      lucent::info("padrec", "run-end: replay fully consumed — {} of {} pad frame(s)", used, total);
    }
  }
  lucent::info("native_boot", "frame loop done");
  const char *rd = cfg_str("PSXPORT_RAMDUMP");
  if (rd) {

    FILE *f = fopen(rd, "wb");
    if (f) {
      fwrite(c->ram, 1, 0x200000, f);
      fclose(f);
      lucent::info("native_boot", "dumped 2MB RAM -> {}", rd);
    }
  }
}

// Wired from the title bootstrap when native boot is selected. Enters framework crt0 and then the
// host-owned product loop.
void native_boot_run(Core *c) {
  // Refuse before diagnostics, FMVs, or a title boot hook can dispatch a non-returning guest main.
  // Product execution has exactly one frame owner: the title's finite native FrameDriver.
  FrameLoopShell{}.prepareProduct(*c->game);

  // Standalone game mains install their override clusters immediately before entering here. Developer
  // diagnostics must install last or a working game override can silently displace them. dc_boot_init
  // performs the same ordering for dual-core and selftest boot paths.
  fntrace_init();
  // The HOST sampling profiler (PSXPORT_PROF). It was written, documented, given a companion report
  // tool, compiled into the library — and CALLED FROM NOWHERE, so `PSXPORT_PROF=1` came back from the
  // exit audit as "set for this whole run and NOTHING ever read it". An instrument that cannot produce
  // evidence is worse than none, because its existence answers "can we measure this?" with a yes.
  // Here is where it belongs: once, at boot, before any frame runs.
  hostprof_init();
  // WHICH CALL SITES MOVE THE BYTES (PSXPORT_MEMCENSUS). hostprof answers "the PC is inside memmove",
  // which has now produced two wrong conclusions on kanban #118 because it cannot name the CALLER.
  // Armed here, beside the profiler, for the same reason.
  memcensus_init();
  {
    void cfg_dump(void);
    cfg_dump();
  } // log active PSXPORT_* config once (see docs/config.md)
  // PSXPORT_ORACLE — THE PURE PSX REFERENCE (user 2026-07-08). recomp gameplay + UNENHANCED guest
  // render: the substrate's own GTE+OT+GP0, with NO native enhancement able to touch the picture. THE
  // trustworthy oracle for byte + picture comparison. It is now expressed as the BUNDLE it always was:
  // psx_fallback (gameplay on the substrate) + RenderPath::Gte (the picture from the guest's own
  // geometry, purity carried by the path itself via RenderMode::enhancementsAllowed). Forced here
  // BEFORE the GATE / RENDER_PATH blocks so their log lines report the final state.
  if (oracle_mode()) {
    c->game->setOracle();                  // per-Game: recomp gameplay + mods forced neutral
    c->rsub.mode.setPath(RenderPath::Gte); // the guest's own GTE+OT — not the native scene pass
    lucent::info("native_boot",
                 "PSXPORT_ORACLE=1 — pure recomp + pure guest render (path=gte: no fps60 / wide / ires / native-depth "
                 "/ observer)");
  }
  // PSX-fallback gate (diagnostic): boot + frame-loop stay native; everything the loop calls runs as PSX
  // recomp (sync CD). PSXPORT_GATE nonzero turns it on; the REPL `gate on|off` toggles it live.
  // ORTHOGONAL TO THE RENDER PATH: this is a GAMEPLAY switch. Do not fold it into one.
  {
    const char *g = cfg_str("PSXPORT_GATE");
    if (g && *g) {
      c->game->psx_fallback = (atoi(g) != 0);
    }
    lucent::info("native_boot",
                 "psx_fallback={} ({})",
                 c->game->psx_fallback,
                 c->game->psx_fallback ? "native boot+frameloop, PSX everything else (sync)" : "full native");
  }
  render_path_install(c); // native | gte | psx, from the CVar ladder + aliases (render_path.cpp)
  // DUAL-VIEW: render the SAME game state TWICE per frame — engine-native (left) + PSX-recomp (right) — and
  // composite side by side. Set at launch (PSXPORT_DUALVIEW=1) so the GPU allocates two geometry batches.
  {
    const char *r = cfg_str("PSXPORT_DUALVIEW");
    if (r && *r) {
      c->rsub.mode.setDualview(atoi(r) != 0);
    }
    if (c->rsub.mode.dualview()) {
      lucent::info("native_boot", "Render::dualview=1 (side-by-side native | PSX render)");
    }
  }
  // Intro FMVs: the real boot is SCEA (stub) -> Whoopee logo (LOGO.STR) -> opening movie (OP.STR) ->
  // title/menu. The game's own STR streaming (strNext) TIMES OUT under our runtime (we don't feed
  // CD-streamed FMV sectors to its StrPlayer — see "time out in strNext()" in the DEMO stage), so the
  // movies are played here with our self-contained native FMV player (native_fmv.c).
  // SPLIT OF OWNERSHIP: only LOGO.STR (the Whoopee logo, which plays BEFORE the front-end overlay is
  // even loaded) is played at boot. OP.STR (the opening movie) is OWNED BY THE FRONT-END — the DEMO
  // menu machine's states 4..7 ARE the OP.STR sequence (demo.cpp demo_menu_machine), which now
  // plays it via fmv.play. Playing OP here too made it play TWICE (boot + front-end) — the
  // "FMV repeats" bug. Boot plays LOGO; the front-end plays OP -> SCEA->LOGO->OP->title, no repeat.
  // Skip the intro FMVs on PSXPORT_NO_FMV ONLY.
  //
  // This used to read `|| cfg_on("PSXPORT_VK_HEADLESS")`, and that one term cost a user-reported bug
  // a whole day. USER RULE: "Headless and windowed should never be different code paths" — headless
  // is the same pipeline with a different FINAL SINK (a readback instead of a swapchain present),
  // and whether a movie PLAYS is game behaviour, not a sink concern.
  //
  // Concretely: the user reported "boots into black screen, missing splash or FMV". Every
  // measurement taken to investigate it was headless, so every one of them SKIPPED THE INTRO MOVIES
  // BY CONSTRUCTION. The instrument could not produce the failing answer, the numbers came back
  // green, and the bug was reported fixed while the user still saw a black screen. A run that
  // silently does less than the real program is not a fast probe, it is a lie with a good excuse.
  //
  // The wall-clock argument the old comment made (a field probe 77s -> 1.4s) is real, and it is what
  // PSXPORT_NO_FMV is FOR — a probe that does not want movies asks for that explicitly, and its log
  // then says so. What is not acceptable is inferring the intent from the render sink.
  int skip_fmv = cfg_on("PSXPORT_NO_FMV");
  const char *nf_ov = cfg_str("PSXPORT_NO_FMV");
  if (nf_ov && atoi(nf_ov) == 0 && *nf_ov) {
    skip_fmv = 0; // explicit PSXPORT_NO_FMV=0 forces FMVs on
  }
  // The list comes from GameConfig::bootFmv — it used to be a hardcoded path, which is the first
  // consumer's file and nothing a second port could ever open. An all-null list is a real answer
  // ("this game's boot plays no movie natively"), not a missing value, so it is not a warning.
  const char *const *boot_fmv = c->cfg ? c->cfg->bootFmv : nullptr;
  const int n_boot_fmv = boot_fmv ? (int)(sizeof c->cfg->bootFmv / sizeof c->cfg->bootFmv[0]) : 0;
  if (!skip_fmv && boot_fmv && boot_fmv[0]) {
    for (int i = 0; i < n_boot_fmv && boot_fmv[i]; i++) {
      lucent::info("native_boot", "playing boot FMV {}/{}: {}", i + 1, n_boot_fmv, boot_fmv[i]);
      c->game->fmv.play(boot_fmv[i]);
    }
  } else if (!skip_fmv) {
    lucent::info("native_boot", "no boot FMV configured (GameConfig::bootFmv is empty) — nothing to play");
  } else {
    lucent::warn("native_boot", "skipping intro FMVs (headless/NO_FMV)");
  }
  // Clean hand-off to the front-end (issues #7/#11): black the display FB before the title builds, so the
  // title's first frames (drawn over several frames while its background/font/CLUT upload) never composite
  // over the stale SCEA white-fill or an FMV last-frame. Covers the no-FMV-ran case too (the stub splash
  // fill is still resident in s_vram even when both intros are skipped). Deterministic, no timer.
  void gpu_clear_display(Core *);
  gpu_clear_display(c);
  lucent::info("native_boot", "entering native crt0 (PC-driven)");
  native_crt0(c);
  lucent::info("native_boot", "returned from native crt0");
}

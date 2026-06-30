// Headless TDD regression harness for the FULL-PSX (psx_fallback) path — the same core the SBS "core B"
// runs. It boots ONE psx_fallback core (recompiler-only, coroutine-resumed cooperative tasks), drives it
// like a player ("mash Start" at the title), and asserts the game actually REACHES field free-roam instead
// of freezing during the start-game sequence. This is the deterministic, render-free verification the SBS
// window can't give us headless: a frozen game = the GAME-stage state machine never advances; a live game =
// it reaches free-roam (sm[0x48]==2, the same value the native field settles at — native_boot.cpp log).
//
// Selected by PSXPORT_SELFTEST=startgame (boot.cpp). Exit 0 = pass, 1 = fail. PSXPORT_SELFTEST_VERBOSE=1
// prints the per-frame stage/SM transitions (for observing the RED state while iterating on a fix).
#include "core.h"
#include "game.h"
#include "cfg.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>

extern "C" int xa_stream_owns_slot2(void);
extern "C" int xa_stream_voice_busy(void);
extern "C" int xa_stream_is_active(void);
void load_exe(const char* path, Core* c);
void dc_boot_init(Core* c);
void dc_step_frame(Core* c, uint32_t f);
void pad_repl_hold(Core* c, uint16_t active_low_mask);

#define TASKBASE   0x801fe000u   // scheduler task-0 object; the active STAGE runs here (entry @+0xc)
#define STAGE_DEMO 0x801062E4u
#define STAGE_GAME 0x8010637Cu   // the field; free-roam settles at sm[0x48]==2
#define PAD_NONE   0xFFFFu
#define PAD_START  (0xFFFFu & ~0x0008u)   // active-low: Start pressed
#define PAD_CROSS  (0xFFFFu & ~0x4000u)   // active-low: Cross pressed (menu confirm)

// One self-test run. Returns 0 on pass, 1 on fail.
static int run_startgame(const char* path) {
  const int verbose = cfg_on("PSXPORT_SELFTEST_VERBOSE");
  Game* game = new Game();
  game->psx_fallback = 1;                 // FULL PSX: cooperative tasks run as coroutine-resumed recomp
  Core* c = &game->core;
  load_exe(path, c);
  dc_boot_init(c);

  const uint32_t REACH_CAP   = 2500;      // frames allowed to mash Start and reach the GAME stage
  const uint32_t SETTLE_CAP  = 1200;      // frames allowed (after reaching GAME) to reach free-roam
  uint32_t f = 0;
  uint32_t last_stage = 0, last_sm = 0xFFFFFFFFu;

  auto stage = [&]{ return c->mem_r32(TASKBASE + 0xc); };
  auto sm48  = [&]{ return (uint32_t)c->mem_r16(TASKBASE + 0x48); };
  auto log_change = [&]{
    uint32_t st = stage(), s = sm48();
    if (verbose && (st != last_stage || s != last_sm)) {
      const char* n = st == STAGE_DEMO ? "DEMO" : st == STAGE_GAME ? "GAME" : "?";
      fprintf(stderr, "[selftest] f%u stage=%s(0x%08X) sm[0x48]=%u\n", f, n, st, s);
      last_stage = st; last_sm = s;
    }
  };

  // Phase 1 — MASH START: pulse Start (and Cross as a confirm) to leave the title and start the game.
  // Pulsing (8 on / 8 off) makes each press a fresh input EDGE the menu's current&~prev logic sees.
  for (; f < REACH_CAP && stage() != STAGE_GAME; f++) {
    bool on = (f % 16u) < 8u;
    pad_repl_hold(c, on ? ((f & 16u) ? PAD_CROSS : PAD_START) : PAD_NONE);
    dc_step_frame(c, f);
    log_change();
  }
  if (stage() != STAGE_GAME) {
    fprintf(stderr, "[selftest] FAIL: never reached GAME stage after %u frames of mashing Start "
                    "(stuck stage=0x%08X sm[0x48]=%u)\n", f, stage(), sm48());
    return 1;
  }
  uint32_t reached_at = f;
  fprintf(stderr, "[selftest] reached GAME stage at frame %u — now checking it reaches free-roam\n", reached_at);

  // Phase 2 — DON'T touch Start in gameplay (Start = pause menu). Just step and require the GAME state
  // machine to advance to field free-roam (sm[0x48]==2). A freeze = it never gets there.
  pad_repl_hold(c, PAD_NONE);
  for (uint32_t g = 0; g < SETTLE_CAP; g++, f++) {
    dc_step_frame(c, f);
    log_change();
    if (stage() == STAGE_GAME && sm48() == 2) {
      // Reached the RUNNING field state (sm[0x48]==2). The bug this test guards (the recompiler split the
      // GAME stage fn at the seeded loop top 0x801063F4, so the prologue func returned instead of flowing
      // into the loop -> the coro task was reaped as "done" and the field FROZE at sm[0x48]==0, never
      // reaching 2) is decisively distinguished here: pre-fix sm[0x48] is STUCK at 0; post-fix it reaches
      // 2 AND the GAME loop actually executes. Confirm the loop ran (its per-iteration counter
      // *(0x1F800198), bumped at 0x80106470, advanced > 0) so this isn't a one-frame fluke.
      // Two things must hold (the two verified fixes):
      //  (1) PROLOGUE-SPLIT (later-269): the GAME loop must actually RUN — its per-iteration counter
      //      *(0x1F800198) advances. Pre-fix it was stuck at 0 (the prologue returned, the task was reaped).
      //  (2) SPU-ADVANCE-HEADLESS (later-270): entering the first area starts an intro XA voice clip and the
      //      field waits on it; the SPU/XA stream is now ticked even headless, so the clip COMPLETES
      //      (xa_active goes 1 -> 0) instead of hanging the wait forever (the old headless artifact). We
      //      assert the clip both PLAYED and ENDED.
      // NB: we do NOT assert the field keeps advancing past the intro cutscene — running the full recompiled
      // field under coroutines (the SBS DIAGNOSTIC core; the shipping game is the NATIVE path) hits a deeper
      // field-mode cooperative-wait that doesn't resolve headless. That's a known full-PSX limit, tracked
      // separately (docs/findings/sbs.md), not this test's subject.
      uint32_t c0 = c->mem_r16(0x1F800198u);
      int saw_clip = 0, clip_ended = 0;
      const uint32_t WIN = 200, MAX_WINS = 40;
      for (uint32_t w = 0; w < MAX_WINS && !clip_ended; w++) {
        for (uint32_t k = 0; k < WIN; k++, f++) dc_step_frame(c, f);
        if (xa_stream_is_active()) saw_clip = 1;
        else if (saw_clip) clip_ended = 1;
        if (verbose) fprintf(stderr, "[selftest]   window %u: loop raw=%u xa_active=%d\n",
                             w, c->mem_r16(0x1F800198u), xa_stream_is_active());
      }
      uint32_t loop_adv = (uint16_t)(c->mem_r16(0x1F800198u) - c0);
      int loop_ran = (loop_adv > 0) || (sm48() == 2 && c->mem_r16(0x1F800198u) != c0);
      if (loop_ran && saw_clip && clip_ended) {
        // DEEPER GUARD (later-272): reaching free-roam + the clip completing is NOT enough — the field must
        // KEEP RUNNING. The full-PSX field froze the GAME loop at counter ~34 because the entity-update
        // dispatcher FUN_8003c048 has an IN-FUNCTION jump-table `jr v0` (0x8003C0AC, table 0x80014db8) that
        // the recompiler mis-emitted as `rec_dispatch(c, target); return;` (it failed to recover the table —
        // its base reg s3 is built `lui v0,HI; addiu s3,v0,LO` with rs!=rt, which find_jump_tables didn't
        // handle). That `return` SKIPS the function epilogue, so dispatching the first entity (the area's
        // terrain actor, ~loop iter 36) leaks 112 (0x70) bytes of guest SP and loses callee-saved s0–s3 →
        // the GAME loop's base reg s0 (0x1f800000) corrupts to 1 → its counter write `*(s0+0x198)` diverts
        // to main RAM 0x80000198 and sm[0x48] dispatch reads garbage → the field spins dead. Assert the
        // counter keeps advancing well past 34 over a sustained window (RED before the emit fix, GREEN after).
        uint32_t before = c->mem_r16(0x1F800198u);
        const uint32_t RUN = 600;
        for (uint32_t k = 0; k < RUN; k++, f++) dc_step_frame(c, f);
        uint32_t adv = (uint16_t)(c->mem_r16(0x1F800198u) - before);
        if (adv < 50) {
          fprintf(stderr, "[selftest] FAIL: field FROZE after the intro clip — GAME loop counter "
                          "*(0x1F800198) advanced only %u over %u frames (stuck=%u). The full-PSX field is "
                          "not progressing (recomp-coro callee-saved-register corruption — see "
                          "docs/findings/sbs.md / FUN_8003c048 jump-table emit).\n",
                  adv, RUN, c->mem_r16(0x1F800198u));
          return 1;
        }
        fprintf(stderr, "[selftest] PASS: GAME loop RAN, the intro XA clip PLAYED then COMPLETED headless, "
                        "AND the field KEEPS running (counter +%u over %u frames).\n", adv, RUN);
        return 0;
      }
      fprintf(stderr, "[selftest] FAIL: loop_ran=%d (counter +%u) saw_clip=%d clip_ended=%d "
                      "(xa_active=%d sm[0x48]=%u).\n",
              loop_ran, loop_adv, saw_clip, clip_ended, xa_stream_is_active(), sm48());
      return 1;
    }
    if (stage() != STAGE_GAME) {   // bounced back out of GAME (e.g. froze->reset) — not free-roam
      fprintf(stderr, "[selftest] note: left GAME stage at frame %u (stage=0x%08X)\n", f, stage());
    }
  }
  fprintf(stderr, "[selftest] FAIL: GAME stage entered at f%u but field never reached free-roam "
                  "(stuck sm[0x48]=%u after %u frames) — the start-game sequence FROZE.\n",
          reached_at, sm48(), SETTLE_CAP);
  return 1;
}

// NARRATION — the NATIVE shipping path (psx_fallback=0), driven through the full intro narration that the
// player does NOT skip: reach the GAME stage (mash Start/Cross at the title = "New Game"), then release all
// input and let the opening narration play and auto-transition into the first field area. This is the path
// that crashed at the SOP->A00 transition: ov_scene_native renders A00 entity objects before their models
// are attached (cmd+0x40 still holds an unrelocated area-data pointer 0x8018Axxx), so native_gt3gt4 reads a
// garbage prim count and overflows the render queue ([rq] FATAL, an abort). The assertions: (1) the run
// SURVIVES well past the transition (an abort kills this process = fail); (2) the GAME loop KEEPS RUNNING
// across it (its per-iteration counter *(0x1F800198) advances) — so we test the field actually progresses,
// not merely that we dodged the abort. RED before the narration->field transition is owned; GREEN after.
static int run_narration(const char* path) {
  const int verbose = cfg_on("PSXPORT_SELFTEST_VERBOSE");
  Game* game = new Game();
  game->psx_fallback = 0;                  // NATIVE shipping path (not the SBS full-PSX coroutine core)
  Core* c = &game->core;
  load_exe(path, c);
  dc_boot_init(c);

  auto stage = [&]{ return c->mem_r32(TASKBASE + 0xc); };
  auto sm48  = [&]{ return (uint32_t)c->mem_r16(TASKBASE + 0x48); };

  const uint32_t REACH_CAP = 2500;
  uint32_t f = 0;
  for (; f < REACH_CAP && stage() != STAGE_GAME; f++) {
    bool on = (f % 16u) < 8u;
    pad_repl_hold(c, on ? ((f & 16u) ? PAD_CROSS : PAD_START) : PAD_NONE);
    dc_step_frame(c, f);
  }
  if (stage() != STAGE_GAME) {
    fprintf(stderr, "[selftest] FAIL: never reached GAME stage after %u frames (stuck stage=0x%08X)\n",
            f, stage());
    return 1;
  }
  fprintf(stderr, "[selftest] reached GAME at frame %u — now playing the intro narration (no input) "
                  "through the field transition\n", f);

  // Release input and run a long window spanning the narration AND the SOP->field transition (the crash was
  // ~frame 1105 into GAME). If the render-queue overflow fires, the process aborts here and the test dies.
  pad_repl_hold(c, PAD_NONE);
  uint32_t c0 = c->mem_r16(0x1F800198u);
  const uint32_t RUN = 2500;
  for (uint32_t k = 0; k < RUN; k++, f++) {
    dc_step_frame(c, f);
    if (verbose && (k % 200u) == 0)
      fprintf(stderr, "[selftest]   narration f=%u sm[0x48]=%u loop=%u\n", f, sm48(), c->mem_r16(0x1F800198u));
  }
  uint32_t adv = (uint16_t)(c->mem_r16(0x1F800198u) - c0);
  if (adv < 50) {
    fprintf(stderr, "[selftest] FAIL: GAME loop did not progress through the narration (counter advanced "
                    "only %u over %u frames; sm[0x48]=%u). The narration->field transition stalled.\n",
            adv, RUN, sm48());
    return 1;
  }
  fprintf(stderr, "[selftest] PASS: the un-skipped intro narration played through the field transition "
                  "WITHOUT a render-queue overflow, and the GAME loop kept running (counter +%u over %u "
                  "frames).\n", adv, RUN);
  return 0;
}

// ORACLE smoke test (later-278) — boot ONE core as the pure-PSX INTERPRETER oracle (psx_fallback + the new
// use_interp engine) and prove it runs the intro cutscene WITHOUT the recomp substrate's two failure modes:
// the recomp-MISS on the un-recompiled overlay code (the interpreter interprets it) and the mis-emitted
// in-function `jr` freeze that stuck the full-PSX GAME loop counter at ~34 (later-272 — the interpreter runs
// the jump directly, so no corruption). PASS = reaches GAME, the loop counter advances FAR past 34 over a
// sustained window, and the SOP narration scene byte (0x800bf9b4) progresses through the beats (it reaches
// the void/cliff scene ids 5..7). This is the foundation check for the divergence harness (docs/oracle.md);
// the state-sync barrier + VRAM diff build on top. Selected by PSXPORT_SELFTEST=oracle.
static int run_oracle(const char* path) {
  const int verbose = cfg_on("PSXPORT_SELFTEST_VERBOSE");
  Game* game = new Game();
  game->psx_fallback = 1;                  // FULL PSX: cooperative tasks run as coroutine-resumed bodies...
  game->core.use_interp = 1;               // ...but INTERPRETED (the oracle engine), not the recomp substrate
  Core* c = &game->core;
  load_exe(path, c);
  dc_boot_init(c);

  auto stage = [&]{ return c->mem_r32(TASKBASE + 0xc); };
  auto sm48  = [&]{ return (uint32_t)c->mem_r16(TASKBASE + 0x48); };
  auto loopc = [&]{ return c->mem_r16(0x1F800198u); };
  auto scene = [&]{ return c->mem_r8(0x800bf9b4u); };

  const uint32_t REACH_CAP = 4000;
  uint32_t f = 0;
  for (; f < REACH_CAP && stage() != STAGE_GAME; f++) {
    bool on = (f % 16u) < 8u;
    pad_repl_hold(c, on ? ((f & 16u) ? PAD_CROSS : PAD_START) : PAD_NONE);
    dc_step_frame(c, f);
  }
  if (stage() != STAGE_GAME) {
    fprintf(stderr, "[selftest] FAIL(oracle): interpreter core never reached GAME after %u frames "
                    "(stuck stage=0x%08X sm[0x48]=%u)\n", f, stage(), sm48());
    return 1;
  }
  fprintf(stderr, "[selftest] oracle: reached GAME at frame %u — now running the cutscene interpreted\n", f);
  pad_repl_hold(c, PAD_NONE);

  // Run a long window; track the loop counter and the furthest SOP scene id seen.
  uint32_t c0 = loopc(), max_scene = 0;
  const uint32_t RUN = 3000;
  for (uint32_t k = 0; k < RUN; k++, f++) {
    dc_step_frame(c, f);
    if (scene() > max_scene) max_scene = scene();
    if (verbose && (k % 250) == 0)
      fprintf(stderr, "[selftest]   oracle f=%u loop=%u sm[0x48]=%u scene(bf9b4)=%u\n",
              f, loopc(), sm48(), scene());
  }
  uint32_t adv = (uint16_t)(loopc() - c0);
  // The recomp full-PSX path froze with adv≈0 (counter stuck ~34). The interpreter must advance FAR past that.
  if (adv < 100) {
    fprintf(stderr, "[selftest] FAIL(oracle): GAME loop did not advance under interpretation "
                    "(counter +%u over %u frames; sm[0x48]=%u, max scene=%u) — the interpreter oracle froze.\n",
            adv, RUN, sm48(), max_scene);
    return 1;
  }
  fprintf(stderr, "[selftest] PASS(oracle): interpreter core RAN the cutscene without freeze/MISS "
                  "(GAME loop +%u over %u frames; furthest SOP scene id=%u).\n", adv, RUN, max_scene);
  return 0;
}

// Dispatched from boot.cpp when PSXPORT_SELFTEST is set.
int selftest_run(const char* path) {
  const char* which = cfg_str("PSXPORT_SELFTEST");
  if (which && !strcmp(which, "startgame")) return run_startgame(path);
  if (which && !strcmp(which, "narration")) return run_narration(path);
  if (which && !strcmp(which, "oracle"))    return run_oracle(path);
  fprintf(stderr, "[selftest] unknown PSXPORT_SELFTEST='%s' (known: startgame, narration, oracle)\n", which ? which : "");
  return 2;
}

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
      // NB: we do NOT require the loop to keep advancing indefinitely here — entering the first area kicks
      // off a long one-shot voice clip and the field's `while(*(0x801fe0e0)!=0)` wait legitimately pauses
      // the outer loop until the clip ENDS, and clip progress is driven by real-time audio consumption
      // (CDC_GetCDAudioSample). Headless logic-frames outrun the 44.1kHz audio, so the clip can't finish in
      // a few hundred frames — that's an audio-gated cutscene, NOT a freeze. (A headless fast-forward XA
      // pump would let this test run the whole cutscene deterministically — future workflow improvement.)
      uint32_t c0 = c->mem_r16(0x1F800198u);
      const uint32_t WIN = 180;
      for (uint32_t k = 0; k < WIN; k++, f++) dc_step_frame(c, f);
      uint32_t adv = (uint16_t)(c->mem_r16(0x1F800198u) - c0);
      if (adv > 0) {
        fprintf(stderr, "[selftest] PASS: field free-roam reached at frame %u and the GAME loop RAN "
                        "(loop counter +%u over %u frames; sm[0x48]=%u)\n", f, adv, WIN, sm48());
        return 0;
      }
      fprintf(stderr, "[selftest] FAIL: field reached sm[0x48]==2 but the GAME loop never executed "
                      "(loop counter *(0x1F800198) did not advance over %u frames).\n", WIN);
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

// Dispatched from boot.cpp when PSXPORT_SELFTEST is set.
int selftest_run(const char* path) {
  const char* which = cfg_str("PSXPORT_SELFTEST");
  if (which && !strcmp(which, "startgame")) return run_startgame(path);
  fprintf(stderr, "[selftest] unknown PSXPORT_SELFTEST='%s' (known: startgame)\n", which ? which : "");
  return 2;
}

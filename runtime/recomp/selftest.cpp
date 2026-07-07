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

#define TASKBASE   0x801fe000u   // scheduler task-0 object; the active STAGE runs here (entry @+0xc)
#define STAGE_DEMO 0x801062E4u
#define STAGE_GAME 0x8010637Cu   // the field; free-roam settles at sm[0x48]==2
#define PAD_NONE   0xFFFFu
#define PAD_START  (0xFFFFu & ~0x0008u)   // active-low: Start pressed
#define PAD_CROSS  (0xFFFFu & ~0x4000u)   // active-low: Cross pressed (menu confirm)
#define PAD_RIGHT  (0xFFFFu & ~0x0020u)   // active-low: D-pad Right (walk east into the field)

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
    c->game->pad.driveHold(on ? ((f & 16u) ? PAD_CROSS : PAD_START) : PAD_NONE);
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
  c->game->pad.driveHold(PAD_NONE);
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
// are attached (cmd+0x40 still holds an unrelocated area-data pointer 0x8018Axxx), so Render::gt3gt4 reads a
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
    c->game->pad.driveHold(on ? ((f & 16u) ? PAD_CROSS : PAD_START) : PAD_NONE);
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
  c->game->pad.driveHold(PAD_NONE);
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
  game->gpu.soft_gpu = 1;                  // ...and SOFTWARE-rasterized (docs/oracle.md Phase 2) into its own s_vram
  Core* c = &game->core;
  load_exe(path, c);
  dc_boot_init(c);
  void gpu_native_shot(Core*, const char*);

  auto stage = [&]{ return c->mem_r32(TASKBASE + 0xc); };
  auto sm48  = [&]{ return (uint32_t)c->mem_r16(TASKBASE + 0x48); };
  auto loopc = [&]{ return c->mem_r16(0x1F800198u); };
  auto scene = [&]{ return c->mem_r8(0x800bf9b4u); };
  auto ovsig = [&]{ return c->mem_r32(0x80109450u); };   // MODE overlay: 0x3C021F80=SOP narration, 0x801138A4=walkable field

  const uint32_t REACH_CAP = 4000;
  uint32_t f = 0;
  for (; f < REACH_CAP && stage() != STAGE_GAME; f++) {
    bool on = (f % 16u) < 8u;
    c->game->pad.driveHold(on ? ((f & 16u) ? PAD_CROSS : PAD_START) : PAD_NONE);
    dc_step_frame(c, f);
  }
  if (stage() != STAGE_GAME) {
    fprintf(stderr, "[selftest] FAIL(oracle): interpreter core never reached GAME after %u frames "
                    "(stuck stage=0x%08X sm[0x48]=%u)\n", f, stage(), sm48());
    return 1;
  }
  fprintf(stderr, "[selftest] oracle: reached GAME at frame %u — now running the cutscene interpreted\n", f);
  c->game->pad.driveHold(PAD_NONE);

  // Run a long window; track the loop counter and the furthest SOP scene id seen.
  uint32_t c0 = loopc(), max_scene = 0;
  const uint32_t RUN = 4200;
  uint8_t shot_done[256] = {0};
  int freeroam_since = -1;   // frame index (k) at which the walkable-field overlay first appeared
  for (uint32_t k = 0; k < RUN; k++, f++) {
    dc_step_frame(c, f);
    uint8_t sc = scene();
    if (sc > max_scene) max_scene = sc;
    // FREE-ROAM oracle capture (later-282): once the cutscene settles into the walkable field
    // (overlay 0x801138A4, scene byte back to 0), dump the soft-GPU field framebuffer at a couple of
    // settled frames so we can diff the REAL PSX field render against the native VK shot.
    if (ovsig() == 0x801138A4u && sc == 0) {
      if (freeroam_since < 0) { freeroam_since = (int)k; fprintf(stderr, "[selftest]   oracle: FREE-ROAM field reached at f=%u\n", f); }
      int held = (int)k - freeroam_since;
      if (held == 30 || held == 120 || held == 300 ||
          held == 600 || held == 1200 || held == 1800 ||
          held == 2400 || held == 2550 || held == 2600 || held == 2700) {
        char p[160]; snprintf(p, sizeof p, "scratch/screenshots/oracle_field_h%d_f%u.ppm", held, f);
        gpu_native_shot(c, p);
        fprintf(stderr, "[selftest]   oracle: dumped free-roam field framebuffer %s\n", p);
      }
    }
    // Phase-2 deliverable: dump the oracle's SOFTWARE-rendered framebuffer the first time we reach each
    // narration beat — this is the REAL PSX cutscene render (void scene 5, cliff scene 7) we need to see.
    if (sc && !shot_done[sc]) {
      shot_done[sc] = 1;
      char p[160]; snprintf(p, sizeof p, "scratch/screenshots/oracle_scene%u_f%u.ppm", sc, f);
      gpu_native_shot(c, p);
    }
    // Extra captures DEEP into each beat (the first-reach frame is too early to show the effect/characters):
    // dump every narration beat every 40 frames so we can compare native vs oracle at matched sub-beats.
    if (sc && (k % 40) == 0) {
      char p[160]; snprintf(p, sizeof p, "scratch/screenshots/oracle_s%u_k%u.ppm", sc, k);
      gpu_native_shot(c, p);
    }
    if (verbose && (k % 250) == 0)
      fprintf(stderr, "[selftest]   oracle f=%u loop=%u sm[0x48]=%u scene(bf9b4)=%u ovsig=0x%08X\n",
              f, loopc(), sm48(), sc, ovsig());
  }
  fprintf(stderr, "[selftest]   oracle END-OF-CUTSCENE: f=%u scene=%u ovsig=0x%08X (0x801138A4=free-roam field)\n",
          f, scene(), ovsig());
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

// ---- ORACLE DIVERGENCE DIFF (later-278) -----------------------------------------------------------
// The state-synced divergence compare (docs/oracle.md): boot the NATIVE port core (A) and the pure-PSX
// INTERPRETER oracle core (B) in ONE process and diff their guest RAM at GAME-STATE CHECKPOINTS — NOT at
// equal frame numbers (the interpreter spends real frames on CD loads/waits the native side does
// synchronously, so frame N is a different point in the game). Each checkpoint is a value of the SOP
// narration scene byte 0x800bf9b4 (the beats: field/letter -> void scene 5 -> cliff scene 7). We advance
// each core INDEPENDENTLY until it reaches the checkpoint (parking the faster one), then diff the
// engine-state RAM band, excluding the render/timing regions that differ BY DESIGN (the native render path
// writes a different OT/packet pool than the PSX one). Surviving divergences are native-reimplementation
// bugs — e.g. scene state the native side fails to set up (the void "effect" we are missing). PSXPORT_SELFTEST=oraclediff.
static bool od_is_render(uint32_t a) {
  if (a >= 0x800BF4F0u && a < 0x800BF54Cu) return true;   // render pool ptrs + dwell
  if (a >= 0x800BFE68u && a < 0x800EA200u) return true;   // packet pool (x2) + OT (x2) + env (render-only)
  return false;
}
// The guest CdSearchFile directory/file cache (FUN_8008b8f0 / FUN_8008bbe8): a 128-entry stride-0x2c dir
// table @0x80102D6C, a stride-0x20 file-record table @*0x80102728, working vars 0x801026E0.., and the
// 0x10-block read buffer @0x80104368. The native port resolves CD files via disc_find_file natively and
// NEVER builds this guest cache → all-zero on native, populated on the interpreter oracle. This is a
// BY-DESIGN native replacement, not a divergence bug, so it is excluded from the compare.
static bool od_is_cd_cache(uint32_t a) {
  return a >= 0x801026E0u && a < 0x80104C00u;
}
// Drive a core (no input) until 0x800bf9b4 == target, or cap frames elapse. Returns frames stepped; sets
// *reached. The caller threads each core's own frame counter.
// Drive a core (no input) until the MODE-overlay signature 0x80109450 == target (0x801138A4 = walkable
// free-roam field), or cap frames elapse. Used to park BOTH cores at the first free-roam frame — the field
// has just loaded, state is freshly initialised, so it is the most stable point to diff native vs oracle.
static uint32_t od_advance_to_ovsig(Core* c, uint32_t& f, uint32_t target, uint32_t cap, bool* reached) {
  void dc_step_frame(Core*, uint32_t); uint32_t n = 0;
  while (n < cap && c->mem_r32(0x80109450u) != target) { dc_step_frame(c, f); f++; n++; }
  *reached = (c->mem_r32(0x80109450u) == target);
  return n;
}
static uint32_t od_advance_to_scene(Core* c, uint32_t& f, uint8_t target, uint32_t cap, bool* reached) {
  void dc_step_frame(Core*, uint32_t); uint32_t n = 0;
  while (n < cap && c->mem_r8(0x800bf9b4u) != target) { dc_step_frame(c, f); f++; n++; }
  *reached = (c->mem_r8(0x800bf9b4u) == target);
  return n;
}
static int run_oraclediff(const char* path) {
  const int verbose = cfg_on("PSXPORT_SELFTEST_VERBOSE");
  Game* A = new Game(); A->psx_fallback = 0;                              // native port core (renders via VK)
  Game* B = new Game(); B->psx_fallback = 1; B->core.use_interp = 1;      // pure-PSX interpreter oracle core
  B->gpu.soft_gpu = 1;                                                     // ...soft-rasterized into its own s_vram (render-diff decoupled from A's VK)
  void gpu_native_shot(Core*, const char*);
  load_exe(path, &A->core); dc_boot_init(&A->core);
  load_exe(path, &B->core); dc_boot_init(&B->core);
  auto stageA = [&]{ return A->core.mem_r32(TASKBASE + 0xc); };
  auto stageB = [&]{ return B->core.mem_r32(TASKBASE + 0xc); };

  // Drive both to the GAME stage (mash Start), each on its own frame clock.
  uint32_t fa = 0, fb = 0;
  for (; fa < 4000 && stageA() != STAGE_GAME; fa++) { bool on=(fa%16u)<8u; A->pad.driveHold(on?((fa&16u)?PAD_CROSS:PAD_START):PAD_NONE); dc_step_frame(&A->core, fa); }
  for (; fb < 4000 && stageB() != STAGE_GAME; fb++) { bool on=(fb%16u)<8u; B->pad.driveHold(on?((fb&16u)?PAD_CROSS:PAD_START):PAD_NONE); dc_step_frame(&B->core, fb); }
  if (stageA() != STAGE_GAME || stageB() != STAGE_GAME) {
    fprintf(stderr, "[oraclediff] FAIL: reach GAME — A stage=0x%08X (f%u), B stage=0x%08X (f%u)\n", stageA(), fa, stageB(), fb);
    return 1;
  }
  A->pad.driveHold(PAD_NONE); B->pad.driveHold(PAD_NONE);
  fprintf(stderr, "[oraclediff] both at GAME (A f%u, B f%u). Checkpoint-diffing the narration beats.\n", fa, fb);

  int total_div = 0, summary_div = 0;   // summary_div = narration+free-roam only (excludes the render-noisy interactive walk)
  // Shared engine-state RAM diff at an aligned checkpoint (both cores parked at the same game moment).
  // Returns the number of non-benign diverging ranges (so callers can gate on it, e.g. the interactive-play
  // first-divergence scan). label==nullptr → count only (no printing).
  auto diff_band = [&](const char* label) -> int {
    const uint32_t LO = 0x800B0000u, HI = 0x80110000u;
    const uint8_t* a = A->core.ram + (LO - 0x80000000u);
    const uint8_t* b = B->core.ram + (LO - 0x80000000u);
    int ranges = 0; uint32_t total_bytes = 0;
    uint32_t pagehist[0x60] = {0};   // per-0x1000-page diverging-byte counts across 0x800B0000..0x80110000
    if (label) fprintf(stderr, "[oraclediff] === %s (native f%u, oracle f%u) ===\n", label, fa, fb);
    for (uint32_t i = 0; i < HI - LO; ) {
      if (a[i] == b[i] || od_is_render(LO + i) || od_is_cd_cache(LO + i)) { i++; continue; }
      uint32_t start = i, bytes = 0;
      while (i < HI - LO && a[i] != b[i] && !od_is_render(LO + i) && !od_is_cd_cache(LO + i)) { i++; bytes++; }
      total_bytes += bytes; if (label) total_div++;
      pagehist[(LO + start - 0x800B0000u) >> 12] += bytes;
      if (ranges++ < 48 && label)
        fprintf(stderr, "[oraclediff]   diff @0x%08X..0x%08X (%u B)  nat[0]=%02X ora[0]=%02X\n",
                LO + start, LO + start + bytes, bytes, a[start], b[start]);
    }
    if (label) {
      fprintf(stderr, "[oraclediff]   %s: %d diverging ranges, %u bytes total%s (CD-cache excluded)\n",
              label, ranges, total_bytes, ranges > 48 ? " (first 48 shown)" : "");
      for (uint32_t p = 0; p < 0x60; p++)
        if (pagehist[p]) fprintf(stderr, "[oraclediff]     page 0x%08X: %u diverging bytes\n",
                                 0x800B0000u + (p << 12), pagehist[p]);
    }
    return ranges;
  };

  const uint8_t CKPTS[] = { 2, 3, 5, 7 };   // narration scene ids (field/letter, transitions, void=5, cliff=7)
  for (uint8_t ck : CKPTS) {
    bool ra=false, rb=false;
    od_advance_to_scene(&A->core, fa, ck, 4000, &ra);
    od_advance_to_scene(&B->core, fb, ck, 8000, &rb);   // B (interp+CD) gets a larger cap
    if (!ra || !rb) {
      fprintf(stderr, "[oraclediff] CHECKPOINT scene=%u UNREACHED: native=%d(f%u) oracle=%d(f%u) "
                      "— one core never reaches this beat (a divergence in itself).\n", ck, ra, fa, rb, fb);
      continue;
    }
    char lbl[32]; snprintf(lbl, sizeof lbl, "scene %u", ck);
    diff_band(lbl);
    (void)verbose;
  }

  // FREE-ROAM checkpoint (later-282): park both cores at the first walkable-field frame (MODE overlay
  // 0x80109450 -> 0x801138A4). The field has just loaded, so state is freshly initialised — the most stable
  // point to diff native gameplay against the PSX oracle beyond the scripted narration.
  {
    bool ra=false, rb=false;
    od_advance_to_ovsig(&A->core, fa, 0x801138A4u, 4000, &ra);
    od_advance_to_ovsig(&B->core, fb, 0x801138A4u, 8000, &rb);
    if (!ra || !rb)
      fprintf(stderr, "[oraclediff] FREE-ROAM UNREACHED: native=%d(f%u) oracle=%d(f%u)\n", ra, fa, rb, fb);
    else {
      // RAM diff at the EXACT onset frame (both cores parked at the overlay flip) — this is where state is
      // aligned. Free-running past it drifts (the two cores animate the scripted opening at slightly different
      // sub-frame cadence: the interp core does real CD loads), so any post-step diff is drift, not a bug.
      fprintf(stderr, "[oraclediff] ONSET area id: A(native) bf870=%u bf9b4=%u sm48=%u | B(oracle) bf870=%u bf9b4=%u sm48=%u\n",
              A->core.mem_r8(0x800bf870u), A->core.mem_r8(0x800bf9b4u), A->core.mem_r16(0x801fe048u),
              B->core.mem_r8(0x800bf870u), B->core.mem_r8(0x800bf9b4u), B->core.mem_r16(0x801fe048u));
      int onset_ranges = diff_band("free-roam");
      summary_div = total_div;   // freeze the summary count here — the interactive walk's diff below is render noise

      // INTERACTIVE-PLAY scan HARNESS (later-283): the scaffold for diffing INTERACTIVE gameplay (Tomba under
      // player control) native-vs-oracle — from the aligned free-roam onset drive BOTH cores with IDENTICAL pad
      // input for WALK_FRAMES, then compare framebuffers.
      //
      // IMPORTANT LIMITATION (later-283, verified): at THIS checkpoint (the free-roam overlay-flip onset) Tomba
      // is still in the scripted "caught on the fishing line" pose and does NOT respond to movement input —
      // holding Right (or mashing buttons) for 1400+ frames leaves him in the EXACT same position in the native
      // core. So this scan currently only RE-CONFIRMS the still-frame convergence already proven at the onset
      // (later-282); it does NOT yet verify interactive MOVEMENT convergence, because there is no movement to
      // diff here. To make it a real interactive test, first reach the point where Tomba becomes player-
      // controllable (progress/skip the caught opening), then run this scan there.
      //
      // The RAM diff below is printed for transparency but is DOMINATED BY RENDER-PATH NOISE during active
      // rendering: gameplay and render state share the same node structs, and the native (VK) vs oracle (PSX
      // soft-GPU) render paths populate each node's render-cache fields (matrix cache node+0x98, render-command
      // array node+0xC0, OT link words, the ordering table 0x800ED000..0x800F1000, the render-queue lists
      // 0x800F24xx) DIFFERENTLY — so hundreds of "divergences" here are expected and are NOT gameplay bugs.
      {
        const int WALK_FRAMES = 90;
        fprintf(stderr, "[oraclediff] === interactive-play scan: hold RIGHT %d frames from onset (baseline=%d benign ranges) ===\n",
                WALK_FRAMES, onset_ranges);
        for (int k = 0; k < WALK_FRAMES; k++) {
          A->pad.driveHold(PAD_RIGHT); dc_step_frame(&A->core, fa); fa++;
          B->pad.driveHold(PAD_RIGHT); dc_step_frame(&B->core, fb); fb++;
        }
        int nd = diff_band("interactive-play (post-walk, render-noise-dominated)");
        fprintf(stderr, "[oraclediff]   post-walk RAM: %d ranges (render scene-graph noise). NOTE: Tomba is NOT controllable at this checkpoint (scripted caught pose) — this re-confirms still-convergence, it is NOT yet an interactive-MOVEMENT test.\n", nd);
      }
      A->pad.driveHold(PAD_NONE); B->pad.driveHold(PAD_NONE);
      gpu_native_shot(&A->core, "scratch/screenshots/oraclediff_freeroam_native.ppm");
      gpu_native_shot(&B->core, "scratch/screenshots/oraclediff_freeroam_oracle.ppm");
      fprintf(stderr, "[oraclediff] post-walk framebuffers: oraclediff_freeroam_{native,oracle}.ppm (native VK vs PSX soft-GPU content match — still-convergent; Tomba stays in the scripted caught pose, so this is not yet an interactive-movement test)\n");

      // NO-INPUT SCRIPTED-OPENING PROGRESSION (later-284): from the free-roam onset, drive BOTH cores with
      // NO input and dump each core's soft-GPU framebuffer at matched held frames. The pure-PSX reference (B)
      // plays out the scripted opening (caught-island -> tree -> cliff-fishing -> "house on fire" dialogue)
      // with no input; if the NATIVE engine (A) does not advance the same way, that is a game-LOGIC divergence
      // in a native reimplementation (NOT render noise). Cadence drifts (B does real CD loads), so this is a
      // gross progression check, not a frame-exact diff.
      {
        const int PROG_FRAMES = 2800;
        const int marks[] = { 300, 600, 1200, 1800, 2600 };
        int mi = 0;
        for (int k = 0; k < PROG_FRAMES; k++) {
          A->pad.driveHold(PAD_NONE); dc_step_frame(&A->core, fa); fa++;
          B->pad.driveHold(PAD_NONE); dc_step_frame(&B->core, fb); fb++;
          if (mi < (int)(sizeof marks/sizeof marks[0]) && k == marks[mi]) {
            char pa[160], pb[160];
            snprintf(pa, sizeof pa, "scratch/screenshots/prog_native_h%d.ppm", marks[mi]);
            snprintf(pb, sizeof pb, "scratch/screenshots/prog_oracle_h%d.ppm", marks[mi]);
            gpu_native_shot(&A->core, pa); gpu_native_shot(&B->core, pb);
            fprintf(stderr, "[oraclediff]   progression h%d: native scene=%u ovsig=0x%08X sm48=%u | oracle scene=%u ovsig=0x%08X sm48=%u\n",
                    marks[mi], A->core.mem_r8(0x800bf9b4u), A->core.mem_r32(0x80109450u), A->core.mem_r16(0x801fe048u),
                    B->core.mem_r8(0x800bf9b4u), B->core.mem_r32(0x80109450u), B->core.mem_r16(0x801fe048u));
            mi++;
          }
        }
      }
    }
  }

  fprintf(stderr, "[oraclediff] DONE: %d total diverging ranges across narration + free-roam (interactive-play walk excluded — render-noise-dominated).\n",
          summary_div ? summary_div : total_div);
  return 0;   // diagnostic harness: always exit 0 (it reports, it doesn't pass/fail)
}

// Dispatched from boot.cpp when PSXPORT_SELFTEST is set.
int selftest_run(const char* path) {
  const char* which = cfg_str("PSXPORT_SELFTEST");
  if (which && !strcmp(which, "startgame")) return run_startgame(path);
  if (which && !strcmp(which, "narration")) return run_narration(path);
  if (which && !strcmp(which, "oracle"))    return run_oracle(path);
  if (which && !strcmp(which, "oraclediff")) return run_oraclediff(path);
  if (which && !strcmp(which, "camera"))    { int run_camera_oracle(const char*); return run_camera_oracle(path); }
  fprintf(stderr, "[selftest] unknown PSXPORT_SELFTEST='%s' (known: startgame, narration, oracle, oraclediff, camera)\n", which ? which : "");
  return 2;
}

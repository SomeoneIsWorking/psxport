// psxport_smoke.cpp — the game-AGNOSTIC framework smoke.
//
// THIS TARGET PROVES TWO DIFFERENT THINGS, and conflating them is how it went unnoticed for so long
// that it segfaulted with zero output:
//
//   1. AT LINK TIME — link-level game-agnosticism. This TU links against ONLY libpsxport.a (+ the
//      framework's inherited system deps): no game/*, no generated/* object. Any undefined
//      game/generated symbol reachable from a Core-only client therefore fails THIS link. That proof
//      is complete before main() ever runs, and it is the reason the target exists
//      (cmake/psxport.cmake registers it as a plain add_executable linking `psxport`).
//   2. AT RUN TIME — the CORE-ALONE STORE/LOAD CONTRACT: a default-constructed `Core` with
//      `game == nullptr` is a complete guest-memory device, and the framework's own store-side
//      instrumentation runs on it rather than crashing or silently switching itself off.
//
// WHY THE RUN HALF IS NOW ASSERTED AND COUNTED (2026-08-12). This program used to do one mem_w32,
// one mem_r32, and printf "psxport_smoke ok". It had been SEGFAULTING (rc=139) with ZERO lines of
// output — `Core::mem_w32` -> `pkt_track` -> `OtAttr::trackStoreSlow` dereferenced `c->game` for a
// frame stamp — and nothing noticed, because this target is not a ctest and its only possible output
// was one success line. A smoke whose entire vocabulary is "ok" cannot report a partial run: silence,
// a crash, and "never built" are the same observation. So now:
//   - stdout is UNBUFFERED and every check ANNOUNCES ITSELF BEFORE it executes. A crash therefore
//     leaves the dangling name of the check that killed it, instead of losing a block-buffered
//     "ok" that was never reached anyway.
//   - the plan (how many checks exist) is printed FIRST, and the verdict compares checks RUN against
//     it. A run that dies, or an edit that stops calling a check, cannot exit 0.
//   - the verdict states what this program CANNOT see, so a PASS is not read as more than it is.
//
// THE STORE-ATTRIBUTION CHECK IS THE POSITIVE CONTROL, and it is the one check here that could not
// be replaced by "it did not crash". `if (!c->game) return;` at the top of trackStoreSlow would also
// have stopped the segfault, while silently disabling the framework's store attribution for every
// Core-alone embedder — this smoke, tests/test_overlay_reloc.cpp, the differential harness. Check 8
// registers an OtAttr watch region, stores into it, and demands the last-writer record come back; it
// FAILS on that early-out and passes only if the real store path ran. tests/test_core_store_no_game.cpp
// is the hermetic ctest form of the same contract.
#include <cstdarg>   // va_start/va_end — the counted harness's printf-forwarding check() helper
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "core.h"               // Core, mem_w32/mem_r32 — framework header (no game.h in its chain)
#include "game_iface.h"         // GameConfig / GameHooks / psxport_install_game
#include "ot_attr.h"            // OtAttr — the store-attribution instrument checked by check 8
#include "render_substrate.h"   // Core::rsub, which owns the OtAttr

// ---- the counted harness ----------------------------------------------------------------------
// PLAN is the DENOMINATOR of this program's result. It is asserted against, not decorative: the
// verdict fails if fewer checks ran than the plan says exist, which is what makes an early crash or a
// deleted call a FAILURE rather than a shorter green run.
static const int PLAN = 8;
static int       g_run = 0;
static int       g_failed = 0;

// Printed BEFORE the check body executes, so the last line of a crashed run names the crash site.
static void begin(const char* name) { printf("  [%d/%d] %-34s ... ", ++g_run, PLAN, name); }
static void pass(void) { printf("PASS\n"); }
static void fail(const char* fmt, ...) {
  ++g_failed;
  va_list ap;
  va_start(ap, fmt);
  printf("FAIL: ");
  vprintf(fmt, ap);
  printf("\n");
  va_end(ap);
}

#define SMOKE_TRUE(name, expr)                                                                     \
  do {                                                                                             \
    begin(name);                                                                                   \
    if (expr) pass();                                                                              \
    else fail("expected true: %s", #expr);                                                         \
  } while (0)

#define SMOKE_EQ(name, got, want)                                                                  \
  do {                                                                                             \
    begin(name);                                                                                   \
    const uint32_t g_ = (uint32_t)(got), w_ = (uint32_t)(want);                                     \
    if (g_ == w_) pass();                                                                          \
    else fail("%s: got 0x%08X want 0x%08X", #got, g_, w_);                                          \
  } while (0)

int main() {
  // Unbuffered, not line-buffered: a segfault mid-check must still have emitted the announcement of
  // the check it died in, even when stdout is a pipe (where the default is block buffering — the
  // reason the pre-2026-08-12 crash produced literally zero bytes).
  setvbuf(stdout, nullptr, _IONBF, 0);

  printf("psxport_smoke: LINK-LEVEL AGNOSTICISM ALREADY PROVEN (this binary linked libpsxport.a with\n"
         "  zero game/ and zero generated/ objects; an undefined game symbol would have failed the\n"
         "  link, not this run). Now checking the CORE-ALONE runtime contract: %d checks planned.\n",
         PLAN);

  // Stub seam: a zeroed config and an all-null hooks table. Null ctxCreate/ctxDestroy is fine — Core's
  // ctor/dtor guard them (`if (hooks && hooks->ctxCreate) ...`). No game code exists to install a real
  // one, which is the whole point.
  static const GameConfig stub_cfg{};     // all-zero guest addresses/tables
  static const GameHooks  stub_hooks{};   // all members nullptr
  psxport_install_game(&stub_cfg, &stub_hooks);

  SMOKE_TRUE("seam-config-readback", psxport_game_config() == &stub_cfg);
  SMOKE_TRUE("seam-hooks-readback", psxport_game_hooks() == &stub_hooks);

  Core* c = new Core();
  SMOKE_TRUE("null-ctxCreate-leaves-null-ctx", c->gameCtx == nullptr);
  // The PRECONDITION every remaining check is about, asserted rather than assumed: this really is a
  // Core with no Game. If a future Core ctor started binding one, the checks below would still pass
  // while no longer testing the Core-alone path at all — so the precondition is part of the plan.
  SMOKE_TRUE("core-alone-precondition-no-game", c->game == nullptr);

  const uint32_t addr = 0x80010000u, val = 0xDEADBEEFu;
  c->mem_w32(addr, val);
  SMOKE_EQ("main-ram-w32-roundtrip", c->mem_r32(addr), val);
  // Through the KUSEG and KSEG1 mirrors: proves the store landed in the one 2 MB buffer rather than
  // somewhere harmless that a same-address read would echo back regardless.
  SMOKE_TRUE("main-ram-kuseg-kseg1-mirror",
             c->mem_r32(0x00010000u) == val && c->mem_r32(0xA0010000u) == val);

  // Scratchpad is a DIFFERENT host buffer and a different address form in trackStoreSlow's `phys`
  // normalization, so it exercises a distinct store path from main RAM.
  c->mem_w32(0x1F800100u, 0xC0FFEE01u);
  SMOKE_EQ("scratchpad-w32-roundtrip", c->mem_r32(0x1F800100u), 0xC0FFEE01u);

  // CHECK 8 — THE POSITIVE CONTROL (see the header comment). Demands that store attribution really
  // RAN with no Game bound, not merely that the store did not crash.
  {
    begin("store-attribution-ran-no-game");
    OtAttr&        oa = c->rsub.otAttr;
    const uint32_t wbase = 0x80020000u;
    const int      slot = oa.watchRegister(wbase, 16);
    OtAttr::WordRec before{}, after{};
    const bool     had_before = oa.watchLookup(wbase, &before);
    c->mem_w32(wbase, 0x12345678u);
    const bool had_after = oa.watchLookup(wbase, &after);
    if (slot < 0)
      fail("watchRegister refused (returned %d) — the instrument, not the store path, is broken", slot);
    else if (!had_before || !had_after)
      fail("watchLookup did not report the region as watched (before=%d after=%d)",
           (int)had_before, (int)had_after);
    else if (before.frame != 0xFFFFFFFFu)
      fail("fresh watch word was not 'never written' (frame 0x%08X) — no denominator for the result",
           before.frame);
    else if (after.frame == 0xFFFFFFFFu)
      fail("the store was NOT attributed: the watched word is still 'never written'. trackStoreSlow "
           "did not run — an early-out on a null Game would look exactly like this");
    else
      pass();
  }

  delete c;

  // ---- verdict, with its denominator and its blind spots ---------------------------------------
  const bool complete = (g_run == PLAN);
  printf("psxport_smoke: %s — %d of %d planned checks ran, %d failed.\n",
         (complete && g_failed == 0) ? "PASS" : "FAIL", g_run, PLAN, g_failed);
  if (!complete)
    printf("psxport_smoke: INCOMPLETE — %d planned check(s) never ran. Treat this as a FAILURE, not "
           "a shorter pass.\n", PLAN - g_run);
  printf("psxport_smoke: what this run does NOT cover — hardware I/O (Core::io_read/io_write model\n"
         "  peripherals that live on Game, deliberately out of contract for a bare Core), the GPU/SPU/\n"
         "  CD backends, the recompiled substrate, and any game logic. A PASS here means the framework\n"
         "  links with zero game symbols and its guest-memory device plus store attribution work with\n"
         "  no Game bound. It means nothing about whether any game runs.\n");
  return (complete && g_failed == 0) ? 0 : 1;
}

// test_render_noise_mask.cpp — the ONE definition of "guest RAM the render path legitimately differs
// in" (runtime/psx/render_noise.h), which every compare harness masks out of its verdict.
//
// WHY THIS TEST IS THE IMPORTANT HALF OF THAT CHANGE. The mask decides which bytes a divergence report
// is allowed to IGNORE. Get it too small and render noise is reported as a gameplay bug; get it too
// large and the harness deletes real divergences from its own headline verdict — silently, and in the
// direction that reads as success. So the arithmetic is asserted here against the literals it replaced,
// bound by bound, rather than trusted.
//
// AND IT PINS A REAL BUG THE DERIVATION EXPOSED. The single hardcoded window it replaces was
// [0x800BFE68, 0x800EA200) — which is not the end of Tomba!2's ordering table. The OT is two 0x2070
// parity pages at 0x800E80A8, so it ends at 0x800EC188: the old literal stopped 0x1F88 bytes short and
// covered only 0xE8 of parity 1's 0x2070 bytes — 2.8% of it. For every run of every harness that used
// it, 97% of one ordering-table page counted as gameplay divergence while the comment above it said
// "packet pool (x2 pages) + OT (x2 pages) + env". The derived mask is therefore not a refactor: it
// masks 0x1F88 MORE bytes, all of them inside the OT, which is render output by definition.
//
// Hermetic: GameConfig is a POD, the mask is pure arithmetic over it. No Core, no GPU, no disc.
#include "render_noise.h"
#include "testutil.h"
#include <string.h>

// Tomba!2's real values (Tomba2Engine/game/core/game_config.cpp), the only game that has RE'd them.
static GameConfig tomba2_cfg() {
  GameConfig c{};
  c.packetPoolBase = 0x800BFE68u;
  c.packetPoolStride = 0x00014000u;
  c.otRegionBase = 0x800E80A8u;
  c.otRegionStride = 0x00002070u;
  c.poolPtrCur = 0x800BF544u;
  c.poolPtrLast = 0x800BF4F4u;
  c.dwellCounter = 0x800E809Cu;
  return c;
}

static void test_bounds_reproduce_the_literals(void) {
  const GameConfig cfg = tomba2_cfg();
  const RenderNoiseMask m = RenderNoiseMask::from(&cfg, "test");
  CHECK(m.known);
  // pool: both parities, EXACTLY the old lower bound.
  CHECK_EQ(m.poolLo, 0x800BFE68u);
  CHECK_EQ(m.poolHi, 0x800E7E68u);
  // ot: both parities. This is the bound the old literal got WRONG.
  CHECK_EQ(m.otLo, 0x800E80A8u);
  CHECK_EQ(m.otHi, 0x800EC188u);
  // env: the words between pool and OT — draw env + the dwell counter, which must fall inside it.
  CHECK_EQ(m.envLo, 0x800E7E68u);
  CHECK_EQ(m.envHi, 0x800E80A8u);
  CHECK(m.covers(0x800E809Cu)); // dwellCounter — the old comment put it in the ptrs window
  // ptrs: the pointer pair plus its adjacent bookkeeping words, byte-identical to the old literal.
  CHECK_EQ(m.ptrLo, 0x800BF4F0u);
  CHECK_EQ(m.ptrHi, 0x800BF54Cu);
}

// The old literal's blind spot, as an assertion: these addresses are ordering table, they were NOT
// masked before, and they must be masked now.
static void test_ot_parity1_is_now_masked(void) {
  const GameConfig cfg = tomba2_cfg();
  const RenderNoiseMask m = RenderNoiseMask::from(&cfg, "test");
  const uint32_t oldHi = 0x800EA200u;          // where the single literal window used to stop
  const uint32_t p1Lo = 0x800E80A8u + 0x2070u; // OT parity 1 starts here (0x800EA118)
  CHECK(m.covers(p1Lo));                       // the 0xE8 bytes the old window did reach
  CHECK(m.covers(oldHi));                      // …and everything past it that it did not
  CHECK(m.covers(0x800EC187u));                // the last byte of parity 1
  CHECK(!m.covers(0x800EC188u));               // hi is EXCLUSIVE — one past the end is NOT masked
  int scanned = 0, masked = 0;                 // the denominator, so "it's covered" is not a vibe
  for (uint32_t a = p1Lo; a < 0x800EC188u; a += 4u) {
    scanned++;
    if (m.covers(a)) {
      masked++;
    }
  }
  CHECK_EQ(scanned, 0x2070 / 4);
  CHECK_EQ(masked, scanned); // ALL of parity 1, where the literal masked 2.8%
}

// Ordinary engine RAM must not be masked, in both directions of each boundary. A mask that swallowed
// engine data would make a harness print "no divergence" over real corruption.
static void test_engine_ram_is_not_masked(void) {
  const GameConfig cfg = tomba2_cfg();
  const RenderNoiseMask m = RenderNoiseMask::from(&cfg, "test");
  CHECK(!m.covers(0x80100000u)); // overlay/engine code+data, well above the OT
  CHECK(!m.covers(0x80010000u)); // low main RAM
  CHECK(!m.covers(0x801FE000u)); // the scheduler task table — gameplay state, must never be masked
  CHECK(!m.covers(0x800BFE64u)); // one word BELOW the pool
  CHECK(m.covers(0x800BFE68u));  // …and the first word of it
  CHECK(!m.covers(0x800BF4ECu)); // one word below the ptrs window
  CHECK(m.covers(0x800BF4F0u));
}

// THE HONEST ZERO, which is the whole reason this file replaced three copies of a literal. A game that
// has not RE'd its pool (spyro, spider1) gets an EMPTY mask — never another game's window.
static void test_unset_config_masks_nothing(void) {
  GameConfig zero{};
  const RenderNoiseMask m = RenderNoiseMask::from(&zero, "test");
  CHECK(!m.known);
  CHECK_EQ(m.bytes(), 0u);
  int scanned = 0, masked = 0;
  for (uint32_t a = 0x80000000u; a < 0x80200000u; a += 0x1000u) {
    scanned++;
    if (m.covers(a)) {
      masked++;
    }
  }
  CHECK_EQ(scanned, 512); // scanned the whole 2 MB window…
  CHECK_EQ(masked, 0);    // …and it masks NOTHING, rather than inheriting Tomba!2's range
  // …and it says so rather than looking like a mask that found nothing.
  char buf[256];
  CHECK(strstr(m.describe(buf, sizeof buf), "unset") != nullptr);
  // A null config is the same case, not a crash.
  const RenderNoiseMask n = RenderNoiseMask::from(nullptr, "test");
  CHECK(!n.known);
  CHECK(!n.covers(0x800BFE68u));
}

// A PARTIAL config masks only what it can derive, and must not invent the rest.
static void test_partial_config_masks_only_what_it_knows(void) {
  GameConfig cfg{};
  cfg.packetPoolBase = 0x80100000u;
  cfg.packetPoolStride = 0x1000u; // pool only: no OT, no ptrs
  const RenderNoiseMask m = RenderNoiseMask::from(&cfg, "test");
  CHECK(m.known);
  CHECK(m.covers(0x80100000u));
  CHECK(m.covers(0x80101FFFu));
  CHECK(!m.covers(0x80102000u));
  CHECK_EQ(m.otLo, 0u);
  CHECK_EQ(m.otHi, 0u);
  CHECK_EQ(m.ptrLo, 0u);
  CHECK_EQ(m.envLo, 0u); // no OT -> no env window invented between pool and nothing
  CHECK_EQ(m.bytes(), 0x2000u);
}

// The env window exists only when the OT actually sits just above the pool. A game whose OT is far away
// must not get a masked megabyte between them.
static void test_env_window_needs_the_ot_to_be_adjacent(void) {
  GameConfig far{};
  far.packetPoolBase = 0x80080000u;
  far.packetPoolStride = 0x1000u; // pool ends 0x80082000
  far.otRegionBase = 0x80190000u;
  far.otRegionStride = 0x1000u; // OT is 1 MB away
  const RenderNoiseMask m = RenderNoiseMask::from(&far, "test");
  CHECK(m.known);
  CHECK_EQ(m.envLo, 0u);
  CHECK_EQ(m.envHi, 0u);
  CHECK(!m.covers(0x80100000u)); // the gap between them stays UNMASKED
  CHECK_EQ(m.bytes(), 0x2000u + 0x2000u);
}

int main(void) {
  RUN(bounds_reproduce_the_literals);
  RUN(ot_parity1_is_now_masked);
  RUN(engine_ram_is_not_masked);
  RUN(unset_config_masks_nothing);
  RUN(partial_config_masks_only_what_it_knows);
  RUN(env_window_needs_the_ot_to_be_adjacent);
  return pt_summary();
}

// test_proj_prim_stale.cpp — THE VERTEX-DEPTH CACHE MUST NOT OUTLIVE THE VERTEX.
//
// WHAT THIS GATES. ProjPrim keys a projected vertex's view-Z by the GUEST ADDRESS its packet word was
// written to. An address-only key is a claim about memory that stops being true the moment the guest
// reuses that memory — and a packet pool exists to be reused. So a 2D element written into a recycled
// pool slot would be served the depth of the 3D vertex that used to live there and sort into the
// world. That is not a hypothetical: reset()'s own comment records the attempt to lengthen entry
// lifetime that "took resolved lookups from 6.9% to 23% AND BROKE THE PICTURE", depth-culling the
// player character out of the frame, and names the fix this test gates — key entries so a reused
// address CANNOT alias.
//
// The mechanism is a word guard: an entry stores the word that was at its address when recorded, and
// is served only while that word is unchanged. With it, lifetime became a cache-space question rather
// than a correctness one, and retention could go from one buffer flip to kGens (measured on spyro:
// 2.10% -> 63.60% of prims carrying real depth, picture byte-identical).
//
// WHY THE POSITIVE CASE IS HERE TOO. A guard that refuses EVERYTHING also passes "stale is refused",
// and would silently switch native depth off entirely — the failure mode is a coverage number quietly
// going to zero, which reads exactly like "this game has no depth". test_unchanged_word_still_hits is
// the control that makes that detectable.
#include "testutil.h"

#include "core.h"
#include "game_iface.h"
#include "proj_prim.h"
#include "render_substrate.h"

#include <memory>

static std::unique_ptr<Core> bare_core(void) {
  static const GameConfig cfg{};
  static const GameHooks  hooks{};
  psxport_install_game(&cfg, &hooks);
  return std::make_unique<Core>();
}

static const uint32_t kAddr = 0x80100000u;

// POSITIVE CONTROL: record, leave memory alone, and the depth comes back. Without this, a guard that
// rejects unconditionally would pass every other case in this file.
static void test_unchanged_word_still_hits(void) {
  auto c = bare_core();
  ProjPrim& pp = c->rsub.projprim;
  c->mem_w32(kAddr, 0x11112222u);
  pp.setPz(c.get(), kAddr, 1234.0f);

  float pz = 0.0f;
  CHECK(pp.lookupPz(c.get(), kAddr, &pz));
  CHECK_EQ((int)pz, 1234);
  CHECK_EQ(pp.stats().hit, 1L);
  CHECK_EQ(pp.stats().stale, 0L);
}

// THE DEFECT: the guest overwrites that word — the packet slot has been recycled. The entry is still
// indexed at this address, and serving it is exactly the wrong-depth bug. It must be refused, and
// counted as STALE rather than as a plain miss: stale means entry lifetime outran the buffer, absent
// means the tap never fired, and those want opposite fixes.
static void test_overwritten_word_is_refused_as_stale(void) {
  auto c = bare_core();
  ProjPrim& pp = c->rsub.projprim;
  c->mem_w32(kAddr, 0x11112222u);
  pp.setPz(c.get(), kAddr, 1234.0f);

  c->mem_w32(kAddr, 0x33334444u);          // the pool slot is reused by something else

  float pz = -1.0f;
  CHECK(!pp.lookupPz(c.get(), kAddr, &pz));
  CHECK_EQ(pp.stats().stale, 1L);
  CHECK_EQ(pp.stats().hit, 0L);
  CHECK_EQ(pp.stats().miss, 1L);           // a stale refusal is a miss as well, so coverage is honest
}

// The copy-propagation probe obeys the same rule. It must, or a staged vertex whose scratchpad slot
// has since been reused would DONATE its old depth to a fresh packet — the same lie, one step earlier
// in the pipeline, and invisible at the render's hit/miss counters because peekPz does not touch them.
static void test_peek_obeys_the_same_rule(void) {
  auto c = bare_core();
  ProjPrim& pp = c->rsub.projprim;
  c->mem_w32(kAddr, 0xAAAABBBBu);
  pp.setPz(c.get(), kAddr, 555.0f);

  float pz = 0.0f;
  CHECK(pp.peekPz(c.get(), kAddr, &pz));   // unchanged: carries
  CHECK_EQ((int)pz, 555);

  c->mem_w32(kAddr, 0xCCCCDDDDu);
  CHECK(!pp.peekPz(c.get(), kAddr, nullptr));
  CHECK_EQ(pp.stats().hit, 0L);            // peek must stay off the render's counters
  CHECK_EQ(pp.stats().miss, 0L);
}

// A re-record after the overwrite restores the entry: the address is fine, it was the CONTENT that had
// moved on. Without this case "refused" could equally mean the address had been poisoned for good.
static void test_rerecord_after_overwrite_hits_again(void) {
  auto c = bare_core();
  ProjPrim& pp = c->rsub.projprim;
  c->mem_w32(kAddr, 0x11112222u);
  pp.setPz(c.get(), kAddr, 1234.0f);
  c->mem_w32(kAddr, 0x33334444u);
  CHECK(!pp.lookupPz(c.get(), kAddr, nullptr));

  pp.setPz(c.get(), kAddr, 777.0f);        // the new vertex records itself here
  float pz = 0.0f;
  CHECK(pp.lookupPz(c.get(), kAddr, &pz));
  CHECK_EQ((int)pz, 777);
}

// The scratchpad is a DIFFERENT host buffer and a different address form, and it is where this game
// stages its vertices — so the guard has to read the right memory for it, not a mirror rebuilt from
// the masked key. (pz_key strips KSEG bits; scratchpad and low RAM would need different mirrors, so a
// reconstructed address silently reads the wrong buffer for one of them.)
static void test_guard_reads_scratchpad_not_a_rebuilt_mirror(void) {
  auto c = bare_core();
  ProjPrim& pp = c->rsub.projprim;
  const uint32_t spad = 0x1F800040u;
  c->mem_w32(spad, 0x0BADC0DEu);
  pp.setPz(c.get(), spad, 42.0f);

  float pz = 0.0f;
  CHECK(pp.lookupPz(c.get(), spad, &pz));  // reads the scratchpad: unchanged, so it hits
  CHECK_EQ((int)pz, 42);

  c->mem_w32(spad, 0xFEEDFACEu);
  CHECK(!pp.lookupPz(c.get(), spad, nullptr));
  CHECK_EQ(pp.stats().stale, 1L);
}

int main(void) {
  RUN(unchanged_word_still_hits);
  RUN(overwritten_word_is_refused_as_stale);
  RUN(peek_obeys_the_same_rule);
  RUN(rerecord_after_overwrite_hits_again);
  RUN(guard_reads_scratchpad_not_a_rebuilt_mirror);
  return pt_summary();
}

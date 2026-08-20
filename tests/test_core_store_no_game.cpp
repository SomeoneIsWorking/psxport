// test_core_store_no_game.cpp — THE CORE-ALONE STORE CONTRACT.
//
// WHAT THIS GATES (the defect it was written RED against, 2026-08-12): `Core::mem_w32` on a Core that
// is not owned by a `Game` SEGFAULTED. The store path runs `pkt_track` -> `OtAttr::trackStore` ->
// `trackStoreSlow`, whose first statement dereferenced `c->game` for a frame stamp. `Core::game`
// defaults to nullptr and is only set by `Game`'s constructor, and `g_producer_census_armed` is
// DEFAULT TRUE — so the crash needed no debug channel and no unusual configuration: every embedder
// that built a bare `Core` and stored one word died. `tools/smoke/psxport_smoke.cpp` was the one that
// did, and it had been exiting 139 with zero output.
//
// PRECISION, because an earlier draft of this comment overstated it: `tests/test_overlay_reloc.cpp`
// also builds a bare Core, but it performs NO memory write at all (`grep -cE 'mem_w' ` -> 0), so it
// never reached this path and was passing throughout. It was a latent victim, not an actual one — which
// is exactly why a bare-Core STORE needed its own test rather than being assumed covered by the
// existing bare-Core tests.
//
// THE CONTRACT, asserted below rather than described in a comment (core.h documents it too):
//   1. A default-constructed `Core` is a COMPLETE guest-memory device on its own: mem_w8/16/32 and
//      mem_r8/16/32 over main RAM (2 MB, mirrored across KUSEG/KSEG0/KSEG1) and scratchpad
//      (0x1F800000..0x1F8003FF) work with `game == nullptr`.
//   2. HARDWARE I/O IS NOT part of that contract and is deliberately NOT tested here: `Core::io_read`/
//      `io_write` model the console's peripherals, which live on `Game` (game->hle, game->cdc, …), so
//      an I/O-range access on a bare Core is out of contract by design. The line is main RAM +
//      scratchpad, and this test asserts exactly that line and nothing wider.
//
// WHY IT ASSERTS THE ATTRIBUTION RECORD AND NOT MERELY "IT DID NOT CRASH". `if (!c->game) return;` at
// the top of trackStoreSlow would stop the segfault while silently switching the framework's store
// attribution off for every Core-alone embedder — a diagnostic that reports nothing looks identical to
// one that had nothing to report. So test_store_is_still_attributed registers an OtAttr watch region,
// stores into it, and requires the last-writer record to come back. That check FAILS on the early-out
// and PASSES only if trackStoreSlow really ran without a Game. It is the positive control that makes
// the other case's silence detectable.
#include "testutil.h"

#include "core.h"
#include "game_iface.h"
#include "ot_attr.h"
#include "render_substrate.h"

#include <memory>

// A Core alone, exactly as an embedder gets one: the seam installed with a stub config/hooks (Core's
// ctor reads psxport_game_config()), no Game anywhere.
static std::unique_ptr<Core> bare_core(void) {
  static const GameConfig cfg{};
  static const GameHooks hooks{};
  psxport_install_game(&cfg, &hooks);
  return std::make_unique<Core>();
}

// (1) main RAM round-trips at all three widths, through the KSEG0 mirror the substrate uses.
static void test_main_ram_roundtrip_without_game(void) {
  auto c = bare_core();
  CHECK(c->game == nullptr); // the precondition under test — not an incidental detail

  c->mem_w32(0x80010000u, 0xDEADBEEFu);
  CHECK_EQ(c->mem_r32(0x80010000u), 0xDEADBEEFu);
  c->mem_w16(0x80010010u, 0xBEEFu);
  CHECK_EQ(c->mem_r16(0x80010010u), 0xBEEFu);
  c->mem_w8(0x80010020u, 0x5Au);
  CHECK_EQ(c->mem_r8(0x80010020u), 0x5Au);

  // The same physical word through KUSEG and KSEG1 — proves it is the one 2 MB buffer, so a "pass"
  // cannot come from a store landing somewhere harmless.
  CHECK_EQ(c->mem_r32(0x00010000u), 0xDEADBEEFu);
  CHECK_EQ(c->mem_r32(0xA0010000u), 0xDEADBEEFu);
}

// (2) scratchpad round-trips too — it is a different host buffer AND a different address form in
// trackStoreSlow's `phys` normalization, so it exercises a distinct path from main RAM.
static void test_scratchpad_roundtrip_without_game(void) {
  auto c = bare_core();
  CHECK(c->game == nullptr);
  c->mem_w32(0x1F800100u, 0xC0FFEE01u);
  CHECK_EQ(c->mem_r32(0x1F800100u), 0xC0FFEE01u);
  c->mem_w32(0x9F800104u, 0xC0FFEE02u); // KSEG1 form of scratchpad+4
  CHECK_EQ(c->mem_r32(0x1F800104u), 0xC0FFEE02u);
}

// (3) THE POSITIVE CONTROL. The store must still be ATTRIBUTED with no Game — i.e. trackStoreSlow
// executed rather than bailing out. Fails on the `if (!c->game) return;` bandaid.
static void test_store_is_still_attributed(void) {
  auto c = bare_core();
  CHECK(c->game == nullptr);
  OtAttr &oa = c->rsub.otAttr;

  const uint32_t base = 0x80020000u;
  CHECK_EQ(oa.watchRegister(base, 16), 0); // slot 0 of WATCH_SLOTS
  CHECK_EQ(oa.watchSlotCount(), 1);

  // Nothing has been written yet: the pooled record for a fresh region reads as "never written"
  // (frame == 0xFFFFFFFF). Asserted so the post-store check below cannot be satisfied by a
  // pre-existing value — this is the denominator for the positive result.
  OtAttr::WordRec before{};
  CHECK(oa.watchLookup(base, &before));
  CHECK_EQ(before.frame, 0xFFFFFFFFu);

  c->mem_w32(base, 0x12345678u);

  OtAttr::WordRec after{};
  uint32_t wordAddr = 0;
  CHECK(oa.watchLookup(base, &after, &wordAddr));
  CHECK_EQ(wordAddr, base & 0x1FFFFFFFu);
  // The frame stamp CHANGED from "never written" — the only way that happens is trackStoreSlow having
  // run to completion on a Core with no Game. (Its value is whatever frame clock OtAttr was last told
  // about, which for a Core no frame loop drives is the initial one; the assertion is on the write
  // having been recorded, not on a particular frame number.)
  CHECK(after.frame != 0xFFFFFFFFu);

  // An address OUTSIDE every watched region must still report "not watched" rather than a stale hit —
  // the negative case of the same instrument, so a lookup that always returns true is caught here.
  CHECK(!oa.watchLookup(0x80030000u, nullptr));

  // NEGATIVE: this Core used the per-frame tables but never declared a frame.
  CHECK(oa.preFrameStampCount() > 0);
  CHECK(!oa.frameContractSatisfied());

  // POSITIVE: exactly the transition a real game makes after pre-loop boot stores. Once its owned
  // loop begins, the same history is healthy and must not be diagnosed as "no frame loop".
  oa.beginLogicFrame(1);
  CHECK(oa.frameContractSatisfied());
}

int main(void) {
  RUN(main_ram_roundtrip_without_game);
  RUN(scratchpad_roundtrip_without_game);
  RUN(store_is_still_attributed);
  return pt_summary();
}

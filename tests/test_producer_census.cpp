// test_producer_census.cpp — the GRAPHICS PRODUCER CENSUS's table and, above all, its DENOMINATORS.
//
// What the census is for: docs/plans/graphics-producer-db.md. USER 2026-08-11 — "I need a DB for each
// game to keep track of native graphics producers, framework should do this automatically, when the
// game renders an effect, if it's not in the DB then a DB entry gets created", populated by ordinary
// play ("should also be populated when I'm playing"), comparing GTE/OT against native producers.
//
// WHY THE DENOMINATORS ARE MOST OF THIS FILE. The census's output is a per-producer coverage report,
// and a coverage report that can print "0 unported producers" is worthless unless every zero is
// distinguishable from "I never looked". This project has paid for that distinction repeatedly (a
// catalog that returned "(no matches)" from a directory that did not exist; a diagnostic that showed 4
// of 78,278 events and "proved" an array was always empty). So the census counts, and this asserts, the
// four ways it can be blind:
//
//   gp0Anon        — prims whose GP0 words carried no source address, so they are PERMANENTLY
//                    unattributable (a direct register write, an FMV/block upload). Not "drawn by
//                    nobody".
//   spanMiss       — prims whose packet address matched no attribution span: the span feed was off or
//                    overflowed. Not "no producer".
//   unscopedNative — native pushes that arrived outside any ProducerScope. Real work by an
//                    undeclared producer, counted rather than silently attributed to whoever was last.
//   reached        — per row: this run PLAYED these frames. A producer absent from a run is
//                    not-reached, never absent.
//
// A negative reported without those four is a lie, and the test that would let it ship is a test that
// only ever checks the positive path. Hermetic: the census is a pure table with no Core, no GPU, no
// disc (that is also why it is a separate class from the OtAttr tables it will be fed from).
#include "testutil.h"
#include "producer_census.h"

static void test_first_sight_creates_a_row(void) {
  ProducerCensus c;
  CHECK_EQ(c.rowCount(), 0);
  c.noteGuest(0x8002BC9Cu, /*prims=*/3, /*frame=*/10);
  CHECK_EQ(c.rowCount(), 1);
  const ProducerCensus::Row* r = c.find(ProducerKey::guest(0x8002BC9Cu));
  CHECK(r != nullptr);
  CHECK_EQ(r->primsGuest, 3);
  CHECK_EQ(r->primsNative, 0);
  CHECK_EQ(r->firstFrame, 10u);
  CHECK_EQ(r->lastFrame, 10u);
  // Same producer again: the row ACCUMULATES, it does not duplicate. "If it's not in the DB an entry
  // gets created" has to mean exactly once per identity, or a play session produces thousands of rows.
  c.noteGuest(0x8002BC9Cu, 5, 11);
  CHECK_EQ(c.rowCount(), 1);
  CHECK_EQ(c.find(ProducerKey::guest(0x8002BC9Cu))->primsGuest, 8);
  CHECK_EQ(c.find(ProducerKey::guest(0x8002BC9Cu))->firstFrame, 10u);   // first sighting is not overwritten
  CHECK_EQ(c.find(ProducerKey::guest(0x8002BC9Cu))->lastFrame, 11u);
}

// The comparison the whole DB exists for: the same identity seen on BOTH legs.
static void test_guest_and_native_share_one_row(void) {
  ProducerCensus c;
  c.noteGuest(0x8007CD38u, 4, 1);
  c.noteNative(ProducerKey::guest(0x8007CD38u), 4, 1);   // a native producer declaring the fn it ports
  CHECK_EQ(c.rowCount(), 1);
  const ProducerCensus::Row* r = c.find(ProducerKey::guest(0x8007CD38u));
  CHECK_EQ(r->primsGuest, 4);
  CHECK_EQ(r->primsNative, 4);
  // A PC-only producer (an enhancement with no guest counterpart) is a legal row that never compares.
  c.noteNative(ProducerKey::native(7), 2, 1);
  CHECK_EQ(c.rowCount(), 2);
  CHECK_EQ(c.find(ProducerKey::native(7))->primsGuest, 0);
  CHECK(c.find(ProducerKey::native(7))->key.isNativeOnly());
  // …and it must not collide with guest fn 7, which is a different identity in the same integer space.
  c.noteGuest(7u, 1, 1);
  CHECK_EQ(c.rowCount(), 3);
}

// ---- the four blindnesses ------------------------------------------------------------------------

static void test_unattributable_prims_are_counted_not_dropped(void) {
  ProducerCensus c;
  c.noteUnattributable(ProducerCensus::WHY_GP0_ANON, 12);
  c.noteUnattributable(ProducerCensus::WHY_SPAN_MISS, 5);
  CHECK_EQ(c.rowCount(), 0);          // no row invented for them…
  CHECK_EQ(c.gp0Anon(), 12);          // …and they are NOT lost
  CHECK_EQ(c.spanMiss(), 5);
  // The report's honesty test: attributed vs total. 0 attributed out of 17 seen is a very different
  // statement from 0 out of 0, and the census must be able to make it.
  CHECK_EQ(c.primsAttributed(), 0);
  CHECK_EQ(c.primsSeen(), 17);
}

static void test_unscoped_native_pushes_are_counted(void) {
  ProducerCensus c;
  c.noteNative(ProducerKey::none(), 9, 3);   // pushes with no ProducerScope open
  CHECK_EQ(c.rowCount(), 0);                 // NOT attributed to a made-up row
  CHECK_EQ(c.unscopedNative(), 9);
  CHECK_EQ(c.primsSeen(), 9);
  CHECK_EQ(c.primsAttributed(), 0);
}

// A census that has never been fed must be able to SAY SO, distinctly from one that was fed and found
// nothing. This is the "(none)" vs "I never looked" case, as an assertion.
static void test_empty_is_distinguishable_from_clean(void) {
  ProducerCensus fed, unfed;
  CHECK(!unfed.wasFed());
  CHECK_EQ(unfed.primsSeen(), 0);
  fed.noteGuest(0x80010000u, 1, 0);
  CHECK(fed.wasFed());
  CHECK_EQ(fed.primsSeen(), 1);
}

// Capacity is finite (this lives on a Core, in a shipping run), so overflow must be REPORTED rather
// than silently dropping producers — the report says "N producers did not fit" instead of a short list
// that reads as complete.
static void test_overflow_is_reported(void) {
  ProducerCensus c;
  const int cap = ProducerCensus::CAP;
  for (int i = 0; i < cap; i++) c.noteGuest(0x80000000u + (uint32_t)(i * 4), 1, 0);
  CHECK_EQ(c.rowCount(), cap);
  CHECK_EQ(c.overflow(), 0);
  c.noteGuest(0xBADF00D0u, 1, 0);                 // one too many
  CHECK_EQ(c.rowCount(), cap);
  CHECK_EQ(c.overflow(), 1);
  CHECK_EQ(c.primsSeen(), cap + 1);               // still SEEN — the prim happened
  CHECK_EQ(c.primsAttributed(), cap + 1);         // and it WAS attributable; only the row did not fit
}

// Per-Core (SBS runs two): two censuses never share state.
static void test_two_censuses_are_independent(void) {
  ProducerCensus a, b;
  a.noteGuest(0x80001000u, 1, 0);
  CHECK_EQ(a.rowCount(), 1);
  CHECK_EQ(b.rowCount(), 0);
  CHECK(!b.wasFed());
}

int main(void) {
  RUN(first_sight_creates_a_row);
  RUN(guest_and_native_share_one_row);
  RUN(unattributable_prims_are_counted_not_dropped);
  RUN(unscoped_native_pushes_are_counted);
  RUN(empty_is_distinguishable_from_clean);
  RUN(overflow_is_reported);
  RUN(two_censuses_are_independent);
  return pt_summary();
}

// test_producer_scope.cpp — stage 3 of the graphics-producer DB (docs/plans/graphics-producer-db.md):
// the NATIVE-LEG feed. Does a native producer's emissions get attributed to that producer, and — the
// half that matters — are emissions with NO producer declared counted as a REAL NUMBER rather than
// silently dropped?
//
// WHY THE UNSCOPED COUNT IS THE POINT OF THIS FILE. The DB's purpose is to compare, per effect, what
// the guest submits against what a native producer draws. A native push that arrives with no
// ProducerScope open is real drawing work by an UNDECLARED producer: exactly the row the DB exists to
// surface. If those pushes were dropped, the census would report "every native prim is attributed" over
// a picture half of which came from nowhere — and that reads as completeness. So `unscopedNative()` is
// asserted here as a counted quantity, in both directions (zero when everything is declared, non-zero
// and exact when it is not).
//
// Hermetic: ProducerScope + ProducerCensus are host-side value types. No Core, no RenderQueue, no GPU,
// no disc — the scope's contract is "which producer is current", and that is testable on its own.
#include "testutil.h"
#include "producer_census.h"
#include "producer_scope.h"
#include <string.h>

// A scope names a producer for the span of a native draw, and RESTORES the previous one rather than
// clearing — nested producers are real (a controller producer calling a shared writer producer), and
// clearing would silently reattribute the outer producer's later prims to nobody.
static void test_scope_sets_and_restores(void) {
  ProducerCensus cx;
  ProducerScopeState st;
  CHECK(!st.active());
  CHECK_EQ(st.currentAddr(), 0u);
  {
    ProducerScope outer(&st, 0x8002BC9Cu, "radialPlumeRender");
    CHECK(st.active());
    CHECK_EQ(st.currentAddr(), 0x8002BC9Cu);
    CHECK(strcmp(st.currentName(), "radialPlumeRender") == 0);
    {
      ProducerScope inner(&st, 0x80027768u, "meshQuadRecordsEmit");
      CHECK_EQ(st.currentAddr(), 0x80027768u);
    }
    // RESTORED, not cleared — this is the assertion ObjScope's own comment says it exists for.
    CHECK_EQ(st.currentAddr(), 0x8002BC9Cu);
    CHECK(strcmp(st.currentName(), "radialPlumeRender") == 0);
  }
  CHECK(!st.active());
  CHECK_EQ(st.currentAddr(), 0u);
}

// Pushes inside a scope are attributed to that producer's row on the NATIVE leg.
static void test_scoped_pushes_are_attributed(void) {
  ProducerCensus cx;
  ProducerScopeState st;
  {
    ProducerScope s(&st, 0x8002BC9Cu, "radialPlumeRender");
    cx.noteNative(st.currentKey(), 4, /*frame=*/10);
    cx.noteNative(st.currentKey(), 2, /*frame=*/10);
  }
  CHECK(cx.wasFed());
  CHECK_EQ(cx.unscopedNative(), 0u);            // everything was declared
  CHECK_EQ(cx.primsSeen(), 6u);
  CHECK_EQ(cx.primsAttributed(), 6u);
  const ProducerCensus::Row* r = cx.find(ProducerKey::guest(0x8002BC9Cu));
  CHECK(r != nullptr);
  CHECK_EQ(r->primsNative, 6u);
  CHECK_EQ(r->primsGuest, 0u);                  // the native leg must not invent guest-leg counts
  CHECK_EQ(r->firstFrame, 10u);
}

// THE NEGATIVE, and the reason this file exists: a push with no scope open is COUNTED, not dropped.
static void test_unscoped_pushes_are_counted_not_dropped(void) {
  ProducerCensus cx;
  ProducerScopeState st;
  cx.noteNative(st.currentKey(), 3, /*frame=*/7);      // no scope open
  CHECK_EQ(cx.unscopedNative(), 3u);
  CHECK_EQ(cx.primsSeen(), 3u);                          // it IS work that happened…
  CHECK_EQ(cx.primsAttributed(), 0u);                    // …and it is NOT attributed to any producer
  CHECK_EQ(cx.rowCount(), 0);                           // and it invents no row
  // Mixed: declared and undeclared work in the same run must not contaminate each other's totals.
  {
    ProducerScope s(&st, 0x8003B704u, "beamQuadRender");
    cx.noteNative(st.currentKey(), 5, 8);
  }
  cx.noteNative(st.currentKey(), 1, 9);
  CHECK_EQ(cx.unscopedNative(), 4u);                     // 3 + 1
  CHECK_EQ(cx.primsAttributed(), 5u);
  CHECK_EQ(cx.primsSeen(), 9u);                          // 3 + 5 + 1 — the DENOMINATOR
  CHECK_EQ(cx.rowCount(), 1);
  // seen == attributed + unscoped, exactly. If this drifts, some prim went uncounted somewhere and the
  // census's own totals would no longer prove they cover the whole picture.
  CHECK_EQ(cx.primsSeen(), cx.primsAttributed() + cx.unscopedNative());
}

// A census that was never fed says so, distinctly from one fed with zero prims — "no native producer
// ran" and "the feed is not wired" print identically otherwise, and only one of them is a bug.
static void test_unfed_is_distinguishable_from_zero(void) {
  ProducerCensus cx;
  CHECK(!cx.wasFed());
  ProducerCensus fed;
  ProducerScopeState st;
  fed.noteNative(st.currentKey(), 0, 1);              // fed, but the producer drew nothing
  CHECK(fed.wasFed());
  CHECK_EQ(fed.primsSeen(), 0u);
  CHECK_EQ(fed.unscopedNative(), 0u);
}

// The interned producer table must not grow a duplicate row per frame — the same producer across many
// frames is ONE row whose counters accumulate and whose first/last frame widen.
static void test_same_producer_across_frames_is_one_row(void) {
  ProducerCensus cx;
  ProducerScopeState st;
  for (uint32_t f = 100; f < 110; f++) {
    ProducerScope s(&st, 0x800288ACu, "impactPlumeRender");
    cx.noteNative(st.currentKey(), 2, f);
  }
  CHECK_EQ(cx.rowCount(), 1);
  const ProducerCensus::Row* r = cx.find(ProducerKey::guest(0x800288ACu));
  CHECK(r != nullptr);
  CHECK_EQ(r->primsNative, 20u);
  CHECK_EQ(r->firstFrame, 100u);
  CHECK_EQ(r->lastFrame, 109u);
}

int main(void) {
  RUN(scope_sets_and_restores);
  RUN(scoped_pushes_are_attributed);
  RUN(unscoped_pushes_are_counted_not_dropped);
  RUN(unfed_is_distinguishable_from_zero);
  RUN(same_producer_across_frames_is_one_row);
  return pt_summary();
}

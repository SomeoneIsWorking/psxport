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
#include "producer_census.h"
#include "producer_scope.h"
#include "testutil.h"
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
  CHECK_EQ(cx.unscopedNative(), 0u); // everything was declared
  CHECK_EQ(cx.primsSeen(), 6u);
  CHECK_EQ(cx.primsAttributed(), 6u);
  const ProducerCensus::Row *r = cx.find(ProducerKey::guest(0x8002BC9Cu));
  CHECK(r != nullptr);
  CHECK_EQ(r->primsNative, 6u);
  CHECK_EQ(r->primsGuest, 0u); // the native leg must not invent guest-leg counts
  CHECK_EQ(r->firstFrame, 10u);
}

// THE NEGATIVE, and the reason this file exists: a push with no scope open is COUNTED, not dropped.
static void test_unscoped_pushes_are_counted_not_dropped(void) {
  ProducerCensus cx;
  ProducerScopeState st;
  cx.noteNative(st.currentKey(), 3, /*frame=*/7); // no scope open
  CHECK_EQ(cx.unscopedNative(), 3u);
  CHECK_EQ(cx.primsSeen(), 3u);       // it IS work that happened…
  CHECK_EQ(cx.primsAttributed(), 0u); // …and it is NOT attributed to any producer
  CHECK_EQ(cx.rowCount(), 0);         // and it invents no row
  // Mixed: declared and undeclared work in the same run must not contaminate each other's totals.
  {
    ProducerScope s(&st, 0x8003B704u, "beamQuadRender");
    cx.noteNative(st.currentKey(), 5, 8);
  }
  cx.noteNative(st.currentKey(), 1, 9);
  CHECK_EQ(cx.unscopedNative(), 4u); // 3 + 1
  CHECK_EQ(cx.primsAttributed(), 5u);
  CHECK_EQ(cx.primsSeen(), 9u); // 3 + 5 + 1 — the DENOMINATOR
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
  fed.noteNative(st.currentKey(), 0, 1); // fed, but the producer drew nothing
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
  const ProducerCensus::Row *r = cx.find(ProducerKey::guest(0x800288ACu));
  CHECK(r != nullptr);
  CHECK_EQ(r->primsNative, 20u);
  CHECK_EQ(r->firstFrame, 100u);
  CHECK_EQ(r->lastFrame, 109u);
}

// ---- PC-ONLY producers: an enhancement with no guest counterpart must be able to declare itself ----
//
// WHY THESE CASES EXIST. Before `pc_producer()` there was no way to open a scope that was not keyed on a
// guest address, so a PC-only producer had exactly two options and both corrupted the DB: borrow a guest
// address (the widescreen pillarbox quad then read 2-vs-1 in a producer that is actually faithful — see
// game/render/render_options.cpp, which deliberately left the quad OUTSIDE its scope for that reason), or
// stay undeclared and inflate unscopedNative(), the number that is supposed to mean "a guest producer
// nobody has written down yet". Both are silent. So the assertions below are the two things the fix must
// make true: a PC-only row EXISTS, and it does not touch the guest row.

// The iid space is derived, deterministic, never zero, and NOT the guest space.
static void test_pc_producer_iids_are_stable_and_distinct(void) {
  static_assert(producer_iid("pc/margin-render") != 0u, "an iid of 0 would mint a row keyed zero");
  static_assert(producer_iid("pc/margin-render") == producer_iid("pc/margin-render"),
                "the same stable id must intern to the same row on every run");
  static_assert(producer_iid("pc/margin-render") != producer_iid("pc/pillarbox-fill"),
                "two different producers must not share a row");
  CHECK(producer_iid("") != 0u); // the empty id is still not a zero key
  CHECK_EQ(pc_producer("pc/margin-render").iid, producer_iid("pc/margin-render"));
  CHECK(strcmp(pc_producer("pc/margin-render").name, "pc/margin-render") == 0);
  // Same NUMBER in the two spaces is two different rows — asserted on the key, not assumed.
  const uint32_t iid = producer_iid("pc/margin-render");
  CHECK(!(ProducerKey::native(iid) == ProducerKey::guest(iid)));
}

// A PC-only scope opens, and its prims land on a NATIVE-ONLY row: attributed, not unscoped, not dropped.
static void test_pc_only_scope_is_attributed_to_a_native_row(void) {
  ProducerCensus cx;
  ProducerScopeState st;
  static constexpr PcProducer kMargin = pc_producer("pc/margin-render");
  {
    ProducerScope s(&st, kMargin);
    CHECK(st.active());
    CHECK(st.currentKey().valid());
    CHECK(st.currentKey().isNativeOnly()); // <- the whole gap: this was GUEST unconditionally
    // A PC-only scope has NO guest address, and must not answer with its iid as though it had one.
    CHECK_EQ(st.currentAddr(), 0u);
    CHECK(strcmp(st.currentName(), "pc/margin-render") == 0);
    cx.noteNative(st.currentKey(), 24, /*frame=*/5, st.currentName());
  }
  CHECK(!st.active());
  CHECK_EQ(cx.unscopedNative(), 0u); // (a) not silently dropped into "undeclared"
  CHECK_EQ(cx.primsSeen(), 24u);
  CHECK_EQ(cx.primsAttributed(), 24u);
  CHECK_EQ(cx.rowCount(), 1);
  const ProducerCensus::Row *pc = cx.find(ProducerKey::native(kMargin.iid));
  CHECK(pc != nullptr);
  CHECK_EQ(pc->primsNative, 24u);
  CHECK_EQ(pc->primsGuest, 0u);
  CHECK(strcmp(pc->name, "pc/margin-render") == 0); // the row can say WHICH code it is
  // The two id spaces stay separate: the same number as a guest address is a different row.
  CHECK(cx.find(ProducerKey::guest(kMargin.iid)) == nullptr);
  // (c) the census invariant, restated on this path.
  CHECK_EQ(cx.primsSeen(), cx.primsAttributed() + cx.unscopedNative());
}

// THE CONSEQUENCE THAT WAS ALREADY BEING PAID: a PC enhancement drawn next to a faithful guest producer
// must not add a prim to that producer's row. Guest row stays 1-vs-1; the enhancement gets its own row.
static void test_pc_only_prims_do_not_land_on_the_guest_row(void) {
  ProducerCensus cx;
  ProducerScopeState st;
  static constexpr PcProducer kPillarbox = pc_producer("pc/pillarbox-fill");
  {
    ProducerScope s(&st, kPillarbox);
    cx.noteNative(st.currentKey(), 1, 3, st.currentName());
  }
  {
    ProducerScope s(&st, 0x8007FC24u, "optionsBackdrop");
    cx.noteNative(st.currentKey(), 1, 3, st.currentName());
  }
  cx.noteGuest(0x8007FC24u, 1, 3); // what the guest leg saw: ONE prim
  const ProducerCensus::Row *g = cx.find(ProducerKey::guest(0x8007FC24u));
  CHECK(g != nullptr);
  CHECK_EQ(g->primsNative, 1u); // 1-vs-1, not the fabricated 2-vs-1
  CHECK_EQ(g->primsGuest, 1u);
  const ProducerCensus::Row *p = cx.find(ProducerKey::native(kPillarbox.iid));
  CHECK(p != nullptr);
  CHECK_EQ(p->primsNative, 1u);
  CHECK_EQ(p->primsGuest, 0u); // a PC-only row can never have a guest leg
  CHECK_EQ(cx.rowCount(), 2);
  CHECK_EQ(cx.unscopedNative(), 0u);
  CHECK_EQ(cx.primsSeen(), cx.primsAttributed());
}

// Nesting works in both directions, and RESTORES — a PC-only overlay drawn inside a guest producer must
// not leave the guest producer's later prims on the PC row (or unattributed).
static void test_pc_only_scope_nests_and_restores(void) {
  ProducerCensus cx;
  ProducerScopeState st;
  static constexpr PcProducer kMargin = pc_producer("pc/margin-render");
  {
    ProducerScope outer(&st, 0x8003CCA4u, "perObjRenderDispatch");
    cx.noteNative(st.currentKey(), 2, 9, st.currentName());
    {
      ProducerScope inner(&st, kMargin);
      CHECK(st.currentKey().isNativeOnly());
      cx.noteNative(st.currentKey(), 5, 9, st.currentName());
    }
    CHECK_EQ(st.currentAddr(), 0x8003CCA4u); // restored, and it is a GUEST scope again
    CHECK(st.currentKey() == ProducerKey::guest(0x8003CCA4u));
    cx.noteNative(st.currentKey(), 3, 9, st.currentName());
  }
  CHECK_EQ(cx.find(ProducerKey::guest(0x8003CCA4u))->primsNative, 5u); // 2 + 3
  CHECK_EQ(cx.find(ProducerKey::native(kMargin.iid))->primsNative, 5u);
  CHECK_EQ(cx.unscopedNative(), 0u);
}

// An iid COLLISION (two different producers hashing to one row) is DETECTED and counted, not merged —
// and the check is run against BOTH classes: it must NOT fire for the same producer seen twice, and it
// must NOT fire for two natives sharing ONE GUEST row, which is by design (0x8007FCC8 has two).
static void test_iid_collision_is_counted_and_only_for_native_rows(void) {
  ProducerCensus cx;
  const uint32_t iid = producer_iid("pc/margin-render");
  cx.noteNative(ProducerKey::native(iid), 1, 1, "pc/margin-render");
  cx.noteNative(ProducerKey::native(iid), 1, 2, "pc/margin-render"); // same producer again
  CHECK_EQ(cx.iidCollisions(), 0);
  cx.noteNative(ProducerKey::native(iid), 1, 3, "pc/something-else"); // a DIFFERENT producer, one iid
  CHECK_EQ(cx.iidCollisions(), 1);
  CHECK_EQ(cx.rowCount(), 1); // it still counts the prims rather than dropping
  CHECK_EQ(cx.primsSeen(), 3u);
  CHECK_EQ(cx.primsAttributed(), 3u);

  // The negative class: one GUEST address reimplemented by two different natives is NORMAL.
  ProducerCensus guestCx;
  guestCx.noteNative(ProducerKey::guest(0x8007FCC8u), 1, 1, "optionsSolidBox");
  guestCx.noteNative(ProducerKey::guest(0x8007FCC8u), 1, 1, "pushDialogBackdrop");
  CHECK_EQ(guestCx.iidCollisions(), 0);
  CHECK_EQ(guestCx.rowCount(), 1);
}

int main(void) {
  RUN(scope_sets_and_restores);
  RUN(scoped_pushes_are_attributed);
  RUN(unscoped_pushes_are_counted_not_dropped);
  RUN(unfed_is_distinguishable_from_zero);
  RUN(same_producer_across_frames_is_one_row);
  RUN(pc_producer_iids_are_stable_and_distinct);
  RUN(pc_only_scope_is_attributed_to_a_native_row);
  RUN(pc_only_prims_do_not_land_on_the_guest_row);
  RUN(pc_only_scope_nests_and_restores);
  RUN(iid_collision_is_counted_and_only_for_native_rows);
  return pt_summary();
}

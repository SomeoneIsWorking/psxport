// test_producer_census_ownership.cpp — the graphics-producer DB's OWNERSHIP fields, and specifically
// that they can show more than one answer.
//
// WHY THIS FILE EXISTS. `producers.py` consumed two per-row fields, `has_native` and `native_reached`,
// and documented them as "DERIVED from the override table by the runtime". Nothing in the runtime ever
// emitted them. So the report line "has a native producer (derived from the override table): N/M" could
// only ever print 0/M, no matter what the run did — while N native producers were demonstrably drawing
// over a million prims. Two downstream consumers were dead in the same way: `--todo`'s rule that ranks
// "owned but never ran" above new work, and `check`'s lint on that combination.
//
// That is the failure mode this project keeps paying for: a diagnostic whose negative is the only branch
// it can reach reads exactly like a measurement. So the assertions below drive the emitter with THREE
// distinct ownership states and require three DIFFERENT outputs. A change that hardwires any one of them
// fails here rather than shipping as a metric.
//
// THE THIRD STATE IS THE POINT. "not override-installed" and "nobody asked" are different facts, and
// writing `false` for both is what let the original defect look like data. With no query injected the
// row must say `owned_query: unavailable` and must NOT claim `has_native: false`.
//
// Hermetic: ProducerCensus is a host-side value type and the ownership query is INJECTED as a function
// pointer, so no override registry, no Core, and no game are linked here — which is also the reason the
// injection exists rather than a direct #include of override_registry.h.
#include "producer_census.h"
#include "producer_scope.h"
#include "testutil.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The three ownership states, as a stub registry. Deliberately not a real one: the contract under test
// is what the census WRITES for each answer, not what the registry decides.
static const uint32_t kOwnedAndRan = 0x8002BC9Cu;   // installed, native fired
static const uint32_t kOwnedNeverRan = 0x800288ACu; // installed, native never fired  <- the lie-catcher
static const uint32_t kNotInstalled = 0x80146478u;  // a display-pass producer; guest function remains active

static bool stub_query(uint32_t addr, uint64_t *nativeHits, uint64_t *oracleHits) {
  if (addr == kOwnedAndRan) {
    if (nativeHits) {
      *nativeHits = 42;
    }
    if (oracleHits) {
      *oracleHits = 7;
    }
    return true;
  }
  if (addr == kOwnedNeverRan) {
    if (nativeHits) {
      *nativeHits = 0;
    }
    if (oracleHits) {
      *oracleHits = 3;
    }
    return true;
  }
  return false; // kNotInstalled and anything else
}

// Read a whole file back. Returns the byte count, or -1 — a test that cannot read its own output must
// fail loudly rather than assert over an empty buffer and pass.
static long slurp(const char *path, char *buf, size_t cap) {
  FILE *f = fopen(path, "rb");
  if (!f) {
    return -1;
  }
  const size_t n = fread(buf, 1, cap - 1, f);
  fclose(f);
  buf[n] = 0;
  return (long)n;
}

static void feed_three_rows(ProducerCensus &cx, ProducerScopeState &st) {
  {
    ProducerScope s(&st, kOwnedAndRan, "a");
    cx.noteNative(st.currentKey(), 3, 10);
  }
  {
    ProducerScope s(&st, kOwnedNeverRan, "b");
    cx.noteNative(st.currentKey(), 5, 11);
  }
  {
    ProducerScope s(&st, kNotInstalled, "c");
    cx.noteNative(st.currentKey(), 7, 12);
  }
}

// WITH a query: three rows, three DIFFERENT ownership verdicts. This is the discriminator run against
// every class it claims to distinguish, rather than against the one case that happened to be handy.
static void test_ownership_distinguishes_all_three_states(void) {
  const char *path = "test_producer_ownership_with_query.jsonl";
  remove(path);
  ProducerCensus cx;
  ProducerScopeState st;
  feed_three_rows(cx, st);
  cx.writeJsonl(path, "stamp", &stub_query);

  static char buf[8192];
  const long n = slurp(path, buf, sizeof buf);
  CHECK(n > 0); // the file exists and is non-empty
  CHECK_EQ(cx.rowCount(), 3);

  // installed AND ran -> both true, and the hit count travels so "ran" is not just a bool to trust.
  CHECK(strstr(buf, "\"key\":\"0x8002BC9C\"") != NULL);
  CHECK(strstr(buf, "\"has_native\":true,\"native_reached\":true,\"native_hits\":42,\"oracle_hits\":7") != NULL);

  // installed and NEVER ran -> the combination that is a lie in the DB rather than a gap. It must be
  // expressible; if this line ever stops appearing, --todo's ranking silently stops working again.
  // ...and the oracle count comes with it, so "never dispatched" (0/0) is separable from "dispatched
  // to the guest body" (0/n). Without this the row reads as unexercised in both cases.
  CHECK(strstr(buf, "\"has_native\":true,\"native_reached\":false,\"native_hits\":0,\"oracle_hits\":3") != NULL);

  // NOT installed -> false, which is the CORRECT and normal answer for a display-pass producer that
  // draws the picture from game state while the guest function stays active.
  CHECK(strstr(buf, "\"has_native\":false,\"native_reached\":false") != NULL);

  // And with a query present, "unavailable" must not appear anywhere — that is the other state.
  CHECK(strstr(buf, "owned_query") == NULL);
  remove(path);
}

// WITHOUT a query: the fields must be ABSENT and the row must say so. Writing `false` here would be the
// original defect in a new costume — an unasked question rendered as a negative answer.
static void test_no_query_reports_unavailable_not_false(void) {
  const char *path = "test_producer_ownership_no_query.jsonl";
  remove(path);
  ProducerCensus cx;
  ProducerScopeState st;
  feed_three_rows(cx, st);
  cx.writeJsonl(path, "stamp"); // no query injected

  static char buf[8192];
  const long n = slurp(path, buf, sizeof buf);
  CHECK(n > 0);
  CHECK(strstr(buf, "\"owned_query\":\"unavailable\"") != NULL);
  CHECK(strstr(buf, "has_native") == NULL); // no fabricated verdict
  CHECK(strstr(buf, "native_reached") == NULL);
  remove(path);
}

// The totals record must still be written and must still carry the denominators — the ownership fields
// are additive, and a row-format change that dropped the totals would let a reader compute coverage from
// rows alone and be confidently wrong.
static void test_totals_record_survives_the_row_change(void) {
  const char *path = "test_producer_ownership_totals.jsonl";
  remove(path);
  ProducerCensus cx;
  ProducerScopeState st;
  feed_three_rows(cx, st);
  cx.noteNative(ProducerKey::none(), 4, 13); // undeclared work, so the denominators differ
  cx.writeJsonl(path, "stamp", &stub_query);

  static char buf[8192];
  const long n = slurp(path, buf, sizeof buf);
  CHECK(n > 0);
  CHECK(strstr(buf, "\"type\":\"totals\"") != NULL);
  CHECK(strstr(buf, "\"prims_seen\":19") != NULL);       // 3+5+7+4
  CHECK(strstr(buf, "\"prims_attributed\":15") != NULL); // 3+5+7
  CHECK(strstr(buf, "\"unscoped_native\":4") != NULL);
  CHECK_EQ(cx.primsSeen(), cx.primsAttributed() + cx.unscopedNative());
  remove(path);
}

// ---- THE FOURTH OWNERSHIP STATE: a PC-only row has no guest function to own -----------------------
//
// A native-only row is keyed by an INTERNED id, not an address, so asking the override registry whether
// that number is installed is a category error — and answering `has_native:false` stamps "no native
// producer" on the one row that is native by construction. Worse, the interned id is printed as the row's
// key, which is also its FILENAME in the DB: `0x6E3A1C2B` would read as a guest function that does not
// exist. Both are silent failures, so both are asserted here.

// Return a COPY of the single JSONL line containing `needle`, or NULL. Two properties, both learned the
// hard way while writing this file: it is LINE-SCOPED, because `strstr` over the whole buffer happily
// matches a field belonging to a DIFFERENT row and passes; and it is NON-DESTRUCTIVE, because an earlier
// version terminated the line in place and every later whole-buffer check then silently searched only the
// first line — a test that reported a missing field that was in fact present.
static const char *line_with(const char *buf, const char *needle) {
  static char line[2048];
  for (const char *p = buf; p && *p;) {
    const char *nl = strchr(p, '\n');
    const size_t len = nl ? (size_t)(nl - p) : strlen(p);
    if (len < sizeof line) {
      memcpy(line, p, len);
      line[len] = 0;
      if (strstr(line, needle)) {
        return line;
      }
    }
    if (!nl) {
      return NULL;
    }
    p = nl + 1;
  }
  return NULL;
}

static void test_pc_only_row_is_keyed_and_not_asked_about_ownership(void) {
  const char *path = "test_producer_ownership_pc_only.jsonl";
  remove(path);
  ProducerCensus cx;
  ProducerScopeState st;
  static constexpr PcProducer kMargin = pc_producer("pc/margin-render");
  feed_three_rows(cx, st);
  {
    ProducerScope s(&st, kMargin);
    cx.noteNative(st.currentKey(), 11, 14, st.currentName());
  }
  cx.writeJsonl(path, "stamp", &stub_query);

  static char buf[8192];
  const long n = slurp(path, buf, sizeof buf);
  CHECK(n > 0);
  CHECK_EQ(cx.rowCount(), 4);

  // The key names its own space, so it cannot be read as a guest address (and cannot collide with a
  // guest row's DB filename).
  char expectKey[40];
  snprintf(expectKey, sizeof expectKey, "\"key\":\"pc-%08X\"", kMargin.iid);
  const char *row = line_with(buf, expectKey);
  CHECK(row != NULL);
  if (row) {
    CHECK(strstr(row, "\"kind\":\"native-only\"") != NULL);
    CHECK(strstr(row, "\"pc_name\":\"pc/margin-render\"") != NULL); // WHICH code this row is
    CHECK(strstr(row, "\"prims_native\":11") != NULL);
    CHECK(strstr(row, "\"owned_query\":\"pc-only\"") != NULL); // the fourth state
    CHECK(strstr(row, "has_native") == NULL);                  // no fabricated verdict…
    CHECK(strstr(row, "native_reached") == NULL);              // …in either field
  }
  // And the guest rows are untouched by the addition: the query still answers all three states.
  CHECK(strstr(buf, "\"has_native\":true,\"native_reached\":true,\"native_hits\":42,\"oracle_hits\":7") != NULL);
  CHECK(strstr(buf, "\"has_native\":true,\"native_reached\":false,\"native_hits\":0,\"oracle_hits\":3") != NULL);
  CHECK(strstr(buf, "\"has_native\":false,\"native_reached\":false") != NULL);
  remove(path);
}

// The invariant, with a PC-only row in the mix: seen == attributed + unscoped, and the totals record
// still carries every denominator (plus the collision counter, which must read 0 on a clean run — a
// counter that is never printed cannot be trusted to be zero).
static void test_totals_hold_with_a_pc_only_row(void) {
  const char *path = "test_producer_ownership_pc_totals.jsonl";
  remove(path);
  ProducerCensus cx;
  ProducerScopeState st;
  feed_three_rows(cx, st); // 3 + 5 + 7 guest-keyed native prims
  {
    ProducerScope s(&st, pc_producer("pc/pillarbox-fill"));
    cx.noteNative(st.currentKey(), 2, 13, st.currentName());
  }
  cx.noteNative(ProducerKey::none(), 4, 13); // still-undeclared work
  cx.writeJsonl(path, "stamp", &stub_query);

  static char buf[8192];
  CHECK(slurp(path, buf, sizeof buf) > 0);
  CHECK(strstr(buf, "\"prims_seen\":21") != NULL);       // 3+5+7+2+4
  CHECK(strstr(buf, "\"prims_attributed\":17") != NULL); // the PC-only prims ARE attributed
  CHECK(strstr(buf, "\"unscoped_native\":4") != NULL);
  CHECK(strstr(buf, "\"iid_collisions\":0") != NULL);
  CHECK_EQ(cx.primsSeen(), cx.primsAttributed() + cx.unscopedNative());
  CHECK_EQ(cx.iidCollisions(), 0);
  remove(path);
}

// The same defect stated WITHOUT the new constructor, so this case compiles against the OLD header too
// and FAILS AT RUNTIME on it rather than at build time — the native-only key space was always reachable
// through `ProducerKey::native()`; what was missing was a scope that could open it and a writer that
// treats the row as PC-only. Pre-fix this prints `"key":"0x6E3A1C2B"` and `"has_native":false`.
static void test_native_only_key_is_written_as_pc_not_as_an_address(void) {
  const char *path = "test_producer_ownership_native_key.jsonl";
  remove(path);
  ProducerCensus cx;
  const uint32_t iid = 0x6E3A1C2Bu;
  cx.noteNative(ProducerKey::native(iid), 11, 14);
  cx.writeJsonl(path, "stamp", &stub_query);
  static char buf[8192];
  CHECK(slurp(path, buf, sizeof buf) > 0);
  CHECK(strstr(buf, "\"key\":\"pc-6E3A1C2B\"") != NULL); // not "0x6E3A1C2B" — a non-existent guest fn
  CHECK(strstr(buf, "\"kind\":\"native-only\"") != NULL);
  CHECK(strstr(buf, "\"owned_query\":\"pc-only\"") != NULL);
  CHECK(strstr(buf, "has_native") == NULL); // the registry was never asked an address
  remove(path);
}

int main(void) {
  RUN(ownership_distinguishes_all_three_states);
  RUN(no_query_reports_unavailable_not_false);
  RUN(totals_record_survives_the_row_change);
  RUN(pc_only_row_is_keyed_and_not_asked_about_ownership);
  RUN(totals_hold_with_a_pc_only_row);
  RUN(native_only_key_is_written_as_pc_not_as_an_address);
  return pt_summary();
}

// test_producer_provenance.cpp — CLAIM-SET PROVENANCE: who earned each claim, and which build.
//
// THE DEFECT THIS WAS WRITTEN AGAINST (kanban #91, Tomba2Engine). `ProducerCensus::loadClaims` and
// `note()` fill the SAME `mClaims[]` array, so `claimCount()` was a UNION of "loaded from disk" and
// "earned in this run" with nothing able to tell the two apart afterwards. Two consequences, both
// measured on the real corpus before the fix:
//
//   1. `appendClaims` wrote the whole union, so EVERY run re-emitted the file's own contents. The
//      newest block of claims.txt therefore always looked freshly earned (0x800803DC appeared in 7 of
//      106 lines while no run since commit 9c94008 had earned it), and the on-disk file could not
//      answer "what did this run earn" even in principle.
//   2. A claim whose producer KEY HAD MOVED kept reading as live. That is the actual failure mode: a
//      fossil attributing guest prims to a native producer that no longer draws them.
//
// WHY THE REPORT'S TEXT IS ASSERTED HERE, not just the predicates it is computed from. The whole
// deliverable of step 3 is a LOUD line a human reads, and its most important sentence is a NEGATIVE:
// "NOT RE-EARNED IS NOT DEAD". A test that only checked `claimEarnedHere()` would pass while the report
// said the opposite of what it must say — and the measured cost of getting that sentence wrong is
// concrete: on Tomba!2, 119 consecutive native legs failed to earn 0x800803DC purely because the corpus
// was all mode-0 seaside content, while 12 of that game's 22 render modes select exactly that address.
// A report saying "stale" would have had a session prune a claim live in over half the game. So the
// report is driven through a `lucent::set_sink` capture and its text is asserted.
//
// BOTH CLASSES ARE GATED, which is what the card asked for and what a one-sided test would have missed:
// a claim RE-EARNED by the running build, and a claim carrying a FOREIGN build id — the report must name
// the second and must NOT name the first.
#include "testutil.h"
#include "producer_census.h"

#include <lucent/log.h>
#include <stdio.h>
#include <string.h>
#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// ---- log capture -------------------------------------------------------------------------------
// The report is the product, so it is captured rather than eyeballed. Kept as a vector of whole lines
// so an assertion can say "no line mentions X", which is the shape most of the negatives need.
// The LEVEL is captured too, not just the text: the blind-spot sentence must arrive at WARN, because at
// Info it would sit in the same stream as the routine per-run chatter and be read past. That is a
// property of the report worth asserting, not an implementation detail.
static std::vector<std::pair<lucent::Level, std::string>> g_lines;
static void capture_begin(void) {
  g_lines.clear();
  lucent::set_sink([](lucent::Level lv, std::string_view line) {
    g_lines.emplace_back(lv, std::string(line));
  });
}
static void capture_end(void) { lucent::set_sink({}); }

static bool logged(const char* needle) {
  for (const auto& l : g_lines)
    if (l.second.find(needle) != std::string::npos) return true;
  return false;
}
static bool logged_at(lucent::Level lv, const char* needle) {
  for (const auto& l : g_lines)
    if (l.first == lv && l.second.find(needle) != std::string::npos) return true;
  return false;
}

// A scratch claim file, in the CWD — which under ctest is the build tree (gitignored), never /tmp: this
// machine's /tmp is a small tmpfs and filling it breaks every write in the session. A bare name is
// deliberate: `tests/CMakeLists.txt` sets no WORKING_DIRECTORY, so any directory prefix here would be
// resolved against whatever directory ctest happened to be invoked from.
static const char* CLAIMS = "test-producer-provenance-claims.txt";

// If the fixture cannot be written, this suite can prove NOTHING — every later CHECK would compare
// against an unloaded claim set and fail for a reason that has nothing to do with provenance. So it dies
// immediately, naming the cause, instead of emitting a cascade that buries it. (Measured: the first run
// of this file printed 22 failed checks across 11 tests, all of them downstream of one unwritable path.)
static void write_file(const char* path, const char* body) {
  FILE* f = fopen(path, "w");
  if (!f) {
    fprintf(stderr, "test_producer_provenance: CANNOT WRITE the fixture %s in cwd — this suite is "
                    "ABORTING rather than reporting provenance failures it did not measure.\n", path);
    exit(2);
  }
  fputs(body, f);
  fclose(f);
}
static std::string read_file(const char* path) {
  FILE* f = fopen(path, "r");
  if (!f) return "";
  std::string s;
  char buf[512];
  size_t n;
  while ((n = fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
  fclose(f);
  return s;
}
static int count_lines(const std::string& s) {
  int n = 0;
  for (char c : s) if (c == '\n') n++;
  return n;
}

// ---- 1. THE UNION SPLIT (step 1) ---------------------------------------------------------------
// The distinction the old code could not express. Everything else is built on these two predicates.
static void test_loaded_and_earned_are_distinguishable(void) {
  write_file(CLAIMS, "0x80001000 2026-01-01T00:00:00 build-OLD\n"
                     "0x80002000 2026-01-01T00:00:00 build-OLD\n");
  ProducerCensus c;
  CHECK_EQ(c.loadClaims(CLAIMS), 2);
  CHECK_EQ(c.claimCount(), 2);
  CHECK_EQ(c.claimLoadedCount(), 2);
  CHECK_EQ(c.claimEarnedHereCount(), 0);   // loading is NOT earning — the crux of the bug

  // Re-earn ONE of them, in this run, by a native producer pushing on that guest key.
  c.noteNative(ProducerKey::guest(0x80001000u), 4, 1);
  CHECK_EQ(c.claimCount(), 2);             // still 2 addresses: re-earning does not duplicate
  CHECK_EQ(c.claimEarnedHereCount(), 1);
  CHECK_EQ(c.claimLoadedCount(), 2);

  // And the per-claim predicates agree with the counts, addressed by index.
  for (int i = 0; i < c.claimCount(); i++) {
    const bool isFirst = (c.claimAt(i) == 0x80001000u);
    CHECK(c.claimFromDisk(i));
    CHECK_EQ(c.claimEarnedHere(i), isFirst);
  }
}

// A claim first seen in THIS run is earned-here and NOT from disk — the fourth combination, which the
// report's "first earned in this run" column is computed from.
static void test_brand_new_claim_is_not_from_disk(void) {
  write_file(CLAIMS, "0x80001000 2026-01-01T00:00:00 build-OLD\n");
  ProducerCensus c;
  CHECK_EQ(c.loadClaims(CLAIMS), 1);
  c.noteNative(ProducerKey::guest(0x8000BEEFu), 2, 1);
  CHECK_EQ(c.claimCount(), 2);
  CHECK_EQ(c.claimLoadedCount(), 1);
  CHECK_EQ(c.claimEarnedHereCount(), 1);
  for (int i = 0; i < c.claimCount(); i++) {
    if (c.claimAt(i) != 0x8000BEEFu) continue;
    CHECK(c.claimEarnedHere(i));
    CHECK(!c.claimFromDisk(i));
  }
}

// ---- 2. THE UNION-ECHO BUG ITSELF (step 2) -----------------------------------------------------
// THE regression test. Before the fix this wrote 3 lines (the whole union); it must write exactly the
// ONE address this run earned, or the file goes on lying about what each run did.
static void test_append_writes_only_what_this_run_earned(void) {
  write_file(CLAIMS, "0x80001000 2026-01-01T00:00:00 build-OLD\n"
                     "0x80002000 2026-01-01T00:00:00 build-OLD\n"
                     "0x80003000 2026-01-01T00:00:00 build-OLD\n");
  ProducerCensus c;
  c.setBuildId("build-NEW");
  CHECK_EQ(c.loadClaims(CLAIMS), 3);
  c.noteNative(ProducerKey::guest(0x80002000u), 5, 1);

  CHECK_EQ(c.appendClaims(CLAIMS, "2026-06-06T12:00:00"), 1);
  const std::string body = read_file(CLAIMS);
  CHECK_EQ(count_lines(body), 4);                       // the 3 pre-existing + exactly 1 appended
  CHECK(body.find("0x80002000 2026-06-06T12:00:00 build-NEW") != std::string::npos);
  // The two claims this run did NOT earn must not have been re-emitted — re-emitting them is both the
  // union-echo bug AND what would destroy the provenance already recorded on their old lines.
  CHECK_EQ((int)std::count(body.begin(), body.end(), '\n'), 4);
  size_t first = body.find("0x80001000");
  CHECK(first != std::string::npos);
  CHECK(body.find("0x80001000", first + 1) == std::string::npos);   // appears ONCE, not twice
}

// Earning nothing must be SAID, not silently skipped — "this run earned nothing" and "the write failed"
// were the same observation (an early `return 0` with no line) before the fix.
static void test_earning_nothing_is_announced_not_silent(void) {
  write_file(CLAIMS, "0x80001000 2026-01-01T00:00:00 build-OLD\n");
  ProducerCensus c;
  c.setBuildId("build-NEW");
  CHECK_EQ(c.loadClaims(CLAIMS), 1);
  capture_begin();
  CHECK_EQ(c.appendClaims(CLAIMS, "2026-06-06T12:00:00"), 0);
  capture_end();
  CHECK(logged("EARNED no claim"));
  CHECK_EQ(count_lines(read_file(CLAIMS)), 1);          // and it wrote nothing
}

// ---- 3. THE LINE FORMAT, both directions -------------------------------------------------------
// The format must load in a PRE-provenance build and a pre-provenance file must load here, because the
// claim file is append-only and therefore always a mix of both.
static void test_bare_address_line_still_loads_with_no_provenance(void) {
  write_file(CLAIMS, "0x80001000\n0x80002000 2026-01-01T00:00:00 build-OLD\n");
  ProducerCensus c;
  CHECK_EQ(c.loadClaims(CLAIMS), 2);
  for (int i = 0; i < c.claimCount(); i++) {
    if (c.claimAt(i) == 0x80001000u) CHECK_STREQ(c.claimProv(i), "");           // REAL third state
    if (c.claimAt(i) == 0x80002000u) CHECK_STREQ(c.claimProv(i), "build-OLD");
  }
}

// Append-only means chronological, so the NEWEST line for an address wins. Keeping the first (the old
// `dup++; continue;`) would make every claim report the OLDEST build that ever earned it and read as a
// fossil forever after one stale block.
static void test_newest_line_supersedes_older_provenance(void) {
  write_file(CLAIMS, "0x80001000 2026-01-01T00:00:00 build-ANCIENT\n"
                     "0x80001000 2026-05-05T00:00:00 build-RECENT\n");
  ProducerCensus c;
  CHECK_EQ(c.loadClaims(CLAIMS), 1);          // one ADDRESS
  CHECK_EQ(c.claimCount(), 1);
  CHECK_STREQ(c.claimProv(0), "build-RECENT");
}

// ---- 4. UNKNOWN(...) IS NOT AN IDENTITY --------------------------------------------------------
// Two builds that both failed to describe themselves are not the same build. If UNKNOWN compared equal,
// a whole class of fossils would report as re-earned by the running build.
static void test_unknown_build_id_is_never_comparable(void) {
  CHECK(ProducerCensus::buildIdIsReal("abc123"));
  CHECK(ProducerCensus::buildIdIsReal("v1.0-3-gdeadbee-dirty"));
  CHECK(!ProducerCensus::buildIdIsReal(""));
  CHECK(!ProducerCensus::buildIdIsReal(nullptr));
  CHECK(!ProducerCensus::buildIdIsReal("UNKNOWN(no-git-executable)"));
  CHECK(!ProducerCensus::buildIdIsReal("UNKNOWN(git-describe-rc=128)"));
}

// A build id lands in a WHITESPACE-DELIMITED column, so an id containing a space would split into two
// fields and silently corrupt every later parse of that file.
static void test_build_id_whitespace_is_sanitised_at_the_boundary(void) {
  ProducerCensus c;
  c.setBuildId("has a space");
  CHECK_STREQ(c.buildId(), "has_a_space");
  c.setBuildId(nullptr);
  CHECK_STREQ(c.buildId(), "");
}

// ---- 5. THE REPORT, BOTH CLASSES (step 3) ------------------------------------------------------
// The card's actual requirement: one claim re-earned by this build, one carrying a FOREIGN build id, and
// the report must name the second and not the first.
static void test_report_names_the_foreign_build_and_not_the_re_earned(void) {
  write_file(CLAIMS, "0x80001000 2026-01-01T00:00:00 build-NEW\n"     // will be re-earned below
                     "0x80002000 2026-01-01T00:00:00 build-FOREIGN\n");
  ProducerCensus c;
  c.setBuildId("build-NEW");
  CHECK_EQ(c.loadClaims(CLAIMS), 2);
  c.noteNative(ProducerKey::guest(0x80001000u), 3, 1);

  capture_begin();
  c.reportClaimProvenance("unit-test");
  capture_end();

  CHECK(g_lines.size() > 0);                       // it must print even when little is wrong
  CHECK(logged("build-NEW"));                      // the running identity is stated
  CHECK(logged("0x80002000"));                     // the FOREIGN one is NAMED
  CHECK(logged("build-FOREIGN"));
  // ...and the re-earned one must NOT be named as suspect. It is the only other address, so its absence
  // from the report's "first:" callout is the assertion.
  CHECK(!logged("first: 0x80001000"));
  // THE SENTENCE WHOSE ABSENCE WOULD INVERT THE READER'S CONCLUSION.
  CHECK(logged_at(lucent::Level::Warn, "NOT RE-EARNED IS NOT DEAD"));
  CHECK(logged_at(lucent::Level::Warn, "POSSIBLY A FOSSIL"));
}

// With every loaded claim re-earned by this build, there is nothing suspect — and the blind-spot warning
// must NOT fire. A report that always cries fossil is as useless as one that never does.
static void test_report_is_quiet_when_everything_was_re_earned(void) {
  write_file(CLAIMS, "0x80001000 2026-01-01T00:00:00 build-NEW\n");
  ProducerCensus c;
  c.setBuildId("build-NEW");
  CHECK_EQ(c.loadClaims(CLAIMS), 1);
  c.noteNative(ProducerKey::guest(0x80001000u), 3, 1);

  capture_begin();
  c.reportClaimProvenance("unit-test");
  capture_end();

  CHECK(logged("claim set provenance"));           // it still reports, with its denominator
  CHECK(!logged("POSSIBLY A FOSSIL"));
  CHECK(!logged("NOT RE-EARNED IS NOT DEAD"));
}

// NO IDENTITY -> REFUSE TO CLASSIFY. With no build id every comparison would be against "" and every
// claim would read DIFFERENT-BUILD: a report that is 100% false positives, in the tool's normal format.
static void test_report_refuses_to_classify_without_a_build_id(void) {
  write_file(CLAIMS, "0x80001000 2026-01-01T00:00:00 build-OLD\n");
  ProducerCensus c;                                 // setBuildId deliberately NOT called
  CHECK_EQ(c.loadClaims(CLAIMS), 1);

  capture_begin();
  c.reportClaimProvenance("unit-test");
  capture_end();

  CHECK(logged("CANNOT BE COMPUTED"));
  CHECK(logged("UNKNOWN, not 'none'"));            // the distinction that makes it honest
  CHECK(!logged("POSSIBLY A FOSSIL"));             // it must not classify anything
}

// The same refusal when the id is UNKNOWN(...) rather than absent — the generator's real failure output.
static void test_report_refuses_on_an_unknown_shaped_id(void) {
  write_file(CLAIMS, "0x80001000 2026-01-01T00:00:00 build-OLD\n");
  ProducerCensus c;
  c.setBuildId("UNKNOWN(no-git-executable)");
  CHECK_EQ(c.loadClaims(CLAIMS), 1);

  capture_begin();
  c.reportClaimProvenance("unit-test");
  capture_end();

  CHECK(logged("CANNOT BE COMPUTED"));
  CHECK(!logged("POSSIBLY A FOSSIL"));
}

// A pre-provenance file (bare addresses) is its OWN class in the report — neither same-build nor
// foreign — because "written by a build that recorded nothing" is not evidence about which build.
static void test_no_provenance_is_its_own_class(void) {
  write_file(CLAIMS, "0x80001000\n");
  ProducerCensus c;
  c.setBuildId("build-NEW");
  CHECK_EQ(c.loadClaims(CLAIMS), 1);

  capture_begin();
  c.reportClaimProvenance("unit-test");
  capture_end();

  CHECK(logged("carry NO provenance"));
  CHECK(logged("NOT RE-EARNED IS NOT DEAD"));      // it is un-re-earned, so the blind spot still applies
  // but it must NOT be counted as a foreign build: that would invent evidence.
  CHECK(!logged("first: 0x80001000"));
}

int main(void) {
  RUN(loaded_and_earned_are_distinguishable);
  RUN(brand_new_claim_is_not_from_disk);
  RUN(append_writes_only_what_this_run_earned);
  RUN(earning_nothing_is_announced_not_silent);
  RUN(bare_address_line_still_loads_with_no_provenance);
  RUN(newest_line_supersedes_older_provenance);
  RUN(unknown_build_id_is_never_comparable);
  RUN(build_id_whitespace_is_sanitised_at_the_boundary);
  RUN(report_names_the_foreign_build_and_not_the_re_earned);
  RUN(report_is_quiet_when_everything_was_re_earned);
  RUN(report_refuses_to_classify_without_a_build_id);
  RUN(report_refuses_on_an_unknown_shaped_id);
  RUN(no_provenance_is_its_own_class);
  remove(CLAIMS);
  return pt_summary();
}

// test_picture_announce — a wide picture that was REQUESTED and did not happen must say so.
//
// THE BUG THIS PINS (measured twice, in two different repositories):
//     Spyro 1:   "the '14/14 checkpoints byte-identical with the enhancements off versus on'
//                 recorded above was NOT a widescreen measurement ... aspect=3, ASPECT_AUTO,
//                 which resolves to the sink's aspect; an agent run is headless, so both arms
//                 rendered 512 wide"
//     Tomba! 2:  "BOTH arms of that differential rendered at the 4:3 width: it is a valid fps60
//                 result and says nothing about widescreen."
// Both repos corrected the claim in their own docs. The announcement printed the deciding number, so
// a careful reader could catch it; nothing SAID so, twice.
//
// This drives the product's own `classifyWide` — the same function `announceOnChange` calls — so the
// contract under test is the shipping one. The cases that matter are the pair that has already cost
// two repositories a body of evidence: AUTO that resolved narrow, and AUTO that resolved wide. Those
// two must be told apart, or the warning becomes noise nobody reads.

#include "../runtime/psx/mods.h"
#include "../runtime/psx/picture_announce.h"
#include "testutil.h"

#include <string>

namespace {
bool mentions(const psx::picture::WideOutcome outcome, const char *needle) {
  return std::string(psx::picture::wideOutcomeReason(outcome)).find(needle) != std::string::npos;
}
bool silent(const psx::picture::WideOutcome outcome) {
  return psx::picture::wideOutcomeReason(outcome)[0] == '\0';
}
} // namespace

// The pair that has already cost two repositories their widescreen claim: the SAME requested aspect,
// once resolving to the sink's own width and once to a genuinely wide one.
static void test_auto_is_told_apart_from_a_real_wide_run(void) {
  const psx::picture::WideOutcome narrow =
      psx::picture::classifyWide(ASPECT_AUTO, /*enhancements=*/true, /*native=*/320, /*render=*/320);
  const psx::picture::WideOutcome wide =
      psx::picture::classifyWide(ASPECT_AUTO, /*enhancements=*/true, /*native=*/320, /*render=*/428);
  CHECK(narrow != wide);
  CHECK(mentions(narrow, "ASPECT_AUTO"));
  CHECK(silent(wide));
}

// A named wide aspect that produced the native width is NOT the AUTO story and must not be reported as
// one: something else refused it, and blaming AUTO would send a reader to the wrong knob.
static void test_a_named_wide_aspect_that_did_not_widen_is_not_blamed_on_auto(void) {
  const psx::picture::WideOutcome named =
      psx::picture::classifyWide(ASPECT_16_9, /*enhancements=*/true, /*native=*/320, /*render=*/320);
  CHECK_EQ(mentions(named, "ASPECT_AUTO"), false);
  CHECK_EQ(silent(named), false);
}

// 4:3 is not a refusal. A run that never asked for a wide picture must stay quiet, or every ordinary
// run in every title grows a warning nobody will read.
static void test_four_three_three_is_quiet(void) {
  CHECK(silent(psx::picture::classifyWide(ASPECT_4_3, true, 512, 512)));
  // ...including on a PURE core, which is 4:3 by definition and is meant to be.
  CHECK(silent(psx::picture::classifyWide(ASPECT_4_3, false, 512, 512)));
}

// A wide aspect on a PURE core is refused by the render mode, and that is the reason to give: the
// oracle bundle is SUPPOSED to be pure, so a reader must not go hunting for an aspect problem.
static void test_pure_core_is_refused_for_its_own_reason(void) {
  const psx::picture::WideOutcome pure =
      psx::picture::classifyWide(ASPECT_16_9, /*enhancements=*/false, /*native=*/320, /*render=*/320);
  CHECK(mentions(pure, "PURE"));
  CHECK_EQ(mentions(pure, "ASPECT_AUTO"), false);
}

// The exact cases the two repositories recorded, so the regression cannot come back with different
// numbers: Spyro 1 is a 512-native title, Tomba! 2 a 320-native one, and the legs that DID widen are
// 512 -> 684 and 320 -> 428.
static void test_the_measured_case_from_the_repositories(void) {
  CHECK(mentions(psx::picture::classifyWide(ASPECT_AUTO, true, 512, 512), "ASPECT_AUTO"));
  CHECK(mentions(psx::picture::classifyWide(ASPECT_AUTO, true, 320, 320), "ASPECT_AUTO"));
  CHECK(silent(psx::picture::classifyWide(ASPECT_AUTO, true, 512, 684)));
  CHECK(silent(psx::picture::classifyWide(ASPECT_AUTO, true, 320, 428)));
}

int main(void) {
  RUN(auto_is_told_apart_from_a_real_wide_run);
  RUN(a_named_wide_aspect_that_did_not_widen_is_not_blamed_on_auto);
  RUN(four_three_three_is_quiet);
  RUN(pure_core_is_refused_for_its_own_reason);
  RUN(the_measured_case_from_the_repositories);
  return pt_summary();
}

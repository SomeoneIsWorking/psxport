// test_picture_announce — a wide picture that was requested and did not happen says why.
//
// Spyro 1 and Tomba! 2 each published widescreen evidence whose runs asked for ASPECT_AUTO headless
// and rendered 4:3. This drives the shipping classifyWide that announceOnChange calls.
#include "mods.h"
#include "picture_announce.h"
#include "testutil.h"

#include <string>

namespace {

using psx::picture::classifyWide;
using psx::picture::WideOutcome;

bool mentions(WideOutcome outcome, const char *needle) {
  return std::string(psx::picture::wideOutcomeReason(outcome)).find(needle) != std::string::npos;
}
bool silent(WideOutcome outcome) {
  return psx::picture::wideOutcomeReason(outcome)[0] == '\0';
}

void test_auto_is_told_apart_from_a_real_wide_run(void) {
  const WideOutcome narrow = classifyWide(ASPECT_AUTO, true, true, 320, 320);
  const WideOutcome wide = classifyWide(ASPECT_AUTO, true, true, 320, 428);
  CHECK(narrow != wide);
  CHECK(mentions(narrow, "ASPECT_AUTO"));
  CHECK(silent(wide));
}

void test_a_named_wide_aspect_that_did_not_widen_is_not_blamed_on_auto(void) {
  const WideOutcome named = classifyWide(ASPECT_16_9, true, true, 320, 320);
  CHECK_EQ(mentions(named, "ASPECT_AUTO"), false);
  CHECK_EQ(silent(named), false);
}

void test_four_three_is_quiet(void) {
  CHECK(silent(classifyWide(ASPECT_4_3, true, true, 512, 512)));
  CHECK(silent(classifyWide(ASPECT_4_3, false, false, 512, 512)));
}

// The device path and a title without a projection are refused for their own reasons, ahead of AUTO.
void test_device_path_and_missing_projection_name_themselves(void) {
  CHECK(mentions(classifyWide(ASPECT_16_9, true, false, 320, 320), "Device"));
  CHECK(mentions(classifyWide(ASPECT_AUTO, true, false, 320, 320), "Device"));
  CHECK(mentions(classifyWide(ASPECT_16_9, false, true, 320, 320), "projection"));
}

void test_the_measured_case_from_the_repositories(void) {
  CHECK(mentions(classifyWide(ASPECT_AUTO, true, true, 512, 512), "ASPECT_AUTO"));
  CHECK(mentions(classifyWide(ASPECT_AUTO, true, true, 320, 320), "ASPECT_AUTO"));
  CHECK(silent(classifyWide(ASPECT_AUTO, true, true, 512, 684)));
  CHECK(silent(classifyWide(ASPECT_AUTO, true, true, 320, 428)));
}

} // namespace

int main(void) {
  RUN(auto_is_told_apart_from_a_real_wide_run);
  RUN(a_named_wide_aspect_that_did_not_widen_is_not_blamed_on_auto);
  RUN(four_three_is_quiet);
  RUN(device_path_and_missing_projection_name_themselves);
  RUN(the_measured_case_from_the_repositories);
  return psx::test::summary();
}

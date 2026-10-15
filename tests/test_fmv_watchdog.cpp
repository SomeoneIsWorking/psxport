// Native FMV frames are real forward progress even though they bypass the normal GPU frame path.
// This source check pins the call-site contract without needing a disc, window, timer, or signal.
#include "source_tree.h"
#include "testutil.h"
#include "watchdog.h"

#include <stdio.h>
#include <string>

namespace {

void test_native_fmv_present_reports_progress(void) {
  const std::string source = psx::test::readRepoFile("runtime/psx/movie/native_fmv.cpp");
  CHECK(!source.empty());
  const size_t present = source.find("core->game->display.presentHostPicture(rgba.data(), {width, height}, 1.0f);");
  CHECK(present != std::string::npos);
  const size_t pet = source.find("psx::platform::Watchdog::progress();", present);
  CHECK(pet != std::string::npos);
  CHECK(pet - present < 500);
}

} // namespace

int main(void) {
  RUN(native_fmv_present_reports_progress);
  return psx::test::summary();
}

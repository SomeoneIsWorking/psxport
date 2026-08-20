// Native FMV frames are real forward progress even though they bypass the normal GPU frame path.
// This source check pins the call-site contract without needing a disc, window, timer, or signal.
#include "testutil.h"

#include <stdio.h>
#include <string>

static std::string read_native_fmv(void) {
  std::string self = __FILE__;
  const size_t slash = self.find_last_of('/');
  const std::string tests = slash == std::string::npos ? "." : self.substr(0, slash);
  const std::string path = tests + "/../runtime/recomp/native_fmv.cpp";
  FILE *file = fopen(path.c_str(), "rb");
  if (!file) {
    return {};
  }
  std::string text;
  char buffer[8192];
  size_t count;
  while ((count = fread(buffer, 1, sizeof buffer, file)) != 0) {
    text.append(buffer, count);
  }
  fclose(file);
  return text;
}

static void test_native_fmv_present_reports_progress(void) {
  const std::string source = read_native_fmv();
  CHECK(!source.empty());
  const size_t present = source.find("gpu_vk_present_image(core, rgba.data(), width, height, 1.0f);");
  CHECK(present != std::string::npos);
  const size_t pet = source.find("watchdog_pet();", present);
  CHECK(pet != std::string::npos);
  CHECK(pet - present < 500);
}

int main(void) {
  RUN(native_fmv_present_reports_progress);
  return pt_summary();
}

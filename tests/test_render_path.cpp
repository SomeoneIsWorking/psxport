// test_render_path.cpp — the render path (RenderMode::path()): its names, its default and its parse.
#include "render_mode.h"
#include "testutil.h"

#include <cstring>

namespace {

void test_paths_have_names(void) {
  CHECK(strcmp(psx::gpu::renderPathName(RenderPath::Record), "record") == 0);
  CHECK(strcmp(psx::gpu::renderPathName(RenderPath::Device), "device") == 0);
}

void test_default_is_record(void) {
  RenderMode m;
  CHECK_EQ((int)m.path(), (int)RenderPath::Record);
}

// A retired or misspelt name is rejected rather than silently meaning something.
void test_parse_names_and_reject_garbage(void) {
  RenderPath p = RenderPath::Record;
  CHECK(psx::gpu::parseRenderPath("device", &p) && p == RenderPath::Device);
  CHECK(psx::gpu::parseRenderPath("DEVICE", &p) && p == RenderPath::Device); // case-insensitive
  CHECK(psx::gpu::parseRenderPath("record", &p) && p == RenderPath::Record);
  p = RenderPath::Device;
  CHECK(!psx::gpu::parseRenderPath("native", &p));
  CHECK(!psx::gpu::parseRenderPath("gte", &p));
  CHECK(!psx::gpu::parseRenderPath("", &p));
  CHECK(!psx::gpu::parseRenderPath("recor", &p)); // no prefix matching
  CHECK_EQ((int)p, (int)RenderPath::Device);      // a rejected parse leaves the target untouched
}

void test_two_cores_are_independent(void) {
  RenderMode a, b;
  a.setPath(RenderPath::Record);
  b.setPath(RenderPath::Device);
  CHECK_EQ((int)a.path(), (int)RenderPath::Record);
  CHECK_EQ((int)b.path(), (int)RenderPath::Device);
}

} // namespace

int main(void) {
  RUN(paths_have_names);
  RUN(default_is_record);
  RUN(parse_names_and_reject_garbage);
  RUN(two_cores_are_independent);
  return psx::test::summary();
}

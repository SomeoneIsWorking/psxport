// test_render_path.cpp — the RENDER PATH (RenderMode::path()).
//
// Who produces the geometry and whether PC enhancements may touch it are DERIVED from one enum, so
// an impossible pair (the device picture over native producers) cannot be spelled. Both guest paths
// are pure: enhancementsAllowed() is false for Gte and Device. The path is per-Core state.
#include "render_mode.h"
#include "testutil.h"

// Every path maps to exactly one (geometry source, enhancements) pair.
static void test_path_derives_its_answers(void) {
  struct Row {
    RenderPath path;
    bool psxRender;
    bool enh;
    const char *name;
  };
  static const Row rows[] = {
      // path                geometry from guest?  PC enhancements?
      {RenderPath::Native, false, true, "native"},
      {RenderPath::Gte, true, false, "gte"},
      {RenderPath::Device, true, false, "device"},
      {RenderPath::Record, true, false, "record"},
  };
  int n = 0;
  for (const Row &r : rows) {
    RenderMode m;
    m.setPath(r.path);
    CHECK_EQ((int)m.path(), (int)r.path);
    CHECK_EQ(m.psxRender(), r.psxRender);
    CHECK_EQ(m.enhancementsAllowed(), r.enh);
    CHECK(strcmp(render_path_name(r.path), r.name) == 0);
    n++;
  }
  CHECK_EQ(n, 4);
}

// The default must be the shipping configuration.
static void test_default_is_native(void) {
  RenderMode m;
  CHECK_EQ((int)m.path(), (int)RenderPath::Native);
  CHECK(m.enhancementsAllowed());
  CHECK(!m.psxRender());
}

// The device picture shows what the guest drew, so the device path takes guest geometry.
static void test_device_picture_implies_guest_geometry(void) {
  int seen = 0, illegal = 0;
  for (int i = 0; i <= (int)RenderPath::Record; i++) {
    RenderMode m;
    m.setPath((RenderPath)i);
    if ((m.path() == RenderPath::Device || m.path() == RenderPath::Record) && !m.psxRender()) {
      illegal++;
    }
    seen++;
  }
  CHECK_EQ(seen, 4);
  CHECK_EQ(illegal, 0);
}

// A typo is rejected rather than silently meaning `native`.
static void test_parse_names_and_reject_garbage(void) {
  RenderPath p = RenderPath::Native;
  CHECK(render_path_parse("native", &p) && p == RenderPath::Native);
  CHECK(render_path_parse("gte", &p) && p == RenderPath::Gte);
  CHECK(render_path_parse("device", &p) && p == RenderPath::Device);
  CHECK(render_path_parse("DEVICE", &p) && p == RenderPath::Device); // case-insensitive
  CHECK(render_path_parse("record", &p) && p == RenderPath::Record);
  p = RenderPath::Gte;
  CHECK(!render_path_parse("psx", &p)); // the retired software-rasterizer path
  CHECK(!render_path_parse("", &p));
  CHECK(!render_path_parse("nativ", &p)); // no prefix matching: a truncation is a typo
  CHECK_EQ((int)p, (int)RenderPath::Gte); // a rejected parse leaves the target UNTOUCHED
}

// Per-Core independence: two RenderModes never share state.
static void test_two_cores_are_independent(void) {
  RenderMode a, b;
  a.setPath(RenderPath::Native);
  b.setPath(RenderPath::Device);
  CHECK(a.enhancementsAllowed());
  CHECK(!b.enhancementsAllowed());
  CHECK(!a.psxRender());
  CHECK(b.psxRender());
}

int main(void) {
  RUN(path_derives_its_answers);
  RUN(default_is_native);
  RUN(device_picture_implies_guest_geometry);
  RUN(parse_names_and_reject_garbage);
  RUN(two_cores_are_independent);
  return pt_summary();
}

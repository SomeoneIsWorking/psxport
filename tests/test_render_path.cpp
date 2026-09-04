// test_render_path.cpp — the RENDER PATH tri-state (RenderMode::path()).
//
// The switch the user asked for: "a toggle to switch between PC render native, PC render from GTE and
// pure PSX rasterizer" (2026-08-11). The renderer boundary is documented in docs/codemap.md.
//
// WHAT THIS TEST IS FOR, and why it is not a tautology. Before the tri-state, the three modes existed
// as THREE INDEPENDENT switches — RenderMode::mPsxRender (geometry source), GpuState::soft_gpu (the
// rasterizer, reachable only from the test harness and the selftest), while the former reference mode
// enhancement lockout). Nothing tied them together, so the combinations that are NOT modes were
// expressible and one of them (PSX rasterizer + native producers) draws a black screen: the native
// producers push geometry to the VK backend while the presenter shows the software framebuffer that
// nothing rasterized into. This asserts that the two questions a render path answers — "who produces
// the geometry" and "who rasterizes it" — are DERIVED from one enum, so an impossible pair cannot be
// spelled.
//
// It also pins the decisions taken with the user, because both are behaviour, not naming:
//   * BOTH guest paths are pure. enhancementsAllowed() is false for Gte as well as Psx — "I don't want
//     GTE enhancements, GTE/OT should stay pure" / "Yes fps60/wide/native-depth is supposed to be
//     native-only". So fps60, widescreen, ires and the deferred passes are native-only, and modes 2
//     and 3 differ by exactly one thing: the rasterizer.
//   * The path is PER-CORE state, so SBS/dualcore can run leg A native against leg B pure without
//     either leg observing the other's setting.
#include "render_mode.h"
#include "testutil.h"

// Every path maps to exactly one (geometry source, rasterizer, enhancements) triple. Spelled as a
// table so a future path (or a changed meaning) has to edit the EXPECTATION, not just the code.
static void test_path_derives_all_three_answers(void) {
  struct Row {
    RenderPath path;
    bool psxRender;
    bool softGpu;
    bool enh;
    const char *name;
  };
  static const Row rows[] = {
      // path                geometry from guest?  software raster?  PC enhancements?
      {RenderPath::Native, false, false, true, "native"},
      {RenderPath::Gte, true, false, false, "gte"},
      {RenderPath::Psx, true, true, false, "psx"},
  };
  int n = 0;
  for (const Row &r : rows) {
    RenderMode m;
    m.setPath(r.path);
    CHECK_EQ((int)m.path(), (int)r.path);
    CHECK_EQ(m.psxRender(), r.psxRender);
    CHECK_EQ(m.softGpu(), r.softGpu);
    CHECK_EQ(m.enhancementsAllowed(), r.enh);
    CHECK(strcmp(render_path_name(r.path), r.name) == 0);
    n++;
  }
  CHECK_EQ(n, 3); // the denominator: three paths were actually exercised, not zero
}

// The default must be the shipping configuration. A default of Gte or Psx would silently turn the
// port into its own reference implementation.
static void test_default_is_native(void) {
  RenderMode m;
  CHECK_EQ((int)m.path(), (int)RenderPath::Native);
  CHECK(m.enhancementsAllowed());
  CHECK(!m.psxRender());
  CHECK(!m.softGpu());
}

// THE COMBINATION THAT MUST NOT BE EXPRESSIBLE: software rasterizer + native geometry. Enumerate the
// whole (psxRender, softGpu) space reachable through the enum and assert softGpu implies psxRender.
static void test_software_raster_implies_guest_geometry(void) {
  int seen = 0, illegal = 0;
  for (int i = 0; i <= (int)RenderPath::Psx; i++) {
    RenderMode m;
    m.setPath((RenderPath)i);
    if (m.softGpu() && !m.psxRender()) {
      illegal++;
    }
    seen++;
  }
  CHECK_EQ(seen, 3);    // scanned all three paths…
  CHECK_EQ(illegal, 0); // …and none of them spells the black-screen pair
}

// Parsing is what the CVar layer and the REPL command both go through, so a typo must be REJECTED
// rather than silently meaning `native` (the workspace's "a knob that matched nothing did nothing"
// rule, applied one level down).
static void test_parse_names_and_reject_garbage(void) {
  RenderPath p = RenderPath::Native;
  CHECK(render_path_parse("native", &p) && p == RenderPath::Native);
  CHECK(render_path_parse("gte", &p) && p == RenderPath::Gte);
  CHECK(render_path_parse("psx", &p) && p == RenderPath::Psx);
  CHECK(render_path_parse("PSX", &p) && p == RenderPath::Psx); // case-insensitive
  p = RenderPath::Gte;
  CHECK(!render_path_parse("softgpu", &p)); // not a name
  CHECK(!render_path_parse("", &p));
  CHECK(!render_path_parse("nativ", &p)); // no prefix matching: a truncation is a typo
  CHECK_EQ((int)p, (int)RenderPath::Gte); // a rejected parse leaves the target UNTOUCHED
}

// Per-Core independence: two RenderModes (SBS's two cores) never share state.
static void test_two_cores_are_independent(void) {
  RenderMode a, b;
  a.setPath(RenderPath::Native);
  b.setPath(RenderPath::Psx);
  CHECK(a.enhancementsAllowed());
  CHECK(!b.enhancementsAllowed());
  CHECK(!a.softGpu());
  CHECK(b.softGpu());
}

int main(void) {
  RUN(path_derives_all_three_answers);
  RUN(default_is_native);
  RUN(software_raster_implies_guest_geometry);
  RUN(parse_names_and_reject_garbage);
  RUN(two_cores_are_independent);
  return pt_summary();
}

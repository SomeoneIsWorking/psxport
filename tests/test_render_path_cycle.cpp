// test_render_path_cycle.cpp — the render-path cycle and its user-facing RmlUi ownership.
//
// WHY IT MATTERS THAT THIS IS ONE FUNCTION. The user asked for a toggle between three renderers precisely
// so they can be compared, and a comparison is only readable if the ORDER is predictable. Before
// render_path_next() existed, the REPL cycled with `(int)p + 1) % 3` inline. The menu and REPL must use
// this same definition rather than growing separate arithmetic that drifts when a path is added.
//
// THE ORDER IS PART OF THE CONTRACT, not an implementation detail: Native -> Gte -> Psx. Consecutive
// presses isolate ONE variable at a time — Native->Gte swaps the geometry source (PC producers vs the
// guest's own GTE/OT output) with the PC rasterizer held fixed; Gte->Psx swaps the rasterizer with the
// geometry held fixed. An order that changed both at once (e.g. Native->Psx) would make a visible
// difference unattributable, which defeats the toggle's purpose. So the order is asserted, not just the
// fact that it cycles.
//
// Hermetic: render_mode.h is a header of value types — no Core, no SDL, no window.
#include "render_mode.h"
#include "testutil.h"
#include <stdio.h>
#include <string.h>
#include <string>

static std::string source_path(const char *relative) {
  std::string self = __FILE__;
  const size_t slash = self.find_last_of('/');
  const std::string tests = slash == std::string::npos ? "." : self.substr(0, slash);
  return tests + "/../" + relative;
}

static std::string read_source(const char *relative) {
  FILE *file = fopen(source_path(relative).c_str(), "rb");
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

static int count_text(const std::string &text, const char *needle) {
  int count = 0;
  for (size_t at = 0; (at = text.find(needle, at)) != std::string::npos; at += strlen(needle)) {
    count++;
  }
  return count;
}

static void test_renderer_selection_is_owned_by_rmlui_not_f5(void) {
  const std::string pad = read_source("runtime/recomp/pad_input.cpp");
  const std::string pad_header = read_source("runtime/recomp/pad_input.h");
  const std::string menu = read_source("assets/rml/menu.rml");
  const std::string document = read_source("runtime/ui/menu_document.cpp");
  CHECK(!pad.empty());
  CHECK(!pad_header.empty());
  CHECK(!menu.empty());
  CHECK(!document.empty());
  CHECK_EQ(count_text(pad, "SDL_SCANCODE_F5"), 0);
  CHECK_EQ(count_text(pad_header, "mPrevRenderPath"), 0);
  CHECK_EQ(count_text(menu, "toggle=\"render_path\""), 1);
  CHECK_EQ(count_text(document, "make_render_path_binding(&mRenderPath)"), 1);
}

// The full cycle, asserted as a sequence rather than "it changed": a function that returned some other
// path every time would satisfy a mere inequality check.
static void test_cycle_order_is_native_gte_psx(void) {
  CHECK(render_path_next(RenderPath::Native) == RenderPath::Gte);
  CHECK(render_path_next(RenderPath::Gte) == RenderPath::Psx);
  CHECK(render_path_next(RenderPath::Psx) == RenderPath::Native);
}

// Three presses from any starting point return to where they began — the property a user relies on when
// they cycle past the one they wanted.
static void test_three_steps_returns_to_start(void) {
  const RenderPath starts[3] = {RenderPath::Native, RenderPath::Gte, RenderPath::Psx};
  for (int i = 0; i < 3; i++) {
    RenderPath p = starts[i];
    for (int k = 0; k < 3; k++) {
      p = render_path_next(p);
    }
    CHECK(p == starts[i]);
  }
}

// Every path is REACHABLE, and the cycle visits each exactly once per lap. A cycle that skipped a path
// would leave one renderer unreachable by the menu while every individual step still looked sane —
// exactly the kind of gap a "it advances" assertion cannot see.
static void test_cycle_visits_all_three_exactly_once(void) {
  int seen[3] = {0, 0, 0};
  RenderPath p = RenderPath::Native;
  for (int k = 0; k < 3; k++) {
    seen[(int)p]++;
    p = render_path_next(p);
  }
  CHECK_EQ(seen[(int)RenderPath::Native], 1);
  CHECK_EQ(seen[(int)RenderPath::Gte], 1);
  CHECK_EQ(seen[(int)RenderPath::Psx], 1);
}

// The names the menu prints must round-trip through the parser, or the CVar Runtime mirror the menu
// writes would be a value the config layer cannot read back.
static void test_cycled_names_round_trip_through_parse(void) {
  RenderPath p = RenderPath::Native;
  for (int k = 0; k < 3; k++) {
    const char *name = render_path_name(p);
    CHECK(name != NULL && strcmp(name, "?") != 0);
    RenderPath back = RenderPath::Native;
    CHECK(render_path_parse(name, &back));
    CHECK(back == p);
    p = render_path_next(p);
  }
}

int main(void) {
  RUN(renderer_selection_is_owned_by_rmlui_not_f5);
  RUN(cycle_order_is_native_gte_psx);
  RUN(three_steps_returns_to_start);
  RUN(cycle_visits_all_three_exactly_once);
  RUN(cycled_names_round_trip_through_parse);
  return pt_summary();
}

// test_render_path_cycle.cpp — the render-path CYCLE: one definition, shared by the F5 hotkey and the
// REPL's bare `renderpath`.
//
// WHY IT MATTERS THAT THIS IS ONE FUNCTION. The user asked for a toggle between three renderers precisely
// so they can be compared, and a comparison is only readable if the ORDER is predictable. Before
// render_path_next() existed, the REPL cycled with `(int)p + 1) % 3` inline; adding a hotkey with its own
// arithmetic would have been two definitions of "next" that agree today and drift the first time a fourth
// path is added — and the drift would be invisible, because each one looks correct on its own.
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
#include <string.h>

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
// would leave one renderer unreachable by the hotkey while every individual step still looked sane —
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

// The names the hotkey prints must round-trip through the parser, or the CVar Runtime mirror the hotkey
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
  RUN(cycle_order_is_native_gte_psx);
  RUN(three_steps_returns_to_start);
  RUN(cycle_visits_all_three_exactly_once);
  RUN(cycled_names_round_trip_through_parse);
  return pt_summary();
}

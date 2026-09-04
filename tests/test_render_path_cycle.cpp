// test_render_path_cycle.cpp — the diagnostic and player-facing render-path cycles.
//
// There are deliberately TWO cycles. Explicit diagnostics compare all three implementations through
// render_path_next(). The RmlUi player menu uses player_render_path_next() and excludes the software PSX
// path, which is not a supported live gameplay renderer. Keeping these policies named and tested prevents
// diagnostic reachability from silently becoming a shipping menu option again.
//
// THE DIAGNOSTIC ORDER IS PART OF ITS CONTRACT: Native -> Gte -> Psx. Consecutive diagnostic steps
// isolate producers and then rasterizers. It is separate from the two-choice RmlUi contract below.
//
// Hermetic: the policy takes only RenderPath values — no Game is constructed, no SDL, no window.
#include "render_mode.h"
#include "render_path_control.h"
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
  const std::string pad = read_source("runtime/psx/pad_input.cpp");
  const std::string pad_header = read_source("runtime/psx/pad_input.h");
  const std::string menu = read_source("assets/rml/menu.rml");
  const std::string document = read_source("runtime/ui/menu_document.cpp");
  const std::string control = read_source("runtime/ui/render_path_control.cpp");
  const std::string pane = read_source("runtime/ui/menu_pane.cpp");
  CHECK(!pad.empty());
  CHECK(!pad_header.empty());
  CHECK(!menu.empty());
  CHECK(!document.empty());
  CHECK(!control.empty());
  CHECK(!pane.empty());
  CHECK_EQ(count_text(pad, "SDL_SCANCODE_F5"), 0);
  CHECK_EQ(count_text(pad_header, "mPrevRenderPath"), 0);
  CHECK_EQ(count_text(menu, "toggle=\"render_path\""), 1);
  CHECK_EQ(count_text(document, "make_render_path_binding(&mRenderPath)"), 1);
  CHECK_EQ(count_text(control, "player_render_path_next"), 2); // one definition, one RmlUi call
  CHECK_EQ(count_text(pane, "!binding->available()"), 1);
  CHECK_EQ(count_text(pane, "SetProperty(\"display\", \"none\")"), 1);
}

// GTE/PSX is an oracle/diagnostic implementation, not a supported player renderer. The RmlUi control
// must offer only the shipping native picture and the guest-geometry comparison on the PC rasterizer.
static void test_player_cycle_excludes_diagnostic_psx_path(void) {
  const RenderCapabilities capabilities = RenderCapabilities::interpolatedNative();
  CHECK(psx::ui::player_render_path_next(RenderPath::Native, capabilities) == RenderPath::Gte);
  CHECK(psx::ui::player_render_path_next(RenderPath::Gte, capabilities) == RenderPath::Native);
  CHECK(psx::ui::player_render_path_next(RenderPath::Psx, capabilities) == RenderPath::Native);

  RenderPath path = RenderPath::Native;
  for (int i = 0; i < 8; i++) {
    path = psx::ui::player_render_path_next(path, capabilities);
    CHECK(path != RenderPath::Psx);
  }
}

static void test_widescreen_only_profile_has_no_native_or_temporal_choice(void) {
  const RenderCapabilities capabilities = RenderCapabilities::widescreenOnly();
  CHECK(capabilities.defaultPath == RenderPath::Gte);
  CHECK(!capabilities.supports(RenderPath::Native));
  CHECK(capabilities.supports(RenderPath::Gte));
  CHECK(capabilities.supports(RenderPath::Psx));
  CHECK(!capabilities.temporalInterpolation);
  CHECK_EQ(capabilities.playerPathCount(), 1);
  CHECK(psx::ui::player_render_path_next(RenderPath::Gte, capabilities) == RenderPath::Gte);
  CHECK(render_path_next_supported(RenderPath::Gte, capabilities, RenderPathAudience::Diagnostic) == RenderPath::Psx);
  CHECK(render_path_next_supported(RenderPath::Psx, capabilities, RenderPathAudience::Diagnostic) == RenderPath::Gte);
  CHECK(render_path_resolve(RenderPath::Native, capabilities) == RenderPath::Gte);
}

static void test_interpolated_native_profile_retains_both_player_choices(void) {
  const RenderCapabilities capabilities = RenderCapabilities::interpolatedNative();
  CHECK(capabilities.defaultPath == RenderPath::Native);
  CHECK(capabilities.supports(RenderPath::Native));
  CHECK(capabilities.temporalInterpolation);
  CHECK_EQ(capabilities.playerPathCount(), 2);
  CHECK(render_path_resolve(RenderPath::Native, capabilities) == RenderPath::Native);
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
  RUN(player_cycle_excludes_diagnostic_psx_path);
  RUN(widescreen_only_profile_has_no_native_or_temporal_choice);
  RUN(interpolated_native_profile_retains_both_player_choices);
  RUN(cycle_order_is_native_gte_psx);
  RUN(three_steps_returns_to_start);
  RUN(cycle_visits_all_three_exactly_once);
  RUN(cycled_names_round_trip_through_parse);
  return pt_summary();
}

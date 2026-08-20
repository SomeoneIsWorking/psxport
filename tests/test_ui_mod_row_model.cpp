// test_ui_mod_row_model — the overlay menu's ROW MODEL: what each row displays, how it cycles, and
// what happens to an id nobody recognises.
//
// WHY THIS UNIT AND NOT THE WIDGET. Splitting `rmlui_overlay.cpp` moved three parallel if-else
// ladders — one to format a row's value, one to toggle it, one to step it — into ONE table
// (`runtime/ui/mod_row_model.cpp`). A table can be wrong in ways three ladders could not: a single
// generic cycle now stands in for three hand-written ones, and a single generic clamp stands in for
// two different out-of-range behaviours. Those are exactly the collapses that look like cleanup and
// silently change behaviour, so they are what this file pins.
//
// HERMETIC: no disc, no GPU, no window, no RmlUi, no document. `Mods` is a plain object and
// `ModRowModel` is pure table lookup over it. `Mods::save()` writes a settings file, so the
// settings path is redirected into the build tree's own scratch before anything mutating runs —
// stated here because a test that quietly wrote the developer's real psxport_settings.ini would be
// a test with a side effect nobody expects.
//
// NEGATIVE-RESULT DISCIPLINE. `knows()` is a DISCRIMINATOR, so it is run against BOTH classes: the
// full set of real ids AND a set of near-misses. A discriminator that has only ever been shown
// answering "yes" has not been tested — it could be `return true;`.

#include "testutil.h"

#include "config_var.h"
#include "config_vars.h"
#include "mod_row_model.h"
#include "mods.h"

#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

using psx::ui::ModRowModel;
using psx::ui::RowKind;

// Every id the shipped assets/rml/menu.rml actually authors, plus debug_ids, which the model knows
// and the shipped document does NOT use (it is reachable by a game shipping its own menu.rml).
static const char *const kRealToggles[] = {
    "aspect",
    "ires",
    "face_order",
    "fps60",
    "ssao",
    "light",
    "shadows",
    "debug_ids",
    "debug_quads",
    "debug_objects",
};
static const char *const kRealAdjusts[] = {
    "ssao_strength",
    "ssao_radius",
    "ssao_bias",
    "ssao_range",
    "light_dir_x",
    "light_dir_y",
    "light_dir_z",
    "light_ambient",
    "light_diffuse",
    "shadow_strength",
};
// Near-misses: plausible typos, wrong-kind lookups, and an id from the OTHER table. Each one must
// come back false, and `warp_area` is in here on purpose — it is an adjust row whose model is
// WarpControl, not Mods, so ModRowModel must NOT claim it.
static const char *const kNotToggles[] = {
    "ssoa",
    "aspect_ratio",
    "ASPECT",
    "",
    "ssao_strength",
    "warp_area",
    "quit",
    "music_0",
};
static const char *const kNotAdjusts[] = {
    "ssao_strenght",
    "light_dir_w",
    "",
    "aspect",
    "warp_area",
    "shadow",
    "close",
};

static std::string text_of(const Mods &m, RowKind k, const char *id) {
  std::string out = "<not-set>";
  ModRowModel::value_text(m, k, id, out);
  return out;
}

// ---- 1. the discriminator, run against BOTH classes -----------------------------------------------
static void test_knows_answers_both_ways(void) {
  int yes = 0, no = 0;
  for (const char *id : kRealToggles) {
    CHECK(ModRowModel::knows(RowKind::Toggle, id));
    yes++;
  }
  for (const char *id : kRealAdjusts) {
    CHECK(ModRowModel::knows(RowKind::Adjust, id));
    yes++;
  }
  for (const char *id : kNotToggles) {
    CHECK(!ModRowModel::knows(RowKind::Toggle, id));
    no++;
  }
  for (const char *id : kNotAdjusts) {
    CHECK(!ModRowModel::knows(RowKind::Adjust, id));
    no++;
  }
  // Action rows are not this model's business at all.
  CHECK(!ModRowModel::knows(RowKind::Action, "quit"));
  CHECK(!ModRowModel::knows(RowKind::None, "aspect"));
  fprintf(stderr, "    [model] knows(): %d ids that MUST be known, %d that MUST NOT — both checked\n", yes, no);
  // The tables' own counts must match what this file believes exists, or the lists above have
  // drifted and every "MUST NOT be known" case is testing a shrunken table.
  CHECK_EQ(ModRowModel::toggle_count(), (int)(sizeof(kRealToggles) / sizeof(kRealToggles[0])));
  CHECK_EQ(ModRowModel::adjust_count(), (int)(sizeof(kRealAdjusts) / sizeof(kRealAdjusts[0])));
}

// ---- 2. every label of every multi-state row --------------------------------------------------------
static void test_toggle_labels_cover_every_state(void) {
  Mods m;
  static const char *const kAspect[] = {"Vanilla", "16:9", "21:9", "Auto"};
  for (int i = 0; i < 4; i++) {
    m.aspect = i;
    CHECK_STREQ(text_of(m, RowKind::Toggle, "aspect").c_str(), kAspect[i]);
  }
  // 0 is Auto for this row and 1 is Vanilla — the two rows' index conventions genuinely differ.
  static const char *const kIres[] = {"Auto", "Vanilla", "X2", "X3", "X4"};
  for (int i = 0; i < 5; i++) {
    m.ires = i;
    CHECK_STREQ(text_of(m, RowKind::Toggle, "ires").c_str(), kIres[i]);
  }
  m.ssao = 0;
  CHECK_STREQ(text_of(m, RowKind::Toggle, "ssao").c_str(), "Off");
  m.ssao = 1;
  CHECK_STREQ(text_of(m, RowKind::Toggle, "ssao").c_str(), "On");
}

// ---- 3. THE COLLAPSE THAT WOULD HAVE BEEN SILENT ------------------------------------------------------
// The pre-split code clamped an out-of-range `aspect` to index 0 and an out-of-range `ires` to index
// 1. Both display "Vanilla", which is why a single shared "clamp to 0" looks correct — and it is
// not: index 0 of the ires row is "Auto". A settings file carrying a bad value would silently start
// showing (and, on the next activation, cycling from) the wrong state. This is the negative control
// for the table rewrite: it is the case a naive generic clamp fails.
static void test_out_of_range_falls_back_per_row(void) {
  Mods m;
  for (int bad : {-1, 4, 99, 1 << 20}) {
    m.aspect = bad;
    CHECK_STREQ(text_of(m, RowKind::Toggle, "aspect").c_str(), "Vanilla"); // index 0
  }
  for (int bad : {-1, 5, 99, 1 << 20}) {
    m.ires = bad;
    CHECK_STREQ(text_of(m, RowKind::Toggle, "ires").c_str(), "Vanilla"); // index 1, NOT "Auto"
    CHECK(text_of(m, RowKind::Toggle, "ires") != "Auto");
  }
}

// ---- 4. the cycles ------------------------------------------------------------------------------------
static void test_toggle_cycles_and_wraps(void) {
  Mods m;
  // A bool row: 0 <-> 1. debug_quads is deliberately chosen because it does NOT persist, so this
  // case touches no file at all.
  m.debug_quads = 0;
  ModRowModel::toggle(m, "debug_quads");
  CHECK_EQ(m.debug_quads, 1);
  ModRowModel::toggle(m, "debug_quads");
  CHECK_EQ(m.debug_quads, 0);

  m.aspect = 0;
  for (int expect : {1, 2, 3, 0, 1}) {
    ModRowModel::toggle(m, "aspect");
    CHECK_EQ(m.aspect, expect);
  }
  // Five states, wrapping 4 -> 0. The old hand-written form was `ires += 1; if (ires > 4) ires = 0`.
  m.ires = 0;
  for (int expect : {1, 2, 3, 4, 0, 1}) {
    ModRowModel::toggle(m, "ires");
    CHECK_EQ(m.ires, expect);
  }
}

// ---- 5. adjust: step, clamp, precision -----------------------------------------------------------------
static void test_adjust_clamps_at_both_ends(void) {
  Mods m;
  m.ssao_strength = 1.0f;
  ModRowModel::adjust(m, "ssao_strength", +1);
  CHECK_STREQ(text_of(m, RowKind::Adjust, "ssao_strength").c_str(), "1.05");
  ModRowModel::adjust(m, "ssao_strength", -1);
  CHECK_STREQ(text_of(m, RowKind::Adjust, "ssao_strength").c_str(), "1.00");
  // Walk hard into both stops. 2.0 high, 0.0 low.
  for (int i = 0; i < 200; i++) {
    ModRowModel::adjust(m, "ssao_strength", +1);
  }
  CHECK_STREQ(text_of(m, RowKind::Adjust, "ssao_strength").c_str(), "2.00");
  for (int i = 0; i < 200; i++) {
    ModRowModel::adjust(m, "ssao_strength", -1);
  }
  CHECK_STREQ(text_of(m, RowKind::Adjust, "ssao_strength").c_str(), "0.00");

  // Per-row precision is part of what the row displays: 3 decimals for bias, 1 for radius.
  m.ssao_bias = 0.02f;
  CHECK_STREQ(text_of(m, RowKind::Adjust, "ssao_bias").c_str(), "0.020");
  m.ssao_radius = 6.0f;
  CHECK_STREQ(text_of(m, RowKind::Adjust, "ssao_radius").c_str(), "6.0");

  // light_dir is a float[3]; the three rows must reach three DIFFERENT elements. A table wired
  // through one accessor by mistake would move all three together and every value would still look
  // plausible.
  m.light_dir[0] = m.light_dir[1] = m.light_dir[2] = 0.0f;
  ModRowModel::adjust(m, "light_dir_y", +1);
  CHECK_STREQ(text_of(m, RowKind::Adjust, "light_dir_x").c_str(), "0.00");
  CHECK_STREQ(text_of(m, RowKind::Adjust, "light_dir_y").c_str(), "0.05");
  CHECK_STREQ(text_of(m, RowKind::Adjust, "light_dir_z").c_str(), "0.00");
}

// ---- 6. an unknown id must not be a silent no-op ---------------------------------------------------------
static void test_unknown_id_reports_rather_than_pretending(void) {
  Mods m;
  m.aspect = 2;
  m.ssao_strength = 1.0f;

  // value_text must SAY it does not know, leaving the caller's string untouched — that false is
  // what MenuDocument::bind_row turns into a named error at load time. Returning true with an empty
  // string would render a blank row and look like a styling bug.
  std::string out = "<untouched>";
  CHECK(!ModRowModel::value_text(m, RowKind::Toggle, "ssoa", out));
  CHECK_STREQ(out.c_str(), "<untouched>");
  CHECK(!ModRowModel::value_text(m, RowKind::Adjust, "ssao_strenght", out));
  CHECK_STREQ(out.c_str(), "<untouched>");

  // And the mutators must not touch anything at all — in particular not the row whose name the
  // typo is closest to.
  ModRowModel::toggle(m, "ssoa");
  ModRowModel::adjust(m, "ssao_strenght", +1);
  CHECK_EQ(m.aspect, 2);
  CHECK_STREQ(text_of(m, RowKind::Adjust, "ssao_strength").c_str(), "1.00");
}

int main(void) {
  // Redirect the settings file the persisting rows write, so this test cannot clobber a real one.
  // Setting the Override layer directly is the hermetic form — no environment, no process state.
  // It is REMOVED at the end: a hermetic test that leaves a file behind in whatever directory it
  // happened to be run from is not hermetic, it just fails somewhere else later.
  const char *kSettings = "scratch_test_ui_mod_row_model_settings.ini";
  psx::config::cv_settings_path.set_text(psx::config::Layer::Override, kSettings);

  RUN(knows_answers_both_ways);
  RUN(toggle_labels_cover_every_state);
  RUN(out_of_range_falls_back_per_row);
  RUN(toggle_cycles_and_wraps);
  RUN(adjust_clamps_at_both_ends);
  RUN(unknown_id_reports_rather_than_pretending);

  std::error_code ec;
  std::filesystem::remove(kSettings, ec); // best-effort; the rows above may never have saved
  return pt_summary();
}

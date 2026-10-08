// test_choice_navigator.cpp — which entry of a "choose one" screen is highlighted, and whether it may be
// taken (runtime/ui/choice_navigator.h). Hermetic: no RmlUi, no window.
//
// A disabled entry (a title that is not provisioned) must stay visible but be unreachable by navigation
// and never activatable; a list with NO enabled entry must answer "nothing selected" rather than index 0.
#include "../runtime/ui/choice_navigator.h"
#include "testutil.h"

using psx::ui::ChoiceEntry;
using psx::ui::ChoiceNavigator;

static ChoiceEntry entry(bool enabled) {
  return ChoiceEntry{"label", "detail", enabled, enabled ? "" : "disabled"};
}

static void test_starts_on_first_enabled(void) {
  ChoiceNavigator nav({entry(false), entry(true), entry(true)});
  CHECK_EQ(nav.selected(), 1);
  CHECK_EQ(nav.enabledCount(), 2);
}

static void test_move_skips_disabled_and_wraps(void) {
  ChoiceNavigator nav({entry(true), entry(false), entry(true)});
  CHECK_EQ(nav.selected(), 0);
  CHECK(nav.move(+1));
  CHECK_EQ(nav.selected(), 2); // skipped the disabled middle entry
  CHECK(nav.move(+1));
  CHECK_EQ(nav.selected(), 0); // wrapped
  CHECK(nav.move(-1));
  CHECK_EQ(nav.selected(), 2);
}

static void test_single_enabled_does_not_move(void) {
  ChoiceNavigator nav({entry(false), entry(true), entry(false)});
  CHECK(!nav.move(+1));
  CHECK_EQ(nav.selected(), 1);
}

static void test_nothing_enabled_selects_nothing(void) {
  ChoiceNavigator nav({entry(false), entry(false)});
  CHECK_EQ(nav.selected(), -1);
  CHECK_EQ(nav.enabledCount(), 0);
  CHECK(!nav.activate().has_value());
  CHECK(!nav.move(+1));
}

static void test_select_refuses_disabled_and_out_of_range(void) {
  ChoiceNavigator nav({entry(true), entry(false), entry(true)});
  CHECK(!nav.select(1));
  CHECK(!nav.select(-1));
  CHECK(!nav.select(3));
  CHECK_EQ(nav.selected(), 0);
  CHECK(nav.select(2));
  CHECK_EQ(nav.selected(), 2);
}

static void test_activate_returns_the_highlight(void) {
  ChoiceNavigator nav({entry(false), entry(true)});
  CHECK(nav.activate().has_value());
  CHECK_EQ(*nav.activate(), 1);
}

int main(void) {
  RUN(starts_on_first_enabled);
  RUN(move_skips_disabled_and_wraps);
  RUN(single_enabled_does_not_move);
  RUN(nothing_enabled_selects_nothing);
  RUN(select_refuses_disabled_and_out_of_range);
  RUN(activate_returns_the_highlight);
  return pt_summary();
}

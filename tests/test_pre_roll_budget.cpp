#include "pre_roll_budget.h"
#include "testutil.h"

#include <chrono>

namespace {

using Clock = psx::host::PreRollBudget::Clock;
using std::chrono::milliseconds;

void test_a_pre_roll_stops_before_the_field_ends() {
  const Clock::time_point start{};
  const psx::host::PreRollBudget budget(start + milliseconds(16));
  CHECK(budget.mayStep(1, start + milliseconds(4), milliseconds(4)));
  CHECK(budget.mayStep(3, start + milliseconds(12), milliseconds(4)));
  CHECK(!budget.mayStep(3, start + milliseconds(13), milliseconds(4)));
}

void test_the_first_step_always_runs() {
  const Clock::time_point start{};
  const psx::host::PreRollBudget budget(start + milliseconds(16));
  CHECK(budget.mayStep(0, start + milliseconds(40), milliseconds(0)));
  CHECK(!budget.mayStep(1, start + milliseconds(40), milliseconds(40)));
}

} // namespace

int main() {
  RUN(a_pre_roll_stops_before_the_field_ends);
  RUN(the_first_step_always_runs);
  return psx::test::summary();
}

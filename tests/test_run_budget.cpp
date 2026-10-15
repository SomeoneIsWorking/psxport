// The run's cap: RunBudget's precedence rule, and the title's own end request.
#include "config.h"
#include "config_vars.h"
#include "run_budget.h"
#include "session_run.h"
#include "testutil.h"
#include "typed_config.h"

namespace {

using psx::host::RunBudget;
using psx::host::RunBudgetInputs;

void test_an_explicit_cap_wins_over_everything(void) {
  const RunBudget budget = RunBudget::decide(
      RunBudgetInputs{.explicitCap = 7, .padRecording = true, .repl = true, .clientLive = true, .windowed = true});
  CHECK_EQ(budget.cap(), 7u);
}

void test_a_pad_recording_lifts_the_headless_cap(void) {
  CHECK_EQ(RunBudget::decide(RunBudgetInputs{.padRecording = true}).cap(), 0u);
}

void test_a_client_or_the_repl_runs_uncapped(void) {
  CHECK_EQ(RunBudget::decide(RunBudgetInputs{.clientLive = true}).cap(), 0u);
  CHECK_EQ(RunBudget::decide(RunBudgetInputs{.repl = true}).cap(), 0u);
}

void test_headless_is_bounded_and_windowed_is_not(void) {
  CHECK_EQ(RunBudget::decide(RunBudgetInputs{}).cap(), RunBudget::kHeadlessDefaultSteps);
  CHECK_EQ(RunBudget::decide(RunBudgetInputs{.windowed = true}).cap(), 0u);
}

void test_the_budget_runs_out_on_its_last_step(void) {
  RunBudget budget(2);
  CHECK(!budget.exhausted());
  budget.step();
  CHECK(!budget.exhausted());
  budget.step();
  CHECK(budget.exhausted());
  RunBudget uncapped;
  for (int i = 0; i < 1000; ++i) {
    uncapped.step();
  }
  CHECK(!uncapped.exhausted());
}

// The live-client rule is read from configuration here and nowhere else; the channel only opens.
void test_a_configured_client_lifts_the_cap_unless_one_is_asked_for(void) {
  const psx::config::Config config{};
  psx::config::set_runtime("PSXPORT_DEBUG_SERVER", "1");
  CHECK_EQ(RunBudget::fromConfig(config).cap(), 0u);
  psx::config::set_runtime("PSXPORT_NATIVE_FRAMES", std::to_string(9));
  CHECK_EQ(RunBudget::fromConfig(config).cap(), 9u);
  psx::config::clear_runtime("PSXPORT_NATIVE_FRAMES");
  psx::config::clear_runtime("PSXPORT_DEBUG_SERVER");
  CHECK_EQ(RunBudget::fromConfig(config).cap(), RunBudget::kHeadlessDefaultSteps);
}

void test_the_launch_repl_runs_uncapped() {
  CHECK_EQ(RunBudget::fromConfig(psx::config::Config{.repl = true}).cap(), 0u);
}

void test_a_title_ends_its_own_run(void) {
  psx::host::SessionRun run;
  CHECK(!run.shouldEnd());
  run.requestEnd();
  CHECK(run.shouldEnd());
}

} // namespace

int main(void) {
  RUN(an_explicit_cap_wins_over_everything);
  RUN(a_pad_recording_lifts_the_headless_cap);
  RUN(a_client_or_the_repl_runs_uncapped);
  RUN(headless_is_bounded_and_windowed_is_not);
  RUN(the_budget_runs_out_on_its_last_step);
  RUN(a_configured_client_lifts_the_cap_unless_one_is_asked_for);
  RUN(the_launch_repl_runs_uncapped);
  RUN(a_title_ends_its_own_run);
  return psx::test::summary();
}

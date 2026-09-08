#include "config_vars.h"
#include "frame_pacer.h"
#include "game.h"
#include "game_iface.h"
#include "testutil.h"

#include <memory>

namespace {

constexpr uint32_t kIStat = 0x1F801070u;

void test_host_deadlines_belong_to_each_instance() {
  auto first = std::make_unique<Game>();
  auto second = std::make_unique<Game>();
  PaceInputs inputs{.quota = 1, .parts = 1, .fieldRateMilliHz = 50000, .nowMs = 1000};
  const auto a = first->framePacer.plan(inputs);
  CHECK(a.paced);
  CHECK_EQ(a.nextMs, 1020.0);
  inputs.nowMs = 9000;
  CHECK_EQ(second->framePacer.plan(inputs).nextMs, 9020.0);
  inputs.nowMs = 1010;
  CHECK_EQ(first->framePacer.plan(inputs).nextMs, 1040.0);
}

void test_unpaced_calls_preserve_the_instance_deadline() {
  FramePacer pacer;
  PaceInputs inputs{.quota = 2, .parts = 2, .fieldRateMilliHz = 50000, .nowMs = 1000};
  CHECK_EQ(pacer.plan(inputs).nextMs, 1020.0);
  inputs.unpaced = true;
  inputs.nowMs = 9000;
  const auto paused = pacer.plan(inputs);
  CHECK(!paused.paced);
  CHECK_EQ(paused.nextMs, 1020.0);
  inputs.unpaced = false;
  inputs.nowMs = 1020;
  CHECK_EQ(pacer.plan(inputs).nextMs, 1040.0);
}

void test_default_runtime_still_delivers_display_time_and_irq() {
  const GameConfig config{};
  const GameHooks hooks{};
  LegacyGameRuntimeAdapter runtime(config, hooks);
  auto game = std::make_unique<Game>();
  const uint64_t before = game->timing.emulatedCpuTicks();
  runtime.pacePresentation(game->core, 1, 1);
  CHECK(game->timing.emulatedCpuTicks() > before);
  CHECK_EQ(game->core.mem_r32(kIStat) & 1u, 1u);
  CHECK_EQ(game->core.pending_work & Core::PW_IRQ, Core::PW_IRQ);
  game->core.mem_w32(kIStat, 0x7FEu);
  runtime.pacePresentation(game->core, 1, 2);
  CHECK_EQ(game->core.mem_r32(kIStat) & 1u, 0u);
  runtime.pacePresentation(game->core, 1, 2);
  CHECK_EQ(game->core.mem_r32(kIStat) & 1u, 1u);
}

void test_presented_field_wait_leaves_time_irq_and_fractional_phase_unchanged() {
  auto game = std::make_unique<Game>();
  const uint64_t before = game->timing.emulatedCpuTicks();
  gpu_wait_presented_fields(&game->core, 1, 2);
  CHECK_EQ(game->timing.emulatedCpuTicks(), before);
  CHECK_EQ(game->core.mem_r32(kIStat) & 1u, 0u);
  CHECK_EQ(game->core.pending_work & Core::PW_IRQ, 0u);

  gpu_pace_subframe_fields(&game->core, 1, 2);
  CHECK_EQ(game->core.mem_r32(kIStat) & 1u, 0u);
  const uint64_t partial = game->timing.emulatedCpuTicks();
  gpu_wait_presented_fields(&game->core, 2, 2);
  gpu_wait_presented_fields(&game->core, 2, 2);
  CHECK_EQ(game->timing.emulatedCpuTicks(), partial);
  CHECK_EQ(game->core.mem_r32(kIStat) & 1u, 0u);
  gpu_pace_subframe_fields(&game->core, 1, 2);
  CHECK_EQ(game->core.mem_r32(kIStat) & 1u, 1u);
  const uint64_t completed = game->timing.emulatedCpuTicks();
  gpu_wait_presented_fields(&game->core, 1, 1);
  CHECK_EQ(game->timing.emulatedCpuTicks(), completed);
  CHECK_EQ(game->core.mem_r32(kIStat) & 1u, 1u);
  CHECK_EQ(game->core.pending_work & Core::PW_IRQ, Core::PW_IRQ);
}

} // namespace

int main() {
  psx::config::cv_nopace.set(psx::config::Layer::Runtime, true);
  RUN(host_deadlines_belong_to_each_instance);
  RUN(unpaced_calls_preserve_the_instance_deadline);
  RUN(default_runtime_still_delivers_display_time_and_irq);
  RUN(presented_field_wait_leaves_time_irq_and_fractional_phase_unchanged);
  return pt_summary();
}

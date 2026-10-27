#include "frame_driver.h"

#include "core.h"
#include "game.h"
#include "lightrec_executor.h"
#include "logic_frame.h"
#include "runtime.h"

#include <lucent/log.h>

namespace psx::testprog {

void FrameDriver::stepFrame(Core &core, std::uint32_t frame) {
  psx::frame::LogicFrame::begin(game_, frame);
  const Facts &facts = session(core).facts;
  const psx::cpu::ExecutionBudget field = psx::cpu::ExecutionBudget::fromCycles(
      psx::frame::displayFieldCpuTicks(1, 1, psx::frame::fieldRateMilliHz(game_.timing.displayFieldRate())));
  for (int boundary = 0; boundary < kMaxFieldsPerFrame; ++boundary) {
    const psx::cpu::ExecutionResult result = core.lightrecExecutor().executeUntilExit(core.pc, field);
    if (result.reason == psx::cpu::ExecutionExitReason::BudgetExhausted) {
      continue;
    }
    if (result.reason != psx::cpu::ExecutionExitReason::FrameBoundary) {
      lucent::error("testprog",
                    "guest turn ended with {} at 0x{:08X}",
                    psx::cpu::executionExitName(result.reason),
                    result.guestPc);
      break;
    }
    // VSync returns to its caller.
    core.pc = core.r[psx::cpu::kRa];
    const std::uint32_t drawn = core.mem_r32(facts.frameCounter);
    if (drawn != committed_) {
      committed_ = drawn;
      game_.presentation.commit(&core, static_cast<int>(core.mem_r32(facts.frameFields)));
      game_.spu_audio.frame();
      return;
    }
  }
  lucent::error("testprog", "no frame drawn in {} fields; ending the run", kMaxFieldsPerFrame);
  game_.run.requestEnd();
  game_.presentation.commit(&core, 1);
}

} // namespace psx::testprog

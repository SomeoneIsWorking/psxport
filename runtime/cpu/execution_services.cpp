#include "execution_services.h"

#include "c_subsys.h"
#include "config_vars.h"
#include "core.h"
#include "game.h"
#include "host_turn.h"

#include <lucent/log.h>

namespace psx::cpu {
namespace {

std::uint64_t spinWindow() {
  static const std::uint64_t value = [] {
    const long configured = psx::config::cv_spin_ticks.get();
    return configured > 0 ? static_cast<std::uint64_t>(configured) : 0;
  }();
  return value;
}

int maximumSpinRun() {
  static const int value = static_cast<int>(psx::config::cv_spin_runs.get());
  return value;
}

} // namespace

void accountGuestInstructions(Core &core, std::uint32_t instructions) {
  core.game->timing.advanceGuestInstructionTicks(instructions);
  if (spin_detector_sample(
          core.spin, core.pc, (core.pending_work & Core::PW_HOST) != 0, instructions, spinWindow(), maximumSpinRun())) {
    watchdog_spin_fault(core.spin.anchor,
                        core.pc,
                        static_cast<unsigned long long>(spinWindow()) *
                            static_cast<unsigned long long>(maximumSpinRun()));
  }
}

void servicePendingWork(Core &core) {
  const bool reportRegisters = lucent::channel_on("pollregs");
  std::uint32_t before[11]{};
  if (reportRegisters) {
    before[0] = core.r[29];
    before[1] = core.r[30];
    before[2] = core.r[28];
    for (int index = 0; index < 8; ++index) {
      before[3 + index] = core.r[16 + index];
    }
  }

  if (core.pending_work & Core::PW_HOST) {
    serviceHostTurn(core);
  }
  if (core.pending_work & Core::PW_IRQ) {
    core.game->hle.irqPoll(&core);
  }

  if (!reportRegisters) {
    return;
  }
  static constexpr const char *names[11] = {"sp", "fp", "gp", "s0", "s1", "s2", "s3", "s4", "s5", "s6", "s7"};
  const std::uint32_t after[11] = {core.r[29],
                                   core.r[30],
                                   core.r[28],
                                   core.r[16],
                                   core.r[17],
                                   core.r[18],
                                   core.r[19],
                                   core.r[20],
                                   core.r[21],
                                   core.r[22],
                                   core.r[23]};
  for (int index = 0; index < 11; ++index) {
    if (after[index] != before[index]) {
      lucent::error("pollregs",
                    "pending-work service clobbered {} at pc=0x{:08X}: 0x{:08X} -> 0x{:08X}",
                    names[index],
                    core.pc,
                    before[index],
                    after[index]);
    }
  }
}

} // namespace psx::cpu

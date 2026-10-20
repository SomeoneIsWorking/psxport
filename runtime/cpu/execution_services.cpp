#include "execution_services.h"

#include "config_vars.h"
#include "core.h"
#include "game.h"
#include "r3000.h"
#include "side_effect_journal.h"
#include "watchdog.h"

#include <lucent/log.h>

#include <array>

namespace psx::cpu {
namespace {

// The callee-saved registers a pending-work service must leave as it found them.
inline constexpr std::array kPreservedAcrossService{kSp, kFp, kGp, kS0, kS1, kS2, kS3, kS4, kS5, kS6, kS7};

std::uint64_t spinWindow() {
  const long configured = psx::config::spin_ticks();
  return configured > 0 ? static_cast<std::uint64_t>(configured) : 0;
}

int maximumSpinRun() {
  return static_cast<int>(psx::config::spin_runs());
}

} // namespace

void accountGuestInstructions(Core &core, std::uint32_t instructions) {
  // Under the override differential only the live path advances guest time (side_effect_journal.h).
  if (core.sideEffectJournal != nullptr && core.sideEffectJournal->withholdsGuestTime()) {
    return;
  }
  core.game->timing.advanceGuestInstructionTicks(instructions);
  core.hostTurn.requestWhenDue(core);
  const psx::platform::SpinSample sample{
      .pc = core.pc, .hostStarved = (core.pending_work & Core::PW_HOST) != 0, .ticks = instructions};
  if (psx::platform::sampleSpinDetector(core.spin, sample, {.windowTicks = spinWindow(), .maxRun = maximumSpinRun()})) {
    psx::platform::spinFault(core.spin.anchor,
                             core.pc,
                             static_cast<unsigned long long>(spinWindow()) *
                                 static_cast<unsigned long long>(maximumSpinRun()));
  }
}

void servicePendingWork(Core &core) {
  if (core.sideEffectJournal != nullptr && !core.sideEffectJournal->admitPendingWork()) {
    return;
  }
  const bool reportRegisters = lucent::channel_on("pollregs");
  std::array<std::uint32_t, kPreservedAcrossService.size()> before{};
  if (reportRegisters) {
    for (std::size_t i = 0; i < kPreservedAcrossService.size(); ++i) {
      before[i] = core.r[kPreservedAcrossService[i]];
    }
  }

  if (core.pending_work & Core::PW_HOST) {
    core.hostTurn.service(core);
  }
  if (core.pending_work & Core::PW_IRQ) {
    core.game->hle.irqPoll(&core);
  }

  if (!reportRegisters) {
    return;
  }
  for (std::size_t i = 0; i < kPreservedAcrossService.size(); ++i) {
    const std::uint32_t after = core.r[kPreservedAcrossService[i]];
    if (after != before[i]) {
      lucent::error("pollregs",
                    "pending-work service clobbered {} at pc=0x{:08X}: 0x{:08X} -> 0x{:08X}",
                    gprName(kPreservedAcrossService[i]),
                    core.pc,
                    before[i],
                    after);
    }
  }
}

void handleBreak(Core & /*core*/, std::uint32_t code) {
  lucent::info("break", "code {}", code);
}

} // namespace psx::cpu

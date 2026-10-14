// WHERE THE BIOS SYNCHRONOUS-WAIT FIELDS LIVE.
//
// Extracted from `synchronous_task_wait.cpp` when adding one diagnostic argument pushed that file
// past its line cap. The cap exists to stop a file becoming the place new behaviour accumulates,
// and the honest response to hitting it is to extract the thing that already has its own name -
// not to shave a line or raise the cap. This is that thing: the resolved memory layout of the
// title's declared wait structure, read once from GameConfig and refused when incomplete, so every
// consumer of a wait field gets the same addresses or the same abort.
#pragma once

#include "core.h"
#include "legacy_game_config.h"

#include <lucent/log.h>

#include <cstdint>
#include <cstdlib>

namespace psx::sync_wait {

struct WaitLayout {
  uint32_t taskBase;
  uint32_t taskStride;
  uint32_t currentTask;
  uint32_t doneFlag;
  uint32_t param2;
  uint32_t param3;
  uint32_t taskGp;
  uint32_t forceCloseRa;
  uint32_t spawnRa;
  uint32_t finishRa;
};

WaitLayout waitLayout(const Core &core) {
  const GameConfig *config = core.cfg;
  if (!config || !config->taskTableBase || !config->taskSlotStride || config->taskCount < 3 || !config->curTaskPtr ||
      !config->syncWaitDoneFlag || !config->syncWaitParam2 || !config->syncWaitParam3 || !config->syncWaitTaskGp ||
      !config->syncWaitForceCloseRa || !config->syncWaitSpawnRa || !config->syncWaitFinishRa) {
    lucent::error("sched", "FATAL: synchronous task wait has no complete GameConfig scheduler layout");
    std::abort();
  }
  return {config->taskTableBase,
          config->taskSlotStride,
          config->curTaskPtr,
          config->syncWaitDoneFlag,
          config->syncWaitParam2,
          config->syncWaitParam3,
          config->syncWaitTaskGp,
          config->syncWaitForceCloseRa,
          config->syncWaitSpawnRa,
          config->syncWaitFinishRa};
}

} // namespace psx::sync_wait

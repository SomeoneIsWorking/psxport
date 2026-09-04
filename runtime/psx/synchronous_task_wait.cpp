#include "synchronous_task_wait.h"

#include "core.h"
#include "coro.h"
#include "execution_control.h"
#include "game.h"
#include "native_dispatch.h"
#include "pc_scheduler.h"
#include "scheduler.h"
#include <cstdlib>
#include <lucent/log.h>

namespace {

constexpr int kPumpLimit = 4096;

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

} // namespace

SyncWaitCompletion SynchronousTaskWait::finish(PcScheduler &scheduler, uint32_t taskBase, uint32_t flag) {
  SyncWaitCompletion result{};
  if (flag != 1) {
    Core &core = scheduler.game->core;
    result.stamp = static_cast<uint16_t>(core.hooks->schedRng(&core));
    core.mem_w16(taskBase + 0x56u, result.stamp);
    result.stamped = true;
  }
  lucent::debug("sched",
                "sync wait complete task=0x{:08X} flag={} stamped={} wait_ticks=0 "
                "loading_services=0",
                taskBase,
                flag,
                result.stamped ? 1 : 0);
  return result;
}

void SynchronousTaskWait::runSlot(PcScheduler &scheduler, int slot) {
  Core *core = &scheduler.game->core;
  const WaitLayout layout = waitLayout(*core);
  const uint32_t base = layout.taskBase + static_cast<uint32_t>(slot) * layout.taskStride;
  const uint32_t entry = core->mem_r32(base + 0x0Cu);
  const bool fresh = !scheduler.task_started[slot];

  const R3000 callerRegs = static_cast<R3000 &>(*core);
  const uint32_t callerTask = core->mem_r32(layout.currentTask);
  const int callerSlot = scheduler.cur_slot;
  const int callerCoro = scheduler.cur_is_coro;

  if (fresh) {
    scheduler.task_ctx[slot] = callerRegs;
    scheduler.task_ctx[slot].r[29] = core->mem_r32(base + 8u);
    scheduler.task_ctx[slot].r[31] = 0xDEAD0000u;
  }
  const uint32_t startPc = fresh ? entry : scheduler.task_ctx[slot].r[31];
  if (scheduler.coro[slot]) {
    delete scheduler.coro[slot];
    scheduler.coro[slot] = nullptr;
  }
  scheduler.task_started[slot] = 1;
  scheduler.native_fiber[slot] = 0;
  core->mem_w16(base, 4);
  core->mem_w32(layout.currentTask, base);
  scheduler.cur_slot = slot;
  scheduler.cur_is_coro = 1;
  static_cast<R3000 &>(*core) = scheduler.task_ctx[slot];
  lucent::debug("sched",
                "inline task slot {} {} pc=0x{:08X} sp=0x{:08X}",
                slot,
                fresh ? "start" : "resume",
                startPc,
                core->r[29]);

  Coro *coroutine = new Coro();
  scheduler.coro[slot] = coroutine;
  coroutine->start([core, startPc] {
    const auto result = psx::cpu::dispatchGuest(*core, startPc, psx::cpu::ExecutionBudget::currentTurn(*core));
    (void)psx::cpu::completeOrPropagate(*core, result);
  });
  int pumps = 0;
  while (true) {
    coroutine->resume();
    if (coroutine->done() || core->mem_r16(base) == 0) {
      break;
    }
    if (++pumps >= kPumpLimit) {
      lucent::error("sched",
                    "FATAL: synchronous task 0x{:08X} (slot {}) still parked "
                    "after {} resumes; "
                    "the remaining dependency must be ported synchronously",
                    entry,
                    slot,
                    pumps);
      std::abort();
    }
    core->mem_w16(base + 0x02u, 0);
    core->mem_w16(base, 2);
  }
  core->mem_w16(base, 0);

  scheduler.cur_is_coro = callerCoro;
  scheduler.cur_slot = callerSlot;
  delete coroutine;
  scheduler.coro[slot] = nullptr;
  scheduler.task_started[slot] = 0;
  scheduler.native_fiber[slot] = 0;
  core->mem_w32(layout.currentTask, callerTask);
  static_cast<R3000 &>(*core) = callerRegs;
}

void SynchronousTaskWait::run(PcScheduler &scheduler, uint32_t fn, uint32_t p2, uint32_t p3, uint32_t flag) {
  Core *core = &scheduler.game->core;
  const WaitLayout layout = waitLayout(*core);
  core->r[29] -= 40;
  core->mem_w32(core->r[29] + 24, core->r[18]);
  core->r[18] = fn;
  core->mem_w32(core->r[29] + 28, core->r[19]);
  core->r[19] = flag;
  core->mem_w32(core->r[29] + 20, core->r[17]);
  core->r[17] = p2;
  core->mem_w32(core->r[29] + 16, core->r[16]);
  core->r[16] = p3;
  core->mem_w32(core->r[29] + 32, core->r[31]);

  while (core->mem_r16(layout.taskBase + layout.taskStride) != 0) {
    runSlot(scheduler, 1);
  }

  core->r[4] = 2;
  core->r[31] = layout.forceCloseRa;
  scheduler.forceClose(2);
  core->mem_w8(layout.param2, static_cast<uint8_t>(core->r[17]));
  core->r[17] = layout.taskGp;
  core->mem_w8(layout.param3, static_cast<uint8_t>(core->r[16]));
  core->mem_w8(layout.doneFlag, 0);
  core->r[4] = 1;
  core->r[5] = core->r[18];
  core->r[31] = layout.spawnRa;
  scheduler.spawnPrim(core->r[4], core->r[5]);
  core->r[31] = layout.finishRa;
  finish(scheduler, core->mem_r32(layout.currentTask), core->r[19]);

  if (core->mem_r8(layout.doneFlag) == 0) {
    runSlot(scheduler, 1);
  }
  if (core->mem_r8(layout.doneFlag) == 0) {
    lucent::error("sched", "FATAL: synchronous task 0x{:08X} ended without raising the done flag", fn);
    std::abort();
  }

  core->r[31] = core->mem_r32(core->r[29] + 32);
  core->r[19] = core->mem_r32(core->r[29] + 28);
  core->r[18] = core->mem_r32(core->r[29] + 24);
  core->r[17] = core->mem_r32(core->r[29] + 20);
  core->r[16] = core->mem_r32(core->r[29] + 16);
  core->r[29] += 40;
}

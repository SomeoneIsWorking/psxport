#include "stock_libgpu.h"

#include "core.h"
#include "game.h"
#include "platform_hle.h"

namespace psx::hle {
namespace {

void timeoutArmEntry(Core *core) {
  core->game->platform_hle.libgpu()->armTimeout(*core);
}

} // namespace

void StockLibgpu::bind(Game &game) const & {
  PlatformHle &table = game.platform_hle;
  table.bindLibgpu(*this);
  table.bindStock(entries_.drawSync, PlatformHle::completeImmediately);
  table.bindStock(entries_.timeoutArm, timeoutArmEntry);
  table.bindStock(entries_.timeoutCheck, PlatformHle::completeImmediately);
}

void StockLibgpu::armTimeout(Core &core) const {
  if (entries_.timeoutDeadlineWord) {
    core.mem_w32(entries_.timeoutDeadlineWord, 0x7fffffffu);
  }
  if (entries_.timeoutFlagWord) {
    core.mem_w32(entries_.timeoutFlagWord, 0);
  }
}

} // namespace psx::hle

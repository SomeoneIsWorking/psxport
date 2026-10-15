#include "stock_libgte.h"

#include "core.h"
#include "game.h"
#include "platform_hle.h"
#include "proj_params.h"

namespace psx::hle {
namespace {

void setGeomOffsetEntry(Core *core) {
  StockLibgte::setGeomOffset(*core);
}

void setGeomScreenEntry(Core *core) {
  StockLibgte::setGeomScreen(*core);
}

} // namespace

void StockLibgte::bind(Game &game) const {
  game.platform_hle.bindStock(entries_.setGeomOffset, setGeomOffsetEntry);
  game.platform_hle.bindStock(entries_.setGeomScreen, setGeomScreenEntry);
}

void StockLibgte::setGeomOffset(Core &core) {
  const auto x = static_cast<std::int32_t>(core.r[psx::cpu::kA0]);
  const auto y = static_cast<std::int32_t>(core.r[psx::cpu::kA1]);
  core.r[psx::cpu::kA0] = static_cast<std::uint32_t>(x) << 16;
  core.r[psx::cpu::kA1] = static_cast<std::uint32_t>(y) << 16;
  psx::gte::setGeomOffset(core, x, y);
}

void StockLibgte::setGeomScreen(Core &core) {
  psx::gte::setGeomScreen(core, static_cast<std::int32_t>(core.r[psx::cpu::kA0]));
}

} // namespace psx::hle

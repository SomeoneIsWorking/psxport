#include "stock_libgte.h"

#include "core.h"
#include "game.h"
#include "platform_hle.h"
#include "proj_params.h"

namespace psx::hle {
namespace {

enum Register : int { A0 = 4, A1 = 5 };

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
  const auto x = static_cast<std::int32_t>(core.r[A0]);
  const auto y = static_cast<std::int32_t>(core.r[A1]);
  core.r[A0] = static_cast<std::uint32_t>(x) << 16;
  core.r[A1] = static_cast<std::uint32_t>(y) << 16;
  libgte_set_geom_offset(&core, x, y);
}

void StockLibgte::setGeomScreen(Core &core) {
  libgte_set_geom_screen(&core, static_cast<std::int32_t>(core.r[A0]));
}

} // namespace psx::hle

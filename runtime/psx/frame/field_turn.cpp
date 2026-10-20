#include "field_turn.h"

#include "core.h"
#include "dbg_server.h"
#include "game.h"
#include "watchdog.h"

#include <cstdlib>

#include <lucent/log.h>

namespace psx::frame {

void FieldTurn::beginField(Core &core) const {
  if (!core.game) {
    lucent::error("field-turn", "the field turn has no bound Game; refusing to pump host services");
    std::abort();
  }
  core.game->dbg_server.honourPause(&core);
  core.game->bugReport.atFieldBoundary(core);
  psx::platform::Watchdog::resume();
}

void FieldTurn::endField(Core &core, std::uint32_t frame) const {
  core.game->snapshots.tick(core, frame);
  core.game->dbg_server.service(&core);
}

} // namespace psx::frame

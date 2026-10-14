#include "logic_frame.h"

#include "core.h"
#include "game.h"

namespace psx::frame {

void LogicFrame::begin(Game &game, std::uint32_t frame) {
  game.timing.logicFrame = frame;
  game.core.rsub.otAttr.beginLogicFrame(frame);
  game.pad.serviceFrame();
}

} // namespace psx::frame

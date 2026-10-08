// Census rows are stamped with the logic frame; GpuState::s_frame counts presents.
#pragma once
#include "core.h"
#include "game.h"
#include <stdint.h>

namespace psx::frame {

// The number every producer-census row is stamped with. `Core &` because a census row exists only
// for a Core that is running a frame.
inline uint32_t censusFrame(Core &core) {
  return core.game->timing.logicFrame;
}
} // namespace psx::frame

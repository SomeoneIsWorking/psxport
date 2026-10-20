// Diagnostics keyed on the pad frame: PSXPORT_GUEST_POKE every frame, PSXPORT_PAD_SHOT_AT and PSXPORT_PAD_DUMP_AT.
#pragma once

#include "diagnostic_config.h"
#include "frame_schedule.h"

#include <cstdint>
#include <vector>

class Core;
class Game;

namespace psx::debug {

class PadFrameProbes {
public:
  // After the pad mask for `padFrame` is resolved.
  void service(Game &game, std::uint32_t padFrame);

private:
  void configure();

  bool configured_ = false;
  std::vector<psx::config::GuestPoke> pokes_;
  FrameSchedule shots_;
  FrameSchedule dumps_;
};

} // namespace psx::debug

#pragma once

#include <cstdint>

class Game;

namespace psx::frame {

// The host-side start of one title logic frame, called by a title's FrameDriver.
class LogicFrame {
public:
  // Publishes `frame` as the logic-frame clock, starts its producer-census span (without it OtAttr
  // attributes reused pool words to stale writers), then services the pad for the frame.
  static void begin(Game &game, std::uint32_t frame);
};

} // namespace psx::frame

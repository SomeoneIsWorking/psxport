// psx::FieldTurn: the per-field services around a title's frame step, in one order.
#pragma once

#include <cstdint>

class Core;

namespace psx::frame {

// Stateless, so `TitleSession` and the picker call the same two halves.
class FieldTurn {
public:
  // A paused field must not spend the watchdog budget, so the pause is answered before the re-arm.
  void beginField(Core &core) const;

  // The scheduled RAM capture, then one queued live-channel command.
  void endField(Core &core, std::uint32_t frame) const;
};

} // namespace psx::frame

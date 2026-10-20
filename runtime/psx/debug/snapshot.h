// Guest RAM captures: the one writer of main RAM plus its scratchpad sidecar.
#pragma once

#include "frame_schedule.h"
#include <cstdint>
#include <string>
#include <vector>
class Core;

namespace psx::debug {

// PSXPORT_SNAP_AT / SNAP_EVERY / SNAP_MAX schedule captures by field; SIGUSR1 asks for one at the next field.
class Snapshotter {
public:
  // Main RAM to `path` and the scratchpad to `path`.spad; true when both were written.
  static bool writeRam(const Core &core, const std::string &path);

  void tick(const Core &core, std::uint32_t frame);

private:
  void configure();
  void capture(const Core &core, std::uint32_t frame, const char *why);

  bool configured_ = false;
  FrameSchedule at_;
  std::uint32_t every_ = 0;
  std::uint32_t max_ = 0;
  std::uint32_t written_ = 0;
};

} // namespace psx::debug

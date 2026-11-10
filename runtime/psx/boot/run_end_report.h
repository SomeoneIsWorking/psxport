// The one report a finished run owes, whatever ended it.
#pragma once

#include <cstdint>

class Game;

namespace psx {

// Executor ledger, guest-call census, play-check counters, the title's replay and motion tracks, the
// PSXPORT_RAMDUMP capture, then the title's lines.
class RunEndReport {
public:
  explicit RunEndReport(Game &game) : game_(game) {}

  void emit(std::uint64_t productSteps) const;

private:
  Game &game_;
};

} // namespace psx

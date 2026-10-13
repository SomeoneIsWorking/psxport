// The one report a finished run owes, whatever ended it.
#pragma once

#include <cstdint>

class Game;

namespace psx {

// The executor counts and ledger, the guest-call census, the play-check counters, the after-loop RAM
// dump when asked, then the title's own lines (`GameRuntime::reportRun`).
class RunEndReport {
public:
  explicit RunEndReport(Game &game) : game_(game) {}

  void emit(std::uint64_t productSteps) const;

private:
  void dumpRamIfRequested() const;

  Game &game_;
};

} // namespace psx

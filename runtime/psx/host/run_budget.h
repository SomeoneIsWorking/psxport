// How long one run may go, in presentation-fence steps.
#pragma once

#include <cstdint>

namespace psx::config {
class Config;
}

namespace psx::host {

// What decides a run's cap, read once when the run starts.
struct RunBudgetInputs {
  std::int64_t explicitCap = 0; // PSXPORT_NATIVE_FRAMES; 0 = none asked
  bool padRecording = false;    // PSXPORT_PAD_REPLAY or PSXPORT_PAD_RESUME
  bool repl = false;            // PSXPORT_REPL drives the run with `run N`
  bool clientLive = false;      // PSXPORT_DEBUG_SERVER: a client steers the run and ends it with `quit`
  bool windowed = false;
};

// The cap for a whole process run, counted in presentation-fence steps: one per product step, since a
// step presents exactly once (FrameLoopShell enforces it), and one per selector frame. A panel's
// pre-roll is not counted; that is PreRollBudget's, which bounds wall time inside one selector frame.
class RunBudget {
public:
  // Headless with nothing else asked: a smoke run, bounded.
  static constexpr std::uint64_t kHeadlessDefaultSteps = 120;

  // An explicit cap wins; then a pad recording lifts the cap; then a live client or the REPL means
  // uncapped; then headless means kHeadlessDefaultSteps and windowed means uncapped.
  static RunBudget decide(const RunBudgetInputs &inputs);
  // The same decision from this process's configuration.
  static RunBudget fromConfig(const psx::config::Config &config);

  explicit RunBudget(std::uint64_t cap = 0) : cap_(cap) {}

  std::uint64_t cap() const {
    return cap_;
  }
  std::uint64_t steps() const {
    return steps_;
  }
  void step() {
    ++steps_;
  }
  bool exhausted() const {
    return cap_ != 0 && steps_ >= cap_;
  }

private:
  std::uint64_t cap_ = 0;
  std::uint64_t steps_ = 0;
};

} // namespace psx::host

// watchdog.h — the process frame-progress watchdog, crash and interrupt reporters.
#pragma once

#include <cstdint>

namespace psx::platform {

// Signal handlers carry no context, so the watchdog's state is process-wide; one RAII owner (ProductHost's
// process) arms it for a run, and every present, pause and resume reports through the static calls.
class Watchdog {
public:
  // Installs the fault and SIGINT/SIGTERM reporters and arms the frame timeout PSXPORT_WATCHDOG selects.
  Watchdog();
  ~Watchdog();
  Watchdog(const Watchdog &) = delete;
  Watchdog &operator=(const Watchdog &) = delete;

  // Forward progress; keeps the current boot or steady timeout phase.
  static void progress();
  // The main presenter completed a present: leave the boot grace and start steady timing.
  static void mainPresentComplete();
  // Resume after an intentional idle without claiming frame progress.
  static void resume();
  // Cancel the timeout during an intentional idle; no present re-arms it until resume() (pause, REPL
  // input wait, a bug-report form held open).
  static void suspend();
  // Permanently disarm (the divergence debugger pauses indefinitely).
  static void disable();
  static bool armed();
  // The guest position a timeout reports: the PC of the block last entered and the guest's 32 GPRs, both owned by
  // the executor until it forgets them.
  static void watchGuest(const volatile std::uint32_t *pc, const std::uint32_t *registers);
  static void forgetGuest(const volatile std::uint32_t *pc);
};

// The guest burned instruction time without yielding: report the region and exit 137.
[[noreturn]] void spinFault(unsigned anchor_pc, unsigned last_pc, unsigned long long instructions);

} // namespace psx::platform

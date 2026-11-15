// watchdog_host.h — the host mechanism under the watchdog: its timer, its crash and interrupt hooks, and the
// emergency stderr path its reports use from a handler.
#pragma once

#include <cstddef>

namespace psx::platform::watchdog_host {

struct Reporters {
  void (*expired)();
  // One line naming the fault, already formatted by the host.
  void (*fault)(const char *line, std::size_t length);
  void (*interrupt)();
};

// Installs the fault, interrupt and timer hooks; the timer stays idle until arm().
void install(const Reporters &reporters);
// Expires `seconds` from now, replacing any earlier deadline; 0 cancels.
void arm(unsigned seconds);
// Handler-safe: no allocation, no locks.
void writeError(const char *text, std::size_t length);
// Handler-safe host stack of the thread that hit the fault or deadline; nothing where the host cannot name it.
void writeBacktrace();

} // namespace psx::platform::watchdog_host

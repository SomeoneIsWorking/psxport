// Frame-progress watchdog: the host timer fires when no frame presents within N seconds, dumps the backtrace and
// aborts.
#include "watchdog.h"
#include "config_vars.h" // watchdog_seconds / watchdog_boot_seconds
#include "host_backtrace.h"
#include "watchdog_host.h"
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <lucent/log.h>
#include <span>

// SANCTIONED SIGNAL-HANDLER EXCEPTION: signal handlers receive no context, so the watchdog's
// state must be reachable from file scope. Per-run state lives in s_wd, the executor's guest position in s_guest;
// the sig_atomic_t fields and s_guest are the only ones a handler reads.
namespace {

namespace watchdog_host = psx::platform::watchdog_host;

struct {
  int secs;      // 0 => disabled
  int boot_secs; // generous grace until the main presenter completes cold initialization
  volatile sig_atomic_t armed;
  volatile sig_atomic_t main_present_ready; // 0 until the first main VRAM presentation completes
  volatile sig_atomic_t suspended;          // an intentional idle: presents during it must not re-arm
  volatile sig_atomic_t int_seen;           // SIGINT/SIGTERM re-entry latch
} s_wd;

// The executor's guest position, registered for the machine's lifetime rather than per run, so a run's reset of s_wd
// keeps it.
struct {
  const volatile uint32_t *volatile pc;
  const uint32_t *volatile registers;
} s_guest;

// Guest code runs as Lightrec blocks the C backtrace cannot name, so the report adds the guest's own position.
void writeGuestPosition() {
  const volatile uint32_t *pc = s_guest.pc;
  const uint32_t *registers = s_guest.registers;
  if (pc == nullptr || registers == nullptr) {
    return;
  }
  char line[] = "[watchdog] guest pc=00000000 ra=00000000 sp=00000000\n";
  const uint32_t values[3] = {*pc, registers[31], registers[29]};
  constexpr int kFirstDigit[3] = {20, 32, 44};
  for (int field = 0; field < 3; field++) {
    for (int digit = 0; digit < 8; digit++) {
      line[kFirstDigit[field] + digit] = "0123456789ABCDEF"[(values[field] >> (28 - 4 * digit)) & 0xFu];
    }
  }
  watchdog_host::writeError(line, sizeof(line) - 1);
}

void onExpired() {
  // SANCTIONED SIGNAL-HANDLER OUTPUT: Lucent, formatting, allocation, and stdio are not
  // async-signal-safe. Keep this emergency report restricted to fixed buffers, the host stderr path,
  // its allocation-free backtrace, and std::_Exit.
  constexpr char msg[] = "\n[watchdog] STUCK: no frame presented within the timeout — backtrace:\n";
  watchdog_host::writeError(msg, sizeof(msg) - 1);
  if (!s_wd.main_present_ready) {
    constexpr char hint[] = "[watchdog] (tripped before the main presenter became ready — likely "
                            "cold GPU initialization, not a hang; re-run, or raise "
                            "PSXPORT_WATCHDOG_BOOT)\n";
    watchdog_host::writeError(hint, sizeof(hint) - 1);
  }
  writeGuestPosition();
  watchdog_host::writeBacktrace();
  std::_Exit(134);
}

void onFault(const char *line, std::size_t length) {
  // See onExpired: this handler can use only the emergency stderr path.
  constexpr char msg[] = "\n[watchdog] FAULT: backtrace:\n";
  watchdog_host::writeError(msg, sizeof(msg) - 1);
  watchdog_host::writeError(line, length);
  watchdog_host::writeBacktrace();
  std::_Exit(139);
}

} // namespace

namespace psx::platform {

// SPIN DETECTOR fatal path (called from timing.cpp psx::platform::sampleSpinDetector). The guest burned a huge
// instruction budget without ever yielding to the host and without leaving one code region — the
// definition of a hang that would otherwise freeze the window with a dead close button. Report
// the region so the poll is findable, then abort. Exit code 137 distinguishes it from the
// frame-watchdog's 134 and faults' 139.
void spinFault(unsigned anchor_pc, unsigned last_pc, unsigned long long instructions) {
  lucent::error("spin",
                "FATAL: guest burned instruction time without yielding to the host — likely polling an "
                "unmodeled device/status; anchor pc=0x{:08X}, last pc=0x{:08X}, ~{} instructions",
                anchor_pc,
                last_pc,
                instructions);
  void *frames[64];
  const int count = psxport::host::captureBacktrace(frames, 64);
  psxport::host::emitBacktrace(
      lucent::Level::Error, "spin", std::span<void *const>(frames, static_cast<std::size_t>(count)));
  std::_Exit(137);
}

} // namespace psx::platform

namespace {

// SIGINT/SIGTERM (Ctrl+C / kill): a hung guest loop never returns to the windowing event
// pump, so the window's own close/Ctrl+C handling is dead and the process is unkillable from the
// UI. Install our OWN handler so Ctrl+C always force-exits IMMEDIATELY — and report where it was
// stuck (interp PC + C backtrace) on the way out, so a hang is diagnosable, not just killable.
// A second signal hard-kills in case anything in the handler wedges.
void onInterrupt() {
  if (s_wd.int_seen) {
    std::_Exit(130); // second Ctrl+C: bail without touching anything
  }
  s_wd.int_seen = 1;
  // See onExpired: this handler can use only the emergency stderr path.
  constexpr char msg[] = "\n[watchdog] INTERRUPT (SIGINT/SIGTERM) — where it was stuck:\n";
  watchdog_host::writeError(msg, sizeof(msg) - 1);
  writeGuestPosition();
  watchdog_host::writeBacktrace();
  std::_Exit(130);
}

} // namespace

namespace psx::platform {

// PSXPORT_WATCHDOG=<seconds> (0 disables).
Watchdog::Watchdog() {
  // A host that runs one Game after another arms the watchdog once per Game. Everything but the SIGNAL
  // DISPOSITIONS below is per-run state: `main_present_ready` left set by the previous Game's first
  // present would hand the next Game's cold GPU initialisation the 3 s steady budget instead of the
  // boot grace, and a disable() left armed=0 would be re-decided by the config below.
  s_wd = {};
  // A crash (SIGSEGV/SIGABRT) during boot should report WHERE (C backtrace names the guest_<addr>
  // guest call chain), not silently dump core — install the fault handler regardless of the setting.
  // A hung guest loop never pumps events, so an interrupt force-exits with the stuck PC.
  watchdog_host::install({onExpired, onFault, onInterrupt});

  // PSXPORT_WATCHDOG=0 disables it.
  s_wd.secs = (int)psx::config::watchdog_seconds();
  if (s_wd.secs <= 0) {
    return;
  }
  // A cold pipeline cache can stall the first present for seconds; unset (-1) grants max(secs, 45).
  const long boot = psx::config::watchdog_boot_seconds();
  s_wd.boot_secs = boot >= 0 ? (int)boot : (s_wd.secs > 45 ? s_wd.secs : 45);
  s_wd.armed = 1;
  watchdog_host::arm((unsigned)s_wd.boot_secs);
  lucent::info("watchdog",
               "armed: {}s frame-progress timeout ({}s grace until the main presenter is ready)",
               s_wd.secs,
               s_wd.boot_secs);
}

// Report forward progress without changing lifecycle phase. The SCEA and FMV image presenters use
// this because they can submit images before the main VRAM presenter has allocated its targets. Once
// that presenter is ready, the same call naturally uses the steady timeout, so later movies do not
// regain boot grace.
void Watchdog::progress() {
  if (s_wd.armed && !s_wd.suspended) {
    watchdog_host::arm((unsigned)(s_wd.main_present_ready ? s_wd.secs : s_wd.boot_secs));
  }
}

// Report a COMPLETED main-VRAM present — one beat per produced gameplay frame and the one-way
// cold-init -> steady transition. Calling this at entry is wrong: SDL/GPU initialization and the
// first full target allocation are part of this present and own boot_secs. Calling it for a simpler
// bootstrap image is equally wrong because that path does not materialise those targets.
void Watchdog::mainPresentComplete() {
  if (!s_wd.armed || s_wd.suspended) {
    return;
  }
  s_wd.main_present_ready = 1;
  watchdog_host::arm((unsigned)s_wd.secs);
}

// Resume after suspend without claiming that a frame completed. Before the first completed
// present this retains boot_secs; afterwards it re-arms the steady-state budget. The native frame loop
// calls this before work so a resumed step can still be diagnosed if it hangs.
void Watchdog::resume() {
  s_wd.suspended = 0;
  if (s_wd.armed) {
    watchdog_host::arm((unsigned)(s_wd.main_present_ready ? s_wd.secs : s_wd.boot_secs));
  }
}

// Suspend the frame-progress timeout during an INTENTIONAL idle where no frame is presented and that
// is NOT a hang: a debug-server PAUSE/step-wait, blocking on REPL stdin for the next command, or a
// bug-report form held open for the player. While suspended no present may re-arm it (a form still
// presents), and resume() re-arms it before work resumes. Keep s_armed so resume can re-arm.
void Watchdog::suspend() {
  s_wd.suspended = 1;
  if (s_wd.armed) {
    watchdog_host::arm(0);
  }
}

// Permanently disable the frame-progress watchdog. For the SBS divergence debugger, which PAUSES the
// process indefinitely on a divergence for live inspection — "no frame presented" is the intended state
// there, not a hang, and suspend (a one-shot alarm cancel) can't keep up with an open pause.
void Watchdog::disable() {
  s_wd.armed = 0;
  watchdog_host::arm(0);
}

bool Watchdog::armed() {
  return s_wd.armed != 0;
}

void Watchdog::watchGuest(const volatile std::uint32_t *pc, const std::uint32_t *registers) {
  s_guest.registers = registers;
  s_guest.pc = pc;
}

void Watchdog::forgetGuest(const volatile std::uint32_t *pc) {
  if (s_guest.pc == pc) {
    s_guest.pc = nullptr;
    s_guest.registers = nullptr;
  }
}

Watchdog::~Watchdog() {
  disable();
}

} // namespace psx::platform

// Frame-progress watchdog: SIGALRM fires when no frame presents within N seconds, dumps the backtrace and aborts.
#include "watchdog.h"
#include "config_vars.h" // watchdog_seconds / watchdog_boot_seconds
#include "host_backtrace.h"
#include <lucent/log.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#if !defined(__ANDROID__) && (defined(__GLIBC__) || defined(__linux__))
#include <execinfo.h>
#define HAVE_BACKTRACE 1
#endif

// SANCTIONED SIGNAL-HANDLER EXCEPTION: signal handlers receive no context, so the watchdog's
// state must be reachable from file scope. All of it lives in this ONE static struct; the
// sig_atomic_t fields are the only ones a handler reads.
namespace {

struct {
  int secs;      // 0 => disabled
  int boot_secs; // generous grace until the main presenter completes cold initialization
  volatile sig_atomic_t armed;
  volatile sig_atomic_t main_present_ready; // 0 until the first main VRAM presentation completes
  volatile sig_atomic_t int_seen;           // SIGINT/SIGTERM re-entry latch
} s_wd;

// During guest execution the C backtrace below names the
// guest_<addr> chain directly (the guest call stack), so there is no separate "interp PC" to
// report. (Core::pc holds the per-core guest fn, but a signal handler has no Core handle and the
// backtrace supersedes it.)

void onAlarm(int sig) {
  (void)sig;
  // SANCTIONED SIGNAL-HANDLER OUTPUT: Lucent, formatting, allocation, and stdio are not
  // async-signal-safe. Keep this emergency report restricted to fixed buffers, raw fd 2 writes,
  // the allocation-free fd backtrace formatter, and _exit.
  constexpr char msg[] = "\n[watchdog] STUCK: no frame presented within the timeout — backtrace:\n";
  write(2, msg, sizeof(msg) - 1);
  if (!s_wd.main_present_ready) {
    constexpr char hint[] = "[watchdog] (tripped before the main presenter became ready — likely "
                            "cold GPU initialization, not a hang; re-run, or raise "
                            "PSXPORT_WATCHDOG_BOOT)\n";
    write(2, hint, sizeof(hint) - 1);
  }
#ifdef HAVE_BACKTRACE
  void *bt[64];
  int n = backtrace(bt, 64);
  backtrace_symbols_fd(bt, n, 2); // async-signal-safe (unlike backtrace_symbols)
#endif
  _exit(134);
}

void onFault(int sig) {
  // See onAlarm: this handler can use only the emergency async-signal-safe fd 2 path.
  constexpr char msg[] = "\n[watchdog] FAULT (signal): backtrace:\n";
  write(2, msg, sizeof(msg) - 1);
  {
    char b[] = "[watchdog] signal = 00\n";
    b[20] = static_cast<char>('0' + (sig / 10) % 10);
    b[21] = static_cast<char>('0' + sig % 10);
    write(2, b, sizeof(b) - 1);
  }
#ifdef HAVE_BACKTRACE
  void *bt[64];
  int n = backtrace(bt, 64);
  backtrace_symbols_fd(bt, n, 2);
#endif
  _exit(139);
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
#ifdef HAVE_BACKTRACE
  void *bt2[64];
  int nbt = backtrace(bt2, 64);
  psxport::host::emitBacktrace(
      lucent::Level::Error, "spin", std::span<void *const>(bt2, static_cast<std::size_t>(nbt)).subspan(1));
#endif
  _exit(137);
}

} // namespace psx::platform

namespace {

// SIGINT/SIGTERM (Ctrl+C / kill): a hung guest loop never returns to the windowing event
// pump, so the window's own close/Ctrl+C handling is dead and the process is unkillable from the
// UI. Install our OWN handler so Ctrl+C always force-exits IMMEDIATELY — and report where it was
// stuck (interp PC + C backtrace) on the way out, so a hang is diagnosable, not just killable.
// A second signal hard-kills in case anything in the handler wedges.
void onInterrupt(int sig) {
  (void)sig;
  if (s_wd.int_seen) {
    _exit(130); // second Ctrl+C: bail without touching anything
  }
  s_wd.int_seen = 1;
  // See onAlarm: this handler can use only the emergency async-signal-safe fd 2 path.
  constexpr char msg[] = "\n[watchdog] INTERRUPT (SIGINT/SIGTERM) — where it was stuck:\n";
  write(2, msg, sizeof(msg) - 1);
#ifdef HAVE_BACKTRACE
  void *bt[64];
  int n = backtrace(bt, 64);
  backtrace_symbols_fd(bt, n, 2);
#endif
  _exit(130);
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
  struct sigaction fa = {};
  fa.sa_handler = onFault;
  sigaction(SIGSEGV, &fa, 0);
  sigaction(SIGABRT, &fa, 0);
  sigaction(SIGBUS, &fa, 0);

  // A hung guest loop never pumps events, so SIGINT/SIGTERM force-exit with the stuck PC.
  struct sigaction ia = {};
  ia.sa_handler = onInterrupt;
  sigaction(SIGINT, &ia, 0);
  sigaction(SIGTERM, &ia, 0);

  // PSXPORT_WATCHDOG=0 disables it.
  s_wd.secs = (int)psx::config::watchdog_seconds();
  if (s_wd.secs <= 0) {
    return;
  }
  // A cold pipeline cache can stall the first present for seconds; unset (-1) grants max(secs, 45).
  const long boot = psx::config::watchdog_boot_seconds();
  s_wd.boot_secs = boot >= 0 ? (int)boot : (s_wd.secs > 45 ? s_wd.secs : 45);
  struct sigaction sa = {};
  sa.sa_handler = onAlarm;
  sigaction(SIGALRM, &sa, 0);
  s_wd.armed = 1;
  alarm((unsigned)s_wd.boot_secs);
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
  if (s_wd.armed) {
    alarm((unsigned)(s_wd.main_present_ready ? s_wd.secs : s_wd.boot_secs));
  }
}

// Report a COMPLETED main-VRAM present — one beat per produced gameplay frame and the one-way
// cold-init -> steady transition. Calling this at entry is wrong: SDL/GPU initialization and the
// first full target allocation are part of this present and own boot_secs. Calling it for a simpler
// bootstrap image is equally wrong because that path does not materialise those targets.
void Watchdog::mainPresentComplete() {
  if (!s_wd.armed) {
    return;
  }
  s_wd.main_present_ready = 1;
  alarm((unsigned)s_wd.secs);
}

// Resume after suspend without claiming that a frame completed. Before the first completed
// present this retains boot_secs; afterwards it re-arms the steady-state budget. The native frame loop
// calls this before work so a resumed step can still be diagnosed if it hangs.
void Watchdog::resume() {
  if (s_wd.armed) {
    alarm((unsigned)(s_wd.main_present_ready ? s_wd.secs : s_wd.boot_secs));
  }
}

// Suspend the frame-progress timeout during an INTENTIONAL idle where no frame is presented and that
// is NOT a hang: a debug-server PAUSE/step-wait, or blocking on REPL stdin for the next command. The
// resume re-arms it before work resumes, and the next completed present resets it. Without
// this the 3s timeout fires on a deliberately paused/idle process. Keep s_armed so resume can re-arm.
void Watchdog::suspend() {
  if (s_wd.armed) {
    alarm(0);
  }
}

// Permanently disable the frame-progress watchdog. For the SBS divergence debugger, which PAUSES the
// process indefinitely on a divergence for live inspection — "no frame presented" is the intended state
// there, not a hang, and suspend (a one-shot alarm cancel) can't keep up with an open pause.
void Watchdog::disable() {
  s_wd.armed = 0;
  alarm(0);
}

bool Watchdog::armed() {
  return s_wd.armed != 0;
}

Watchdog::~Watchdog() {
  disable();
}

} // namespace psx::platform

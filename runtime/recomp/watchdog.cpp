// Frame-progress watchdog. The native boot loop can wedge (an interpreted task spinning on a
// condition that never becomes true, an infinite loop, etc.) without crashing — so an external
// `timeout` is the only way to stop it and it tells you nothing about WHERE. This arms a SIGALRM
// that fires if no frame has been presented within N seconds and dumps the current backtrace
// (the stuck call stack) before aborting. The main VRAM presenter owns the one-way transition from
// cold-init grace to steady timing. Bootstrap image presenters report progress without making that
// transition: they do not allocate the main presenter's per-Game targets or prove it is ready.
//
// Build note: backtrace symbol names need -rdynamic at link time (run.sh adds it). Without it
// you still get addresses — resolve with `addr2line -e scratch/bin/tomba2_port <addr>`.
#include "c_subsys.h"    // C linkage for watchdog_init/pet/suspend/disable (callers are C++ and vendored C)
#include "cfg.h"         // cfg_str — the CONFIG half of cfg.h; the logging half is retired
#include "config_vars.h" // cv_watchdog / cv_watchdog_boot — migrated onto the layered CVar system
#include <lucent/log.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#if defined(__GLIBC__) || defined(__linux__)
#include <execinfo.h>
#define HAVE_BACKTRACE 1
#endif

// SANCTIONED SIGNAL-HANDLER EXCEPTION: signal handlers receive no context, so the watchdog's
// state must be reachable from file scope. All of it lives in this ONE static struct; the
// sig_atomic_t fields are the only ones a handler reads.
static struct {
  int secs;      // 0 => disabled
  int boot_secs; // generous grace until the main presenter completes cold initialization
  volatile sig_atomic_t armed;
  volatile sig_atomic_t main_present_ready; // 0 until the first main VRAM presentation completes
  volatile sig_atomic_t int_seen;           // SIGINT/SIGTERM re-entry latch
} s_wd;

// The interpreter is gone (2026-06-30): under the recomp substrate the C backtrace below names the
// gen_func_<addr> chain directly (the guest call stack), so there is no separate "interp PC" to
// report. (Core::pc holds the per-core guest fn, but a signal handler has no Core handle and the
// backtrace supersedes it.)

static void on_alarm(int sig) {
  (void)sig;
  static const char msg[] = "\n[watchdog] STUCK: no frame presented within the timeout — backtrace:\n";
  write(2, msg, sizeof(msg) - 1);
  if (!s_wd.main_present_ready) {
    static const char hint[] = "[watchdog] (tripped before the main presenter became ready — likely "
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

static void on_fault(int sig) {
  static const char msg[] = "\n[watchdog] FAULT (signal): backtrace:\n";
  write(2, msg, sizeof(msg) - 1);
  {
    char b[] = "[watchdog] signal = 00\n";
    b[20] = '0' + (sig / 10) % 10;
    b[21] = '0' + sig % 10;
    write(2, b, sizeof(b) - 1);
  }
#ifdef HAVE_BACKTRACE
  void *bt[64];
  int n = backtrace(bt, 64);
  backtrace_symbols_fd(bt, n, 2);
#endif
  _exit(139);
}

// SIGINT/SIGTERM (Ctrl+C / kill): a hung interpreter loop never returns to the windowing event
// pump, so the window's own close/Ctrl+C handling is dead and the process is unkillable from the
// UI. Install our OWN handler so Ctrl+C always force-exits IMMEDIATELY — and report where it was
// stuck (interp PC + C backtrace) on the way out, so a hang is diagnosable, not just killable.
// A second signal hard-kills in case anything in the handler wedges.
static void on_interrupt(int sig) {
  (void)sig;
  if (s_wd.int_seen) {
    _exit(130); // second Ctrl+C: bail without touching anything
  }
  s_wd.int_seen = 1;
  static const char msg[] = "\n[watchdog] INTERRUPT (SIGINT/SIGTERM) — where it was stuck:\n";
  write(2, msg, sizeof(msg) - 1);
#ifdef HAVE_BACKTRACE
  void *bt[64];
  int n = backtrace(bt, 64);
  backtrace_symbols_fd(bt, n, 2);
#endif
  _exit(130);
}

// Enable with PSXPORT_WATCHDOG=<seconds> (0/unset disables). Call once at startup.
void watchdog_init(void) {
  // A crash (SIGSEGV/SIGABRT) during boot should report WHERE (C backtrace names the gen_func_<addr>
  // guest call chain), not silently dump core — install the fault handler regardless of the setting.
  struct sigaction fa = {};
  fa.sa_handler = on_fault;
  sigaction(SIGSEGV, &fa, 0);
  sigaction(SIGABRT, &fa, 0);
  sigaction(SIGBUS, &fa, 0);

  // Always make Ctrl+C / kill work + diagnostic, regardless of the frame-watchdog setting — a hung
  // interpreter loop otherwise leaves the window unclosable and SIGINT swallowed (the event pump
  // never runs). Our handler force-exits with the stuck PC.
  struct sigaction ia = {};
  ia.sa_handler = on_interrupt;
  sigaction(SIGINT, &ia, 0);
  sigaction(SIGTERM, &ia, 0);

  // Frame-progress watchdog. Default ON (3s) even when PSXPORT_WATCHDOG is unset, so a hang in a
  // windowed `./run.sh` self-aborts with a backtrace instead of wedging forever. A frame must take
  // well under a second, so 3s is already far past any healthy frame; gameplay pets every present
  // and never trips it. Explicit PSXPORT_WATCHDOG=0 disables it; set higher only for slow debugging.
  // MIGRATED to a CVar (runtime/recomp/config_vars.h). One DELIBERATE behaviour change, called out
  // here because "nothing may silently change meaning": this used to be `atoi(cfg_str(...))`, and
  // atoi("abc") is 0, so a TYPO in PSXPORT_WATCHDOG silently DISABLED the watchdog. It now falls
  // back to the declared default and the CVar binding logs a warn naming the bad value. A hang that
  // wedges forever because someone mistyped the timeout is not a behaviour worth preserving.
  s_wd.secs = (int)psx::config::cv_watchdog.get();
  if (s_wd.secs <= 0) {
    return;
  }
  // The FIRST main-VRAM frame is legitimately slow: RADV/AMD compiles every Vulkan pipeline (SSAO,
  // shadow map, tritex, present blit, …) on first use, so that present can block in the GPU fence
  // wait for several seconds on a cold shader cache (e.g. right after a full ./run.sh rebuild). That
  // is NOT a hang, so the 3s steady-state budget must not apply to it. Give cold initialization a much
  // larger grace (still finite, so a real first-frame GPU hang is still caught + a Ctrl+C works).
  // PSXPORT_WATCHDOG_BOOT overrides; default = max(s_secs, 45). The CVar's default is -1, NOT 0,
  // precisely so that an explicit PSXPORT_WATCHDOG_BOOT=0 still means zero rather than being
  // swallowed by the "derive it" sentinel — that is the one way this migration could have changed
  // an existing run's behaviour, so the sentinel is out of the value's range.
  const long boot = psx::config::cv_watchdog_boot.get();
  s_wd.boot_secs = boot >= 0 ? (int)boot : (s_wd.secs > 45 ? s_wd.secs : 45);
  struct sigaction sa = {};
  sa.sa_handler = on_alarm;
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
void watchdog_progress(void) {
  if (s_wd.armed) {
    alarm((unsigned)(s_wd.main_present_ready ? s_wd.secs : s_wd.boot_secs));
  }
}

// Report a COMPLETED main-VRAM present — one beat per produced gameplay frame and the one-way
// cold-init -> steady transition. Calling this at entry is wrong: SDL/GPU initialization and the
// first full target allocation are part of this present and own boot_secs. Calling it for a simpler
// bootstrap image is equally wrong because that path does not materialise those targets.
void watchdog_main_present_complete(void) {
  if (!s_wd.armed) {
    return;
  }
  s_wd.main_present_ready = 1;
  alarm((unsigned)s_wd.secs);
}

// Resume after watchdog_suspend without claiming that a frame completed. Before the first completed
// present this retains boot_secs; afterwards it re-arms the steady-state budget. The native frame loop
// calls this before work so a resumed step can still be diagnosed if it hangs.
void watchdog_resume(void) {
  if (s_wd.armed) {
    alarm((unsigned)(s_wd.main_present_ready ? s_wd.secs : s_wd.boot_secs));
  }
}

// Suspend the frame-progress timeout during an INTENTIONAL idle where no frame is presented and that
// is NOT a hang: a debug-server PAUSE/step-wait, or blocking on REPL stdin for the next command. The
// watchdog_resume re-arms it before work resumes, and the next completed present resets it. Without
// this the 3s timeout fires on a deliberately paused/idle process. Keep s_armed so resume can re-arm.
void watchdog_suspend(void) {
  if (s_wd.armed) {
    alarm(0);
  }
}

// Permanently disable the frame-progress watchdog. For the SBS divergence debugger, which PAUSES the
// process indefinitely on a divergence for live inspection — "no frame presented" is the intended state
// there, not a hang, and watchdog_suspend (a one-shot alarm cancel) can't keep up with an open pause.
void watchdog_disable(void) {
  s_wd.armed = 0;
  alarm(0);
}

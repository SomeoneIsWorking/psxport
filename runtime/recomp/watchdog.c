// Frame-progress watchdog. The native boot loop can wedge (an interpreted task spinning on a
// condition that never becomes true, an infinite loop, etc.) without crashing — so an external
// `timeout` is the only way to stop it and it tells you nothing about WHERE. This arms a SIGALRM
// that fires if no frame has been presented within N seconds and dumps the current backtrace
// (the stuck call stack) before aborting. Pet it from gpu_present (one beat per presented frame).
//
// Build note: backtrace symbol names need -rdynamic at link time (run.sh adds it). Without it
// you still get addresses — resolve with `addr2line -e scratch/bin/tomba2_port <addr>`.
#include <signal.h>
#include "cfg.h"
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#if defined(__GLIBC__) || defined(__linux__)
#include <execinfo.h>
#define HAVE_BACKTRACE 1
#endif

static int g_secs;       // 0 => disabled
static int g_boot_secs;  // generous grace for the FIRST presented frame (cold pipeline compile)
static volatile sig_atomic_t g_armed;
static volatile sig_atomic_t g_first_frame_done;  // 0 until the first present pets the watchdog

extern volatile uint32_t g_interp_pc;  // last PC the hybrid interpreter executed (interp.c)

static void on_alarm(int sig) {
  (void)sig;
  static const char msg[] = "\n[watchdog] STUCK: no frame presented within the timeout — backtrace:\n";
  write(2, msg, sizeof(msg) - 1);
  if (!g_first_frame_done) {
    static const char hint[] = "[watchdog] (tripped on the FIRST frame — likely cold GPU pipeline "
                               "compile, not a hang; re-run, or raise PSXPORT_WATCHDOG_BOOT)\n";
    write(2, hint, sizeof(hint) - 1);
  }
  // Report where the interpreter is spinning (async-signal-safe hex emit; no fprintf).
  { char b[] = "[watchdog] interp PC = 0x00000000\n"; uint32_t p = g_interp_pc;
    for (int i = 0; i < 8; i++) b[32 - i] = "0123456789abcdef"[(p >> (i * 4)) & 0xF];
    write(2, b, sizeof(b) - 1); }
#ifdef HAVE_BACKTRACE
  void* bt[64];
  int n = backtrace(bt, 64);
  backtrace_symbols_fd(bt, n, 2);              // async-signal-safe (unlike backtrace_symbols)
#endif
  _exit(134);
}

static void on_fault(int sig) {
  static const char msg[] = "\n[watchdog] FAULT (signal): backtrace:\n";
  write(2, msg, sizeof(msg) - 1);
  { char b[] = "[watchdog] signal = 00\n"; b[20] = '0' + (sig / 10) % 10; b[21] = '0' + sig % 10;
    write(2, b, sizeof(b) - 1); }
  { char b[] = "[watchdog] interp PC = 0x00000000\n"; uint32_t p = g_interp_pc;
    for (int i = 0; i < 8; i++) b[32 - i] = "0123456789abcdef"[(p >> (i * 4)) & 0xF];
    write(2, b, sizeof(b) - 1); }
#ifdef HAVE_BACKTRACE
  void* bt[64]; int n = backtrace(bt, 64); backtrace_symbols_fd(bt, n, 2);
#endif
  _exit(139);
}

// SIGINT/SIGTERM (Ctrl+C / kill): a hung interpreter loop never returns to the windowing event
// pump, so the window's own close/Ctrl+C handling is dead and the process is unkillable from the
// UI. Install our OWN handler so Ctrl+C always force-exits IMMEDIATELY — and report where it was
// stuck (interp PC + C backtrace) on the way out, so a hang is diagnosable, not just killable.
// A second signal hard-kills in case anything in the handler wedges.
static volatile sig_atomic_t g_int_seen;
static void on_interrupt(int sig) {
  (void)sig;
  if (g_int_seen) _exit(130);          // second Ctrl+C: bail without touching anything
  g_int_seen = 1;
  static const char msg[] = "\n[watchdog] INTERRUPT (SIGINT/SIGTERM) — where it was stuck:\n";
  write(2, msg, sizeof(msg) - 1);
  { char b[] = "[watchdog] interp PC = 0x00000000\n"; uint32_t p = g_interp_pc;
    for (int i = 0; i < 8; i++) b[32 - i] = "0123456789abcdef"[(p >> (i * 4)) & 0xF];
    write(2, b, sizeof(b) - 1); }
#ifdef HAVE_BACKTRACE
  void* bt[64]; int n = backtrace(bt, 64); backtrace_symbols_fd(bt, n, 2);
#endif
  _exit(130);
}

// Enable with PSXPORT_WATCHDOG=<seconds> (0/unset disables). Call once at startup.
void watchdog_init(void) {
  // A crash (SIGSEGV/SIGABRT) during boot should report WHERE (C backtrace + interpreter PC),
  // not silently dump core — install the fault handler regardless of the frame-watchdog setting.
  struct sigaction fa = {0};
  fa.sa_handler = on_fault;
  sigaction(SIGSEGV, &fa, 0);
  sigaction(SIGABRT, &fa, 0);
  sigaction(SIGBUS, &fa, 0);

  // Always make Ctrl+C / kill work + diagnostic, regardless of the frame-watchdog setting — a hung
  // interpreter loop otherwise leaves the window unclosable and SIGINT swallowed (the event pump
  // never runs). Our handler force-exits with the stuck PC.
  struct sigaction ia = {0};
  ia.sa_handler = on_interrupt;
  sigaction(SIGINT, &ia, 0);
  sigaction(SIGTERM, &ia, 0);

  // Frame-progress watchdog. Default ON (3s) even when PSXPORT_WATCHDOG is unset, so a hang in a
  // windowed `./run.sh` self-aborts with a backtrace instead of wedging forever. A frame must take
  // well under a second, so 3s is already far past any healthy frame; gameplay pets every present
  // and never trips it. Explicit PSXPORT_WATCHDOG=0 disables it; set higher only for slow debugging.
  const char* s = cfg_str("PSXPORT_WATCHDOG");
  g_secs = s ? atoi(s) : 3;
  if (g_secs <= 0) return;
  // The FIRST presented frame is legitimately slow: RADV/AMD compiles every Vulkan pipeline (SSAO,
  // shadow map, tritex, present blit, …) on first use, so the first present blocks in the GPU fence
  // wait for several seconds on a cold shader cache (e.g. right after a full ./run.sh rebuild). That
  // is NOT a hang, so the 3s steady-state budget must not apply to it. Give the first frame a much
  // larger grace (still finite, so a real first-frame GPU hang is still caught + a Ctrl+C works).
  // PSXPORT_WATCHDOG_BOOT overrides; default = max(g_secs, 45).
  const char* sb = cfg_str("PSXPORT_WATCHDOG_BOOT");
  g_boot_secs = sb ? atoi(sb) : (g_secs > 45 ? g_secs : 45);
  struct sigaction sa = {0};
  sa.sa_handler = on_alarm;
  sigaction(SIGALRM, &sa, 0);
  g_armed = 1;
  alarm((unsigned)g_boot_secs);
  fprintf(stderr, "[watchdog] armed: %ds frame-progress timeout (%ds grace for the first frame)\n",
          g_secs, g_boot_secs);
}

// Pet from the present path — one beat per produced frame. Re-arms the timer. The first present
// switches from the boot grace to the steady-state budget.
void watchdog_pet(void) {
  if (!g_armed) return;
  g_first_frame_done = 1;
  alarm((unsigned)g_secs);
}

// Suspend the frame-progress timeout during an INTENTIONAL idle where no frame is presented and that
// is NOT a hang: a debug-server PAUSE/step-wait, or blocking on REPL stdin for the next command. The
// next watchdog_pet (the next presented frame after resume) re-arms it. Without this the 3s timeout
// fires on a deliberately paused/idle process. (cancel any pending alarm; keep g_armed so pet re-arms.)
void watchdog_suspend(void) {
  if (g_armed) alarm(0);
}

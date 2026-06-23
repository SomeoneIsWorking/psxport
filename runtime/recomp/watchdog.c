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
static volatile sig_atomic_t g_armed;

extern volatile uint32_t g_interp_pc;  // last PC the hybrid interpreter executed (interp.c)

static void on_alarm(int sig) {
  (void)sig;
  static const char msg[] = "\n[watchdog] STUCK: no frame presented within the timeout — backtrace:\n";
  write(2, msg, sizeof(msg) - 1);
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
  struct sigaction sa = {0};
  sa.sa_handler = on_alarm;
  sigaction(SIGALRM, &sa, 0);
  g_armed = 1;
  alarm((unsigned)g_secs);
  fprintf(stderr, "[watchdog] armed: %ds frame-progress timeout\n", g_secs);
}

// Pet from the present path — one beat per produced frame. Re-arms the timer.
void watchdog_pet(void) {
  if (g_armed) alarm((unsigned)g_secs);
}

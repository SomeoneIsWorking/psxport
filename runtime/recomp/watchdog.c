// Frame-progress watchdog. The native boot loop can wedge (an interpreted task spinning on a
// condition that never becomes true, an infinite loop, etc.) without crashing — so an external
// `timeout` is the only way to stop it and it tells you nothing about WHERE. This arms a SIGALRM
// that fires if no frame has been presented within N seconds and dumps the current backtrace
// (the stuck call stack) before aborting. Pet it from gpu_present (one beat per presented frame).
//
// Build note: backtrace symbol names need -rdynamic at link time (run.sh adds it). Without it
// you still get addresses — resolve with `addr2line -e scratch/bin/tomba2_port <addr>`.
#include <signal.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#if defined(__GLIBC__) || defined(__linux__)
#include <execinfo.h>
#define HAVE_BACKTRACE 1
#endif

static int g_secs;       // 0 => disabled
static volatile sig_atomic_t g_armed;

static void on_alarm(int sig) {
  (void)sig;
  static const char msg[] = "\n[watchdog] STUCK: no frame presented within the timeout — backtrace:\n";
  write(2, msg, sizeof(msg) - 1);
#ifdef HAVE_BACKTRACE
  void* bt[64];
  int n = backtrace(bt, 64);
  backtrace_symbols_fd(bt, n, 2);              // async-signal-safe (unlike backtrace_symbols)
#endif
  _exit(134);
}

// Enable with PSXPORT_WATCHDOG=<seconds> (0/unset disables). Call once at startup.
void watchdog_init(void) {
  const char* s = getenv("PSXPORT_WATCHDOG");
  g_secs = s ? atoi(s) : 0;
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

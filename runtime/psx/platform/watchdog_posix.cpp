// watchdog_posix.cpp — watchdog_host over SIGALRM, fault and interrupt signals.
#include "watchdog_host.h"

#include <signal.h>
#include <unistd.h>

#if !defined(__ANDROID__) && (defined(__GLIBC__) || defined(__linux__))
#include <execinfo.h>
#define HAVE_BACKTRACE 1
#endif

namespace psx::platform::watchdog_host {

namespace {

// SANCTIONED SIGNAL-HANDLER EXCEPTION: a handler receives no context.
Reporters s_reporters;

void onAlarm(int) {
  s_reporters.expired();
}

void onFault(int sig) {
  char line[] = "[watchdog] signal = 00\n";
  line[20] = static_cast<char>('0' + (sig / 10) % 10);
  line[21] = static_cast<char>('0' + sig % 10);
  s_reporters.fault(line, sizeof(line) - 1);
}

void onInterrupt(int) {
  s_reporters.interrupt();
}

void handle(int sig, void (*handler)(int)) {
  struct sigaction action = {};
  action.sa_handler = handler;
  sigaction(sig, &action, nullptr);
}

} // namespace

void install(const Reporters &reporters) {
  s_reporters = reporters;
  handle(SIGSEGV, onFault);
  handle(SIGABRT, onFault);
  handle(SIGBUS, onFault);
  handle(SIGINT, onInterrupt);
  handle(SIGTERM, onInterrupt);
  handle(SIGALRM, onAlarm);
}

void arm(unsigned seconds) {
  alarm(seconds);
}

void writeError(const char *text, std::size_t length) {
  (void)!write(2, text, length);
}

void writeBacktrace() {
#ifdef HAVE_BACKTRACE
  void *frames[64];
  const int count = backtrace(frames, 64);
  backtrace_symbols_fd(frames, count, 2); // async-signal-safe, unlike backtrace_symbols
#endif
}

} // namespace psx::platform::watchdog_host

// watchdog_win32.cpp — watchdog_host over a timer thread, the unhandled-exception filter and the console
// control handler.
#include "watchdog_host.h"

#include <windows.h>

#include <csignal>
#include <cstdint>

namespace psx::platform::watchdog_host {

namespace {

// Process-wide like the POSIX signal state: the filter and control handler receive no context.
Reporters s_reporters;
HANDLE s_rearm = nullptr;
volatile LONG s_seconds = 0;
DWORD s_timerThread = 0;

DWORD WINAPI timerThread(void *) {
  for (;;) {
    const LONG seconds = s_seconds;
    const DWORD wait = seconds > 0 ? static_cast<DWORD>(seconds) * 1000u : INFINITE;
    if (WaitForSingleObject(s_rearm, wait) == WAIT_TIMEOUT) {
      s_reporters.expired();
    }
  }
}

void writeHex(char *digits, std::uint64_t value, int count) {
  for (int digit = 0; digit < count; ++digit) {
    digits[digit] = "0123456789ABCDEF"[(value >> (4 * (count - 1 - digit))) & 0xFu];
  }
}

LONG WINAPI onException(EXCEPTION_POINTERS *exception) {
  char line[] = "[watchdog] exception = 0x00000000\n";
  writeHex(line + 25, exception->ExceptionRecord->ExceptionCode, 8);
  s_reporters.fault(line, sizeof(line) - 1);
  return EXCEPTION_EXECUTE_HANDLER;
}

void onAbort(int) {
  constexpr char line[] = "[watchdog] abort\n";
  s_reporters.fault(line, sizeof(line) - 1);
}

BOOL WINAPI onConsoleControl(DWORD) {
  s_reporters.interrupt();
  return TRUE;
}

} // namespace

void install(const Reporters &reporters) {
  s_reporters = reporters;
  SetUnhandledExceptionFilter(onException);
  std::signal(SIGABRT, onAbort);
  SetConsoleCtrlHandler(onConsoleControl, TRUE);
  if (s_rearm == nullptr) {
    s_rearm = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    CloseHandle(CreateThread(nullptr, 0, timerThread, nullptr, 0, &s_timerThread));
  }
}

void arm(unsigned seconds) {
  InterlockedExchange(&s_seconds, static_cast<LONG>(seconds));
  SetEvent(s_rearm);
}

void writeError(const char *text, std::size_t length) {
  DWORD written = 0;
  WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, static_cast<DWORD>(length), &written, nullptr);
}

void writeBacktrace() {
  // A deadline expires on the timer thread, whose own stack says nothing about the stall.
  if (GetCurrentThreadId() == s_timerThread) {
    return;
  }
  void *frames[64];
  const USHORT count = RtlCaptureStackBackTrace(1, 64, frames, nullptr);
  const auto base = reinterpret_cast<std::uint64_t>(GetModuleHandleA(nullptr));
  char line[] = "[watchdog]   exe+0x0000000000000000\n";
  for (USHORT i = 0; i < count; ++i) {
    writeHex(line + 19, reinterpret_cast<std::uint64_t>(frames[i]) - base, 16);
    writeError(line, sizeof(line) - 1);
  }
}

} // namespace psx::platform::watchdog_host

// host_backtrace.cpp — see host_backtrace.h.
#include "host_backtrace.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(_WIN32)
#include <windows.h>
#elif !defined(__ANDROID__)
#include <execinfo.h>
#endif

namespace psxport::host {

namespace {

#if defined(_WIN32)
// No symbol table in a release binary: name each frame as module+offset.
std::string describeFrame(void *frame) {
  HMODULE module = nullptr;
  char name[MAX_PATH] = "?";
  if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         static_cast<LPCSTR>(frame),
                         &module) != 0) {
    GetModuleFileNameA(module, name, sizeof name);
  }
  const char *base = std::max(std::strrchr(name, '\\'), std::strrchr(name, '/'));
  const std::uintptr_t offset = reinterpret_cast<std::uintptr_t>(frame) - reinterpret_cast<std::uintptr_t>(module);
  return lucent::format("{}+0x{:x}", base != nullptr ? base + 1 : name, offset);
}
#endif

} // namespace

int captureBacktrace(void **frames, int capacity) {
#if defined(_WIN32)
  return RtlCaptureStackBackTrace(1, static_cast<DWORD>(capacity), frames, nullptr);
#elif defined(__ANDROID__)
  (void)frames;
  (void)capacity;
  return 0;
#else
  void *captured[65];
  const int count = ::backtrace(captured, std::min(capacity + 1, 65));
  const int kept = std::max(count - 1, 0);
  std::copy(captured + 1, captured + 1 + kept, frames);
  return kept;
#endif
}

void emitBacktrace(lucent::Level level, const char *channel, std::span<void *const> frames) {
  const int count = static_cast<int>(frames.size());
  lucent::Line trace;
  trace.add("host backtrace: {} frame(s) captured", count);
#if defined(_WIN32)
  for (int i = 0; i < count; ++i) {
    trace.add(" | [{}] {}", i, describeFrame(frames[static_cast<std::size_t>(i)]));
  }
#elif defined(__ANDROID__)
  for (int i = 0; i < count; ++i) {
    trace.add(" | [{}] {}", i, frames[static_cast<std::size_t>(i)]);
  }
#else
  char **symbols = ::backtrace_symbols(frames.data(), count);
  for (int i = 0; i < count; ++i) {
    trace.add(" | [{}] {}", i, symbols && symbols[i] ? symbols[i] : "?");
  }
  std::free(static_cast<void *>(symbols));
#endif
  trace.flush(level, channel);
}

void logBacktrace(int depth) {
  void *frames[32];
  const int count = captureBacktrace(frames, std::min(depth, 32));
  emitBacktrace(std::span<void *const>(frames, static_cast<std::size_t>(count)));
}

} // namespace psxport::host

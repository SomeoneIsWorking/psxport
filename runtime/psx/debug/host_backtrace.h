// host_backtrace.h — host call-stack capture for fault and watch reports.
#pragma once

#include <lucent/log.h>

#include <cstdlib>
#include <span>

#if !defined(__ANDROID__)
#include <execinfo.h>
#else
extern "C" inline int backtrace(void **frames, int capacity) {
  (void)frames;
  (void)capacity;
  return 0;
}
extern "C" inline char **backtrace_symbols(void *const *frames, int count) {
  (void)frames;
  (void)count;
  return nullptr;
}
#endif

namespace psxport::host {

inline int captureBacktrace(void **frames, int capacity) {
  return ::backtrace(frames, capacity);
}

inline void emitBacktrace(lucent::Level level, const char *channel, std::span<void *const> frames) {
  const int count = static_cast<int>(frames.size());
  lucent::Line trace;
  trace.add("host backtrace: {} frame(s) captured", count);
  char **symbols = ::backtrace_symbols(frames.data(), count);
  for (int i = 0; i < count; ++i) {
    trace.add(" | [{}] {}", i, symbols && symbols[i] ? symbols[i] : "?");
  }
  std::free(static_cast<void *>(symbols));
  trace.flush(level, channel);
}

inline void emitBacktrace(std::span<void *const> frames) {
  emitBacktrace(lucent::Level::Info, "host-backtrace", frames);
}

// Captures and logs up to 32 frames of the current host stack.
inline void logBacktrace(int depth) {
  void *frames[32];
  const int count = captureBacktrace(frames, depth < 32 ? depth : 32);
  emitBacktrace(std::span<void *const>(frames, static_cast<std::size_t>(count)));
}

} // namespace psxport::host

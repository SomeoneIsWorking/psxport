#pragma once

#include <lucent/log.h>

#include <cstdlib>

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

inline void emitBacktrace(lucent::Level level, const char *channel, void *const *frames, int count, int skip = 0) {
  lucent::Line trace;
  trace.add("host backtrace: {} frame(s) captured", count);
  char **symbols = ::backtrace_symbols(frames, count);
  for (int i = skip; i < count; ++i) {
    trace.add(" | [{}] {}", i, symbols && symbols[i] ? symbols[i] : "?");
  }
  std::free(symbols);
  trace.flush(level, channel);
}

inline void emitBacktrace(void *const *frames, int count) {
  emitBacktrace(lucent::Level::Info, "host-backtrace", frames, count);
}

} // namespace psxport::host

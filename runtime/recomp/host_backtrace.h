#pragma once

#include <lucent/log.h>

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
extern "C" inline void backtrace_symbols_fd(void *const *frames, int count, int fd) {
  (void)frames;
  (void)count;
  (void)fd;
  lucent::warn("host-backtrace", "host stack capture is unavailable on Android");
}
#endif

namespace psxport::host {

inline int captureBacktrace(void **frames, int capacity) {
#if defined(__ANDROID__)
  return ::backtrace(frames, capacity);
#else
  return ::backtrace(frames, capacity);
#endif
}

inline void emitBacktrace(void *const *frames, int count) {
#if defined(__ANDROID__)
  ::backtrace_symbols_fd(frames, count, 2);
#else
  ::backtrace_symbols_fd(frames, count, 2);
#endif
}

} // namespace psxport::host

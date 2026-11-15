// host_backtrace.h — host call-stack capture for fault and watch reports.
#pragma once

#include <lucent/log.h>

#include <span>

namespace psxport::host {

// Fills frames with the caller's stack, its own frame excluded; 0 where the host has no unwinder.
int captureBacktrace(void **frames, int capacity);

void emitBacktrace(lucent::Level level, const char *channel, std::span<void *const> frames);

inline void emitBacktrace(std::span<void *const> frames) {
  emitBacktrace(lucent::Level::Info, "host-backtrace", frames);
}

// Captures and logs up to 32 frames of the current host stack.
void logBacktrace(int depth);

} // namespace psxport::host

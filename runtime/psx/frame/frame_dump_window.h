#pragma once

#include <cstdint>

inline bool frame_dump_window_contains(uint64_t fence, long first_fence) {
  return first_fence <= 0 || fence >= static_cast<uint64_t>(first_fence);
}

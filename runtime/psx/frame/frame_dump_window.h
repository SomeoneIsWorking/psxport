#pragma once

#include <cstdint>

namespace psx::frame {

inline bool frameDumpWindowContains(uint64_t fence, long first_fence) {
  return first_fence <= 0 || fence >= static_cast<uint64_t>(first_fence);
}
} // namespace psx::frame

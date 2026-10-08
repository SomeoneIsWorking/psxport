// Read answers stay one line, so a request past the cap is cut short; callers must not read it as complete.
#pragma once

#include <cstdint>

namespace psx::control {

// Words (32-bit) one `rw`-style read may return on a single line. 64 words = 256 bytes, which is what a
// line-oriented protocol can carry without a reader having to reassemble.
inline constexpr std::uint32_t kMaxControlReadWords = 64;

} // namespace psx::control

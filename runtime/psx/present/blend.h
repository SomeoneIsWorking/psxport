// blend.h — the integer blends a render moves its saved inputs with.
#pragma once

#include <cmath>
#include <cstdint>

namespace psx::present {

// `t` of the way from `from` to `to`, rounded to the nearest integer.
inline std::int32_t lerpInt(std::int32_t from, std::int32_t to, float t) {
  return from + static_cast<std::int32_t>(std::lround(static_cast<double>(to - from) * static_cast<double>(t)));
}

// Halfword `half` (0 low, 1 high) of two words, blended, as an unsigned halfword.
inline std::uint32_t lerpHalf(std::uint32_t from, std::uint32_t to, unsigned half, float t) {
  const auto a = static_cast<std::int16_t>(from >> (half * 16u));
  const auto b = static_cast<std::int16_t>(to >> (half * 16u));
  return static_cast<std::uint16_t>(lerpInt(a, b, t));
}

// A word of two s16 halves blended half by half.
inline std::uint32_t lerpHalves(std::uint32_t from, std::uint32_t to, float t) {
  return lerpHalf(from, to, 0, t) | lerpHalf(from, to, 1, t) << 16;
}

} // namespace psx::present

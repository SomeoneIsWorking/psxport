// blend.h — the integer blends a render moves its saved inputs with.
#pragma once

#include <cmath>
#include <cstdint>

namespace psx::present {

// `t` of the way from `from` to `to`, rounded to the nearest integer.
inline std::int32_t lerpInt(std::int32_t from, std::int32_t to, float t) {
  return from + static_cast<std::int32_t>(std::lround(static_cast<double>(to - from) * static_cast<double>(t)));
}

enum class Halfword : unsigned { Low = 0, High = 16 };

// One halfword of two words, blended, as an unsigned halfword.
inline std::uint32_t lerpHalf(std::uint32_t from, std::uint32_t to, Halfword half, float t) {
  const auto shift = static_cast<unsigned>(half);
  return static_cast<std::uint16_t>(
      lerpInt(static_cast<std::int16_t>(from >> shift), static_cast<std::int16_t>(to >> shift), t));
}

// A word of two s16 halves blended half by half.
inline std::uint32_t lerpHalves(std::uint32_t from, std::uint32_t to, float t) {
  return lerpHalf(from, to, Halfword::Low, t) | lerpHalf(from, to, Halfword::High, t) << 16;
}

} // namespace psx::present

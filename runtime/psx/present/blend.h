// blend.h — the integer blends a render moves its saved inputs with.
#pragma once

#include <cmath>
#include <cstdint>

namespace psx::present {

// `t` of the way from `from` to `to`, rounded to the nearest integer; the span is taken in 64 bits.
inline std::int32_t lerpInt(std::int32_t from, std::int32_t to, float t) {
  const std::int64_t span = static_cast<std::int64_t>(to) - from;
  return static_cast<std::int32_t>(from + std::llround(static_cast<double>(span) * static_cast<double>(t)));
}

// A guest word blended as a signed 32-bit value.
inline std::uint32_t lerpWord(std::uint32_t from, std::uint32_t to, float t) {
  return static_cast<std::uint32_t>(lerpInt(static_cast<std::int32_t>(from), static_cast<std::int32_t>(to), t));
}

// An angle of `bits` bits at `t` from `from` to `to` the short way round, wrapped into the turn.
inline std::uint32_t lerpTurn(std::uint32_t from, std::uint32_t to, unsigned bits, float t) {
  const std::uint32_t mask = (1u << bits) - 1u;
  auto step = static_cast<std::int32_t>((to - from) & mask);
  if (step > static_cast<std::int32_t>((mask >> 1) + 1u)) {
    step -= static_cast<std::int32_t>(mask + 1u);
  }
  return (from + static_cast<std::uint32_t>(lerpInt(0, step, t))) & mask;
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

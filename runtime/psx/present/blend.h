// blend.h — the blends a render moves its saved inputs with.
#pragma once

#include <cmath>
#include <cstdint>

namespace psx::present {

// `t` of the way from `from` to `to`, rounded to the nearest integer; the span is taken in 64 bits.
// A tie rounds the blended value to even, so an odd span at t = 0.5 favours neither end.
inline std::int32_t lerpInt(std::int32_t from, std::int32_t to, float t) {
  const double span = static_cast<double>(static_cast<std::int64_t>(to) - from);
  return static_cast<std::int32_t>(std::nearbyint(static_cast<double>(from) + span * static_cast<double>(t)));
}

// A guest word blended as a signed 32-bit value.
inline std::uint32_t lerpWord(std::uint32_t from, std::uint32_t to, float t) {
  return static_cast<std::uint32_t>(lerpInt(static_cast<std::int32_t>(from), static_cast<std::int32_t>(to), t));
}

// The step from `from` to `to` the short way round a ring of `period`: in (-period/2, period/2].
inline std::int32_t ringStep(std::int64_t from, std::int64_t to, std::int64_t period) {
  std::int64_t step = ((to - from) % period + period) % period;
  if (step > period / 2) {
    step -= period;
  }
  return static_cast<std::int32_t>(step);
}

// An angle of `bits` bits at `t` from `from` to `to` the short way round, wrapped into the turn.
inline std::uint32_t lerpTurn(std::uint32_t from, std::uint32_t to, unsigned bits, float t) {
  const std::uint32_t mask = (1u << bits) - 1u;
  const std::int32_t step = ringStep(from & mask, to & mask, std::int64_t{mask} + 1);
  const auto start = static_cast<std::int32_t>(from & mask);
  return static_cast<std::uint32_t>(lerpInt(start, start + step, t)) & mask;
}

// A halfword holding an angle of `bits` bits with whole turns above it, `t` of the way the short way round and
// carried into the turns, unrounded.
inline double turnedBetween(std::uint32_t from, std::uint32_t to, unsigned bits, float t) {
  const std::uint32_t mask = (1u << bits) - 1u;
  const std::int32_t step = ringStep(from & mask, to & mask, std::int64_t{mask} + 1);
  return static_cast<double>(from & 0xFFFFu) + static_cast<double>(step) * static_cast<double>(t);
}

// turnedBetween as the nearest halfword, a tie to even.
inline std::uint32_t lerpTurned(std::uint32_t from, std::uint32_t to, unsigned bits, float t) {
  return static_cast<std::uint32_t>(static_cast<std::int32_t>(std::nearbyint(turnedBetween(from, to, bits, t)))) &
         0xFFFFu;
}

// `t` of the way from `from` to `to` on a ring of `period`, the short way round, unrounded and wrapped into
// [0, period).
inline double lerpRing(std::int32_t from, std::int32_t to, std::int32_t period, float t) {
  const double at = std::fmod(from + static_cast<double>(ringStep(from, to, period)) * static_cast<double>(t), period);
  return at < 0.0 ? at + period : at;
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

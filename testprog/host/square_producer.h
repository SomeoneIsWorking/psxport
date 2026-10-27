// square_producer.h — the test program's one owned drawer, square_draw, as a producer: its saved state and its
// render at any t.
#pragma once

#include "state_producer.h"

#include <array>
#include <cstdint>

class Core;

namespace psx::testprog {

struct Facts;

class SquareProducer final : public psx::present::StateProducer {
public:
  // What square_draw drew from: the square's fields and the packet's five GP0 words.
  struct State {
    std::int32_t x = 0;
    std::int32_t y = 0;
    std::uint32_t angle = 0; // 1/64 turns
    std::array<std::uint32_t, 5> words{};
  };

  explicit SquareProducer(const std::array<std::int16_t, 17> &sineQuarter) : sineQuarter_(sineQuarter) {}

  void render(const psx::present::StateInterval &states, psx::present::PrimitiveSink &sink) const override;

  // The override around square_draw and this render, for the Game `core` belongs to.
  static void install(Core &core, const Facts &facts);

private:
  // The guest's table sine, between its 1/64-turn entries for a fractional angle.
  float sine(float angle) const;

  std::array<std::int16_t, 17> sineQuarter_;
};

} // namespace psx::testprog

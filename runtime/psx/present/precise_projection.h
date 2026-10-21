// precise_projection.h — the unrounded screen position of each point the GTE projects during one render,
// keyed by the packed SXY word it rounded to, so a rendered polygon's vertices keep their sub-pixel place.
#pragma once

#include "frame_record.h"
#include "gte.h"

#include <cstdint>
#include <vector>

namespace psx::present {

class PreciseProjection {
public:
  PreciseProjection();

  // Forgets every point recorded so far.
  void clear();
  void record(std::uint32_t sxy, float x, float y);
  // Each polygon vertex the GTE projected takes its fraction past the whole pixel it was rounded to. Points that
  // round to one word share the first one's place, so every polygon naming the word moves together.
  void refine(DrawPrimitive &primitive) const;

  // Records every point `gte` projects while it lives.
  class Capture {
  public:
    Capture(psx::cpu::Gte &gte, PreciseProjection &points);
    ~Capture();
    Capture(const Capture &) = delete;
    Capture &operator=(const Capture &) = delete;
    Capture(Capture &&) = delete;
    Capture &operator=(Capture &&) = delete;

  private:
    psx::cpu::Gte &gte_;
  };

private:
  struct Point {
    std::uint32_t sxy = 0;
    std::uint32_t generation = 0;
    float x = 0.0f;
    float y = 0.0f;
  };
  [[nodiscard]] const Point *find(std::uint32_t sxy) const;

  std::vector<Point> points_;
  std::uint32_t generation_ = 1;
};

} // namespace psx::present

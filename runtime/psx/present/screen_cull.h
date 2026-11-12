// The retail screen-edge cull: a primitive is kept when some corner is inside each axis, compared unsigned.
#pragma once

#include "guest_widescreen_projection.h"

#include <cstdint>

namespace psx::present {

// Guest screen columns [left, right) that reach the picture.
struct CullWindow {
  int left = 0;
  int right = 0;
};

// The record canvas's columns for a guest that draws `guestWidth` wide: [-M, guestWidth + M).
constexpr CullWindow recordCanvasColumns(const GuestProjectionPlan &plan, int guestWidth) {
  const int margin = plan.presentationHorizontalMargin;
  return {-margin, guestWidth + margin};
}

// A packed SXY halfword (`lhu` of a packet vertex), or a raw 16.16 GTE SX register.
enum class CornerDomain : std::uint8_t {
  Packed16,
  Fixed16_16,
};

// `sltiu`/`sltu` against the edge: a coordinate the GTE saturated negative wraps high and fails.
constexpr bool insideAxis(std::uint32_t coordinate, CornerDomain domain, int bound) {
  const std::uint32_t limit =
      domain == CornerDomain::Fixed16_16 ? static_cast<std::uint32_t>(bound) << 16 : static_cast<std::uint32_t>(bound);
  return coordinate < limit;
}

// Keep iff some corner X is inside [left, right) and some corner Y inside [0, rows). With left 0 and
// right the retail width this is the retail compare bit for bit; a negative left lets the margin in.
class ScreenCull {
public:
  constexpr ScreenCull(CullWindow columns, int rows) : mLeft(columns.left), mRight(columns.right), mRows(rows) {}

  constexpr int left() const {
    return mLeft;
  }
  constexpr int right() const {
    return mRight;
  }
  constexpr int rows() const {
    return mRows;
  }

  constexpr bool keepsX(const std::uint32_t *corners, int count, CornerDomain domain) const {
    for (int index = 0; index < count; ++index) {
      if (insideAxis(shifted(corners[index], domain), domain, mRight - mLeft)) {
        return true;
      }
    }
    return false;
  }

  constexpr bool keepsY(const std::uint32_t *corners, int count, CornerDomain domain) const {
    for (int index = 0; index < count; ++index) {
      if (insideAxis(corners[index], domain, mRows)) {
        return true;
      }
    }
    return false;
  }

  constexpr bool keeps(const std::uint32_t *xs, const std::uint32_t *ys, int count, CornerDomain domain) const {
    return keepsX(xs, count, domain) && keepsY(ys, count, domain);
  }

private:
  constexpr std::uint32_t shifted(std::uint32_t coordinate, CornerDomain domain) const {
    if (domain == CornerDomain::Fixed16_16) {
      return coordinate - (static_cast<std::uint32_t>(mLeft) << 16);
    }
    return (coordinate - static_cast<std::uint32_t>(mLeft)) & 0xFFFFu;
  }

  int mLeft;
  int mRight;
  int mRows;
};

} // namespace psx::present

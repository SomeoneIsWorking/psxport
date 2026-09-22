#include "fade_interpolation.h"

namespace psxport::fade {
namespace {

// One 0..255 channel resolved at t. The endpoints are the title's own quantised levels, so this
// ROUNDS rather than truncating: truncating a ramp that steps by 8 leaves the midpoint 4 below the
// arithmetic mean on every channel, which is a bias in one direction rather than a half-step.
unsigned char channel(unsigned char a, unsigned char b, float t) {
  const float value = static_cast<float>(a) + (static_cast<float>(b) - static_cast<float>(a)) * t;
  const int rounded = static_cast<int>(value + 0.5f);
  if (rounded < 0) {
    return 0;
  }
  if (rounded > 255) {
    return 255;
  }
  return static_cast<unsigned char>(rounded);
}

} // namespace

FadeState resolve(const FadeState &prev, const FadeState &cur, bool havePrev, float t) {
  // No `t >= 1` shortcut: at t = 1 the arithmetic below already yields cur exactly, so a branch for
  // it would be one no test could falsify. The real present resolves to the current level through
  // the same expression as every other factor rather than through a special case.
  if (!havePrev || prev.mode != cur.mode) {
    return cur;
  }
  FadeState out = cur;
  out.r = channel(prev.r, cur.r, t);
  out.g = channel(prev.g, cur.g, t);
  out.b = channel(prev.b, cur.b, t);
  return out;
}

void PresentFade::capture(const FadeState &current) {
  havePrevious_ = captures_ > 0;
  previous_ = current_;
  current_ = current;
  ++captures_;
}

FadeState PresentFade::resolveNow(const FadeState &current) const {
  return resolve(previous_, current, havePrevious_, factor_);
}

} // namespace psxport::fade

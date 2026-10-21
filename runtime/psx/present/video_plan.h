// video_plan.h — the internal-resolution scale as a pure function of the presentation sink.
#pragma once

#include <cmath>

namespace psx::present {

// A PSX display field; ires=N is N render samples per PSX scanline.
inline constexpr int PRESENT_NATIVE_LINES = 240;
// The display width assumed before the guest has programmed one.
inline constexpr int WIDE_REFERENCE_NATIVE_W = 320;

struct VideoInputs {
  int sinkW = 0;
  int sinkH = 0;
  int modsIres = 1; // Mods::ires: 0 = AUTO, 1..cap = fixed
  int iresCap = 1;  // the memory-budget cap computed by the caller
};

// AUTO takes the number of PSX fields the sink's height holds.
inline int iresScale(const VideoInputs &in) {
  const int cap = in.iresCap < 1 ? 1 : in.iresCap;
  int i = in.modsIres;
  if (i == 0) {
    i = static_cast<int>(std::lround(static_cast<double>(in.sinkH) / static_cast<double>(PRESENT_NATIVE_LINES)));
  }
  if (i < 1) {
    i = 1;
  }
  if (i > cap) {
    i = cap;
  }
  return i;
}

} // namespace psx::present

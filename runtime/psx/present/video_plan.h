#pragma once

#include <cmath>
#include <cstdint>
// video_plan — the internal-resolution scale as a pure function of the presentation sink, which
// exists in both legs (windowed: the drawable; headless: PSXPORT_PRESENT_SINK), so headless and
// windowed runs pick the same scale.

// A PSX display field; ires=N is N render samples per PSX scanline.
enum : std::uint8_t { PRESENT_NATIVE_LINES = 240 };

// The display width assumed before the guest has programmed one.
enum : std::uint16_t { WIDE_REFERENCE_NATIVE_W = 320 };

struct VideoInputs {
  int sinkW = 0;
  int sinkH = 0;
  int modsIres = 1; // Mods::ires: 0 = AUTO, 1..cap = fixed
  int iresCap = 1;  // the memory-budget cap computed by the caller
};

namespace psx::present {

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

// crop_band.h — the shape of crop widescreen (plan 0203): the 4:3 picture's band of full width and p/q of its rows
// fills the wide screen, and its 4:3 UI shows at p/q of the band's scale, centred.
#pragma once

#include "pixel_size.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>

namespace psx::present {

// The band's share of the 4:3 rows, p/q in lowest terms; q = 0: no crop.
struct CropBand {
  int p = 0;
  int q = 0;

  bool active() const {
    return q > 0 && p > 0 && p < q;
  }
  bool operator==(const CropBand &) const = default;
};

// A wider aspect over 4:3 keeps 4:3 / aspect of the rows; a sink shape is approximated by the nearest p/q with q at
// most kCropBandMaxQ, so the layer scales stay small whole multiples.
inline constexpr int kCropBandMaxQ = 8;

inline CropBand cropBandForShare(double share) {
  if (!(share > 0.0) || share >= 1.0) {
    return {};
  }
  CropBand best;
  double bestError = 1.0;
  for (int q = 2; q <= kCropBandMaxQ; q++) {
    const int p = static_cast<int>(std::lround(share * q));
    const double error = std::abs(share - static_cast<double>(p) / q);
    if (p > 0 && p < q && error < bestError - 1e-12) {
      best = {p, q};
      bestError = error;
    }
  }
  return best;
}

// The band of a sink `sink` wide picture: 4:3 / (width / height) of the rows.
inline CropBand cropBandForSink(PixelSize sink) {
  if (sink.width <= 0 || sink.height <= 0) {
    return {};
  }
  return cropBandForShare(4.0 * sink.height / (3.0 * sink.width));
}

// The crop target's unit m: the UI layer draws at p * m, the world at q * m, the smallest with the UI at least `scale`.
inline int cropUnit(const CropBand &band, int scale) {
  return band.active() ? (scale + band.p - 1) / band.p : 0;
}

} // namespace psx::present

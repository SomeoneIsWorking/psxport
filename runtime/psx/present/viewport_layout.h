// viewport_layout.h — a rectangle of a presented image, and the letterbox and cover rules.
#pragma once

namespace psx::present {

struct PaneRect {
  int x;
  int y;
  int w;
  int h;
};

inline PaneRect letterbox(int aspectWidth, int aspectHeight, int outputWidth, int outputHeight) {
  int width;
  int height;
  if (static_cast<long>(outputWidth) * aspectHeight >= static_cast<long>(outputHeight) * aspectWidth) {
    height = outputHeight;
    width = outputHeight * aspectWidth / aspectHeight;
  } else {
    width = outputWidth;
    height = outputWidth * aspectHeight / aspectWidth;
  }
  return {(outputWidth - width) / 2, (outputHeight - height) / 2, width, height};
}

// The aspect's rectangle that covers the whole output, centred; the overflow lies outside it.
inline PaneRect cover(int aspectWidth, int aspectHeight, int outputWidth, int outputHeight) {
  int width;
  int height;
  if (static_cast<long>(outputWidth) * aspectHeight >= static_cast<long>(outputHeight) * aspectWidth) {
    width = outputWidth;
    height = outputWidth * aspectHeight / aspectWidth;
  } else {
    height = outputHeight;
    width = outputHeight * aspectWidth / aspectHeight;
  }
  return {(outputWidth - width) / 2, (outputHeight - height) / 2, width, height};
}

} // namespace psx::present

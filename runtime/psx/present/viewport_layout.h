// viewport_layout.h — a rectangle of a presented image, and how a picture fits one.
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

// A source picture of an aspect placed in an output: where it lands and which part of the source shows.
struct PictureFit {
  PaneRect viewport; // in the output
  PaneRect source;   // in the source's pixels
};

// The whole source letterboxed into the output at an aspect.
inline PictureFit fitPicture(long long aspectWidth,
                             long long aspectHeight,
                             int sourceWidth,
                             int sourceHeight,
                             int outputWidth,
                             int outputHeight) {
  return {letterbox(static_cast<int>(aspectWidth), static_cast<int>(aspectHeight), outputWidth, outputHeight),
          PaneRect{0, 0, sourceWidth, sourceHeight}};
}

} // namespace psx::present

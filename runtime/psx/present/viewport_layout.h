#pragma once

struct PaneRect {
  int x;
  int y;
  int w;
  int h;
};

inline PaneRect pane_letterbox(int aspectWidth, int aspectHeight, int outputWidth, int outputHeight) {
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

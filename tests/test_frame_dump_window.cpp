#include "frame_dump_window.h"

int main() {
  if (!frame_dump_window_contains(7, 0) || !frame_dump_window_contains(7, -1)) {
    return 1;
  }
  if (frame_dump_window_contains(2074, 2075)) {
    return 2;
  }
  return frame_dump_window_contains(2075, 2075) && frame_dump_window_contains(2076, 2075) ? 0 : 3;
}

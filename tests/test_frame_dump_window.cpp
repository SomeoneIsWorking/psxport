#include "frame_dump_window.h"

int main() {
  if (!psx::frame::frameDumpWindowContains(7, 0) || !psx::frame::frameDumpWindowContains(7, -1)) {
    return 1;
  }
  if (psx::frame::frameDumpWindowContains(2074, 2075)) {
    return 2;
  }
  return psx::frame::frameDumpWindowContains(2075, 2075) && psx::frame::frameDumpWindowContains(2076, 2075) ? 0 : 3;
}

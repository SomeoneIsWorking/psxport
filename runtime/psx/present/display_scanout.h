#pragma once

namespace psxport::display {

// HOW MANY ROWS ARE PRESENTED, AND HOW MANY A CONSOLE WOULD SCAN OUT. They are not the same number
// and conflating them has cost a retracted defect report.
//
// A port that HLEs the guest's display setup never writes GP1(07), so the decoded display height
// keeps the framework's default while the game really scans fewer lines. `GameConfig::
// guestDisplayHeight` is the port stating the real count.
//
// A GUEST-SOURCED render path claims to show what the console showed, so it presents the declared
// count. A NATIVE render path owns its own frame and deliberately presents more (USER 2026-08-19:
// "PC is fine, oracle isn't") — the product is right to draw those rows.
//
// The scanned window starts at the top of the presented frame, because both begin at the same VRAM
// row. Measured on Tomba! 2 (2026-09-19): fitting the product's presented frame against the Beetle
// console reference's gives native_row = console_content_row * 1.008 - 8.5 over a 240-row frame
// whose reference counterpart carries 8 black rows top and bottom — unit scale, window at the top.
// Comparing the frames WITHOUT that crop read as a 92% pixel difference and an apparent 7-pixel
// vertical offset, neither of which existed.
struct Scanout {
  int presented; // rows this path puts on screen
  int scanned;   // rows of those a console would have scanned out, starting at row 0
};

// `displayHeight` is the decoded GP1(07) height (or the framework default when it was never
// written); `declaredGuestHeight` is GameConfig::guestDisplayHeight, or 0 when the port declares
// none; `nativePath` is whether a native renderer owns this frame.
constexpr Scanout scanout(int displayHeight, int declaredGuestHeight, bool nativePath) {
  if (declaredGuestHeight <= 0) {
    return {displayHeight, displayHeight};
  }
  const int presented = nativePath ? displayHeight : declaredGuestHeight;
  return {presented, declaredGuestHeight};
}

} // namespace psxport::display

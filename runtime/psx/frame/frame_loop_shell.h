// frame_loop_shell.h — framework-owned product-loop boundary.
#pragma once

#include <cstdint>

class Core;
class FrameDriver;
class Game;

// The framework owns iteration; the title owns one finite native frame step. A missing driver is a
// product-contract violation, never permission to dispatch a non-returning guest frame loop.
class FrameLoopShell {
public:
  // Run once after the title has installed its native overrides and before product boot or
  // stepping. This is the single owner of the driver + fatal guest-VSync product preflight.
  FrameDriver &prepareProduct(Game &game) const;
  FrameDriver &requireDriver(Game &game) const;
  void step(Core &core, uint32_t frame) const;
};

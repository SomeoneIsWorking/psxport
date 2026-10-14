// frame_loop_shell.h — framework-owned product-loop boundary.
#pragma once

#include <cstdint>

class Core;
class FrameDriver;
class Game;

namespace psx::frame {

// The framework owns iteration; the title owns one finite native frame step. A missing driver is a
// product-contract violation, never permission to dispatch a non-returning guest frame loop.
class FrameLoopShell {
public:
  // Run once after the title has installed its native overrides and before product boot or
  // stepping. This is the single owner of the driver + fatal guest-VSync product preflight.
  FrameDriver &prepareProduct(Game &game) const;
  FrameDriver &requireDriver(Game &game) const;
  void step(Core &core, uint32_t frame) const;
  // Point the process-global device bindings (GTE, projection, SPU, MDEC, XA) at `core`. `step` calls
  // it when the stepped Core changes; a session boot calls it so the next step knows what is bound.
  static void bindRunningCore(Core &core);

private:
  inline static Core *bound_ = nullptr;
};
} // namespace psx::frame

// frame_loop_shell.h — framework-owned product-loop boundary.
#pragma once

#include <cstdint>

class Core;
namespace psx::game {
class FrameDriver;
}
class Game;

namespace psx::frame {

// The framework owns iteration; the title owns one finite native frame step. A missing driver is a
// product-contract violation, never permission to dispatch a non-returning guest frame loop. One shell
// per process (psx::host::SessionServices), because the device bindings it tracks are process-global.
class FrameLoopShell {
public:
  // Run once after the title has installed its native overrides and before product boot or
  // stepping. This is the single owner of the driver + fatal guest-VSync product preflight.
  psx::game::FrameDriver &prepareProduct(Game &game) const;
  psx::game::FrameDriver &requireDriver(Game &game) const;
  // Rebinds the process-global devices only when `core` is not the Core bound last.
  void step(Core &core, uint32_t frame);
  // Point the process-global device bindings (GTE, projection, SPU, MDEC, XA) at `core`.
  void bindRunningCore(Core &core);
  // `core` is being destroyed; its Game releases the bindings, so the next step must rebind.
  void forgetCore(const Core &core);

private:
  const Core *bound_ = nullptr;
};
} // namespace psx::frame

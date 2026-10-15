#include "stock_vsync.h"

#include "core.h"
#include "execution_control.h"
#include "field_rate.h"
#include "game.h"
#include "gpu_device.h"
#include "platform_hle.h"

#include <cstdlib>
#include <lucent/log.h>

namespace psx::hle {

void StockVSync::bind(Game &game) const & {
  game.platform_hle.bindVSync(*this);
}

void StockVSync::serve(Core &core) const {
  if (static_cast<std::int32_t>(core.r[psx::cpu::kA0]) < 0) {
    if (!queryCounter_) {
      lucent::error("plat-hle", "VSync negative query at 0x{:08X} has no measured libetc field counter", entry_);
      std::abort();
    }
    core.r[psx::cpu::kV0] = core.mem_r32(queryCounter_);
    return;
  }
  // libetc returns the HBlanks since the last VBlank; the boundary completes a whole field.
  core.r[psx::cpu::kV0] = psx::frame::displayLinesPerField(core.gpuDevice().displayArea().pal);
  psx::cpu::requestExecutionExit(core, psx::cpu::ExecutionExitReason::FrameBoundary);
}

} // namespace psx::hle

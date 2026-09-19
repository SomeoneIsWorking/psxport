// GpuState's display-row resolution: which rows this path presents, and which of them a console
// would have scanned out. The policy itself is display_scanout.h; these resolve this GpuState's
// inputs into it and own the one-time report when a guest path presents fewer rows than decoded.
//
// Split out of gpu_native.cpp rather than raising that file's line cap.
// c_subsys.h leads, exactly as it does in gpu_native.cpp: it establishes the C linkage context the
// shared device declarations in core.h are declared under, and including core.h without it makes
// those declarations disagree with the ones the rest of the runtime saw.
#include "c_subsys.h"
#include "core.h"

#include "display_scanout.h"
#include "gpu_native_internal.h"
#include "legacy_game_config.h"

#include <lucent/log.h>

int GpuState::declaredGuestHeight(Core *core) {
  return (core->cfg && core->cfg->guestDisplayHeight) ? core->cfg->guestDisplayHeight : 0;
}

int GpuState::guestScanHeight(Core *core) const {
  return psxport::display::scanout(s_disp_h, declaredGuestHeight(core), true).scanned;
}

int GpuState::presentedHeight(Core *core) const {
  const int declared = declaredGuestHeight(core);
  const bool native = core->rsub.mode.path() == RenderPath::Native;
  const auto geometry = psxport::display::scanout(s_disp_h, declared, native);
  if (declared > 0 && !native && declared != s_disp_h) {
    static bool said = false;
    if (!said) {
      said = true;
      lucent::info("gpu",
                   "guest render path presents {} lines, not {} — GameConfig::guestDisplayHeight. "
                   "The extra rows are framebuffer this game never scans out.",
                   declared,
                   s_disp_h);
    }
  }
  return geometry.presented;
}

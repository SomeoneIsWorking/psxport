// Required Beetle GTE ABI hooks. The projection push reaches the bound register file's precise sink;
// the vendor's PGXP memory tracking is not enabled.
//
// MDFNSS_StateAction (the save-state chunk codec, mednafen/state.c) is NOT stubbed here any more:
// that stub answered 1 without reading a byte, which made every Beetle SPU/MDEC save state an empty
// section that a load would happily accept. The fork's own state.c is compiled now
// (cmake/psxport.cmake) and reached through psx::state::BeetleDeviceState.
#include "beetle_gte.h"

#include <cstdint>

extern "C" void PGXP_pushSXYZ2f(float x, float y, float, uint32_t sxy) {
  const GteRegs *regs = GTE_CurState();
  if (regs->preciseSink != nullptr) {
    regs->preciseSink(regs->preciseContext, sxy, x, y);
  }
}

extern "C" int PGXP_NCLIP_valid(uint32_t, uint32_t, uint32_t) {
  return 0;
}

extern "C" float PGXP_NCLIP(void) {
  return 0.0f;
}

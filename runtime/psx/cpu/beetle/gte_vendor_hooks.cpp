// Required Beetle GTE ABI hooks. The projection push keeps the bound register file's unrounded FIFO places;
// the vendor's value-keyed PGXP memory tracking is not enabled.
//
// MDFNSS_StateAction (the save-state chunk codec, mednafen/state.c) is NOT stubbed here any more:
// that stub answered 1 without reading a byte, which made every Beetle SPU/MDEC save state an empty
// section that a load would happily accept. The fork's own state.c is compiled now
// (cmake/psxport.cmake) and reached through psx::state::BeetleDeviceState.
#include "beetle_gte.h"

#include <cstdint>

extern "C" void PGXP_pushSXYZ2f(float x, float y, float, uint32_t) {
  GteRegs *regs = GTE_CurState();
  regs->places[0] = regs->places[1];
  regs->places[1] = regs->places[2];
  regs->places[2] = GtePlace{x, y};
}

extern "C" int PGXP_NCLIP_valid(uint32_t, uint32_t, uint32_t) {
  return 0;
}

extern "C" float PGXP_NCLIP(void) {
  return 0.0f;
}

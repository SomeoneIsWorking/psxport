// Required Beetle GTE ABI hooks. Native projection provenance is owned by ProjPrim;
// the vendor's optional value-keyed PGXP integration is not enabled.
//
// MDFNSS_StateAction (the save-state chunk codec, mednafen/state.c) is NOT stubbed here any more:
// that stub answered 1 without reading a byte, which made every Beetle SPU/MDEC save state an empty
// section that a load would happily accept. The fork's own state.c is compiled now
// (cmake/psxport.cmake) and reached through psx::state::BeetleDeviceState.
#include <cstdint>

extern "C" void PGXP_pushSXYZ2f(float, float, float, uint32_t) {}

extern "C" int PGXP_NCLIP_valid(uint32_t, uint32_t, uint32_t) {
  return 0;
}

extern "C" float PGXP_NCLIP(void) {
  return 0.0f;
}

// Required Beetle GTE ABI hooks. Native projection provenance is owned by ProjPrim;
// the vendor's optional value-keyed PGXP and savestate integrations are not enabled.
#include <cstdint>

extern "C" void PGXP_pushSXYZ2f(float, float, float, uint32_t) {}

extern "C" int PGXP_NCLIP_valid(uint32_t, uint32_t, uint32_t) {
  return 0;
}

extern "C" float PGXP_NCLIP(void) {
  return 0.0f;
}

extern "C" int MDFNSS_StateAction(void *, int, int, void *, const char *) {
  return 1;
}

// GTE (COP2) coprocessor, lifted from the Beetle GPL-2 fork (mednafen/psx/gte.c, compiled
// as-is). All the game's geometry — RTPS/RTPT projection, NCLIP, matrix ops, color/depth —
// flows through here; our previous stub was a no-op, so any 3D was inert. This adapts the
// recomp GTE interface (r3000.h) to Beetle's GTE_* API and provides faithful-first stubs for
// the few externs gte.c references (PGXP off, widescreen off, savestate unused) so the math
// matches the oracle exactly. The widescreen GTE-scale hack stays OFF here (wide60 tier later).
#include "r3000.h"
#include <stdint.h>
#include <stdbool.h>

// Beetle GTE API (mednafen/psx/gte.h), declared locally to avoid pulling Beetle headers.
void     GTE_Init(void);
void     GTE_Power(void);
int32_t  GTE_Instruction(uint32_t instr);
void     GTE_WriteCR(unsigned which, uint32_t value);
void     GTE_WriteDR(unsigned which, uint32_t value);
uint32_t GTE_ReadCR(unsigned which);
uint32_t GTE_ReadDR(unsigned which);

// Externs gte.c references — faithful-first values.
bool     psx_gte_overclock = false;
uint8_t  widescreen_hack = 0;                   // GTE widescreen-scale hack OFF (faithful)
uint8_t  widescreen_hack_aspect_ratio_setting = 0;
uint32_t gMode = 0;                             // PGXP mode 0 = off
void  PGXP_pushSXYZ2f(float x, float y, float z, uint32_t v) { (void)x;(void)y;(void)z;(void)v; }
int   PGXP_NCLIP_valid(uint32_t a, uint32_t b, uint32_t c)   { (void)a;(void)b;(void)c; return 0; }
float PGXP_NCLIP(void)                                        { return 0.0f; }
int   MDFNSS_StateAction(void* st, int load, int data_only, void* sf, const char* name) {
  (void)st; (void)load; (void)data_only; (void)sf; (void)name; return 1;  // savestate unused
}

// Recomp GTE interface (r3000.h) -> Beetle GTE. mfc2/cfc2/mtc2/ctc2/lwc2/swc2 map to the
// data/control register ports; the COP2 ops map to GTE_Instruction.
uint32_t gte_read_data (uint32_t reg)            { return GTE_ReadDR(reg); }
void     gte_write_data(uint32_t reg, uint32_t v){ GTE_WriteDR(reg, v); }
uint32_t gte_read_ctrl (uint32_t reg)            { return GTE_ReadCR(reg); }
void     gte_write_ctrl(uint32_t reg, uint32_t v){ GTE_WriteCR(reg, v); }
void     gte_op(R3000* c, uint32_t insn)         { (void)c; GTE_Instruction(insn); }
void     gte_init(void)                          { GTE_Init(); GTE_Power(); }

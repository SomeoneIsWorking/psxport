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
// --- Widescreen RE tool (journal later-55): histogram the projected screen-X the GTE produces, to
// learn whether world geometry is projected beyond the 320 display window. Result: ~14% of verts
// land outside [0,320) (near bands [-64,0)≈3%, [320,384)≈11%) — the GTE DOES project wide geometry,
// but VRAM packing (textures abut the FB) blocks drawing it. Gated on PSXPORT_WS_SXHIST; accumulates
// and gpu_present dumps it every 500 frames. Reads SXY-FIFO (DR12/13/14) after RTPS/RTPT.
#include <stdio.h>
#include <stdlib.h>
static long s_sx_hist[16];   // buckets of 64px from -256..+704 (display is [0,320))
static long s_sx_n, s_sx_oob_lo, s_sx_oob_hi;
static int  s_sxhist_on = -1;
static void ws_sx_record(void) {
  if (s_sxhist_on < 0) s_sxhist_on = getenv("PSXPORT_WS_SXHIST") ? 1 : 0;
  if (!s_sxhist_on) return;
  for (unsigned r = 12; r <= 14; r++) {
    int16_t sx = (int16_t)(GTE_ReadDR(r) & 0xFFFF);
    s_sx_n++;
    if (sx < 0)   s_sx_oob_lo++;
    if (sx >= 320) s_sx_oob_hi++;
    int b = (sx + 256) / 64; if (b < 0) b = 0; if (b > 15) b = 15;
    s_sx_hist[b]++;
  }
}
void ws_sx_dump(const char* tag) {
  if (s_sxhist_on != 1 || s_sx_n == 0) return;
  fprintf(stderr, "[ws_sxhist] %s n=%ld  below0=%ld(%.1f%%)  atOrAbove320=%ld(%.1f%%)\n",
          tag, s_sx_n, s_sx_oob_lo, 100.0*s_sx_oob_lo/s_sx_n, s_sx_oob_hi, 100.0*s_sx_oob_hi/s_sx_n);
  for (int b = 0; b < 16; b++)
    fprintf(stderr, "  [%5d..%5d) %ld\n", b*64-256, b*64-256+64, s_sx_hist[b]);
  for (int b = 0; b < 16; b++) s_sx_hist[b] = 0;
  s_sx_n = s_sx_oob_lo = s_sx_oob_hi = 0;
}

void     gte_op(R3000* c, uint32_t insn)         { (void)c; GTE_Instruction(insn);
                                                   unsigned op = insn & 0x3F;
                                                   if (op == 0x01 || op == 0x30) ws_sx_record(); }
void     gte_init(void)                          { GTE_Init(); GTE_Power(); }

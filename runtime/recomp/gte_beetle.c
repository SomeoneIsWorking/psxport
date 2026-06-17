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

// --- PGXP subpixel cache (vertex-smoothing / PSX-wobble fix) -------------------------------------
// PSX projected vertices to INTEGER screen coords, so 3D geometry jitters ("wobbles") as the
// camera/object moves — the classic PS1 look. Beetle's GTE already computes the SUBPIXEL-precise
// projected x/y/z (gte.c TransformXY: precise_x/precise_y/precise_z) and hands them to this hook,
// keyed `v` = the packed integer SXY the game then copies verbatim into its GP0 vertex packets.
// We cache (precise_x,y,z) keyed by that packed int; the GPU tee (gpu_native.c) looks the precise
// coords back up by each vertex's integer (sx,sy) and rasterizes with FLOAT positions instead of
// the integer-snapped ones. This is value-keyed "PGXP-lite": on a key collision we simply fall
// back to the integer coords (a miss), so a wrong match can only ever cost smoothing, not correctness.
// The cache is reset each presented frame (pgxp_frame_reset) so a stale precise value from a prior
// frame can't be re-applied to a freshly-placed integer vertex (kills cross-frame wobble artifacts).
#define PGXP_BITS 15
#define PGXP_SIZE (1 << PGXP_BITS)
#define PGXP_MASK (PGXP_SIZE - 1)
typedef struct { uint32_t key; float x, y, z; uint8_t valid; } PgxpEnt;
static PgxpEnt s_pgxp[PGXP_SIZE];

static inline uint32_t pgxp_key(int sx, int sy) {
  // canonical 22-bit key = 11-bit signed sx | sy, matching the GP0 vertex word's usable range
  return ((uint32_t)(sy & 0x7FF) << 11) | (uint32_t)(sx & 0x7FF);
}
static inline uint32_t pgxp_slot(uint32_t key) {
  uint32_t h = key * 2654435761u;        // Knuth multiplicative hash
  return (h >> (32 - PGXP_BITS)) & PGXP_MASK;
}

void PGXP_pushSXYZ2f(float x, float y, float z, uint32_t v) {
  int sx = (int16_t)(v & 0xFFFF), sy = (int16_t)(v >> 16);
  uint32_t key = pgxp_key(sx, sy);
  PgxpEnt* e = &s_pgxp[pgxp_slot(key)];
  e->key = key; e->x = x; e->y = y; e->z = z; e->valid = 1;
}

// Renderer hook: fetch the subpixel coords for an integer vertex (sx,sy). Returns 1 on a cache hit.
int pgxp_lookup(int sx, int sy, float* px, float* py, float* pz) {
  uint32_t key = pgxp_key(sx, sy);
  PgxpEnt* e = &s_pgxp[pgxp_slot(key)];
  if (e->valid && e->key == key) {
    if (px) *px = e->x; if (py) *py = e->y; if (pz) *pz = e->z;
    return 1;
  }
  return 0;
}

void pgxp_frame_reset(void) {
  for (uint32_t i = 0; i < PGXP_SIZE; i++) s_pgxp[i].valid = 0;
}

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

extern int g_wide60_on;                            // wide60.c: gates the capture taps
void wide60_rtp(uint32_t op);                      // wide60.c: fold fingerprint + tag SXY->object

// --- GTE/lighting RE probe (PSXPORT_GTEPROBE=N) -------------------------------------------------
// Logs which GTE commands ACTUALLY execute (static call-site counts in generated/ aren't run-weighted)
// and snapshots the lighting/fog control registers, to pin down Tomba2's shading model. Finding so far:
// NO NC*/CC/CDP ops => no GTE dynamic per-vertex lighting; colors are baked + DPCS/DPCT depth-cue FOG.
static long s_gte_hist[64];
static int  s_gteprobe = -1, s_gteprobe_done = 0;
static const char* gte_name(unsigned fn) {
  switch (fn) {
    case 0x01:return"RTPS"; case 0x06:return"NCLIP"; case 0x0C:return"OP"; case 0x10:return"DPCS";
    case 0x11:return"INTPL"; case 0x12:return"MVMVA"; case 0x13:return"NCDS"; case 0x14:return"CDP";
    case 0x16:return"NCDT"; case 0x1B:return"NCCS"; case 0x1C:return"CC"; case 0x1E:return"NCS";
    case 0x20:return"NCT"; case 0x28:return"SQR"; case 0x29:return"DCPL"; case 0x2A:return"DPCT";
    case 0x2D:return"AVSZ3"; case 0x2E:return"AVSZ4"; case 0x30:return"RTPT"; case 0x3D:return"GPF";
    case 0x3E:return"GPL"; case 0x3F:return"NCCT"; default:return"?";
  }
}
void gte_probe_dump(const char* tag) {
  if (s_gteprobe <= 0) return;
  fprintf(stderr, "[gteprobe %s] executed GTE ops:\n", tag);
  for (unsigned fn = 0; fn < 64; fn++) if (s_gte_hist[fn])
    fprintf(stderr, "    %-6s(0x%02X) = %ld\n", gte_name(fn), fn, s_gte_hist[fn]);
  // lighting/fog control-register snapshot (cop2 CR numbering)
  fprintf(stderr, "  LightMatrix(LLM cr8-12): %08X %08X %08X %08X %08X\n",
          GTE_ReadCR(8),GTE_ReadCR(9),GTE_ReadCR(10),GTE_ReadCR(11),GTE_ReadCR(12));
  fprintf(stderr, "  BackColor(RBK/GBK/BBK cr13-15): %d %d %d\n",
          (int32_t)GTE_ReadCR(13),(int32_t)GTE_ReadCR(14),(int32_t)GTE_ReadCR(15));
  fprintf(stderr, "  LightColorMtx(LCM cr16-20): %08X %08X %08X %08X %08X\n",
          GTE_ReadCR(16),GTE_ReadCR(17),GTE_ReadCR(18),GTE_ReadCR(19),GTE_ReadCR(20));
  fprintf(stderr, "  FarColor(RFC/GFC/BFC cr21-23): %d %d %d   [fog target]\n",
          (int32_t)GTE_ReadCR(21),(int32_t)GTE_ReadCR(22),(int32_t)GTE_ReadCR(23));
  fprintf(stderr, "  DepthCue DQA(cr27)=%d DQB(cr28)=%d   [IR0 = DQB + DQA*h/sz -> fog factor]\n",
          (int16_t)GTE_ReadCR(27),(int32_t)GTE_ReadCR(28));
}
void     gte_op(R3000* c, uint32_t insn)         { (void)c; GTE_Instruction(insn);
                                                   unsigned op = insn & 0x3F;
                                                   if (s_gteprobe < 0) { const char* e = getenv("PSXPORT_GTEPROBE"); s_gteprobe = e ? atoi(e) : 0; }
                                                   if (s_gteprobe > 0) s_gte_hist[op]++;
                                                   if (op == 0x01 || op == 0x30) {
                                                     ws_sx_record();          // self-gated (PSXPORT_WS_SXHIST)
                                                     if (g_wide60_on) wide60_rtp(op);
                                                   } }
void     gte_init(void)                          { GTE_Init(); GTE_Power();
  // PSXPORT_WIDE is PC-native widescreen now: the GTE keeps its NATIVE projection (NO squish) and the
  // renderer (gpu_vk) re-centers the geometry into a wider scratch framebuffer at a true wider FOV.
  // The old emulator squish-X + display-stretch hack (widescreen_hack=1) is intentionally NOT used —
  // it loses horizontal resolution and stretches the 2D HUD (user rejected it). Keep the hack OFF. }
  widescreen_hack = 0; }

// gpu_native_internal.h — shared internals of the native GPU, split across translation units.
//
// gpu_native.c owns the rasterizer, GP0 parser, present/window and the canonical definitions of the
// shared state below. The auxiliary modules that were carved out of it — gpu_trace.c (GP0-stream
// capture for gpu_differ) and gpu_debug.c (scene/provenance dumps) — pull that state in through this
// header instead of everything living in one giant file. NOT a public API; internal to the GPU TUs.
#ifndef GPU_NATIVE_INTERNAL_H
#define GPU_NATIVE_INTERNAL_H
#include <stdio.h>
#include <stdint.h>

#define VRAM_W 1024
#define VRAM_H 512

extern uint16_t s_vram[VRAM_W * VRAM_H];                 // the GPU VRAM (textures + framebuffers)
static inline uint16_t* vram(int x, int y) { return &s_vram[(y & 511) * VRAM_W + (x & 1023)]; }

extern int      s_frame;                                 // present-frame counter
extern int      s_disp_x, s_disp_y;                      // VRAM top-left of the displayed region
extern uint32_t g_ot_madr;                               // last OT DMA root

uint32_t mem_r32(uint32_t);

// ---- Per-pixel primitive provenance (shared with gpu_debug.c) -------------------------------
typedef struct { uint32_t gid, frame, node; int clut_x, clut_y, tp_x, tp_y, x0, y0, u0, v0;
                 uint8_t op, r, g, b, semi, tex, mode, blend; } ProvMeta;
#define PROVRING 16384
extern uint32_t s_prov[VRAM_W * VRAM_H];                 // gid of last writer per pixel
extern ProvMeta s_provmeta[PROVRING];                    // gid -> prim details (ring)
extern int      s_prov_on;                               // 1 if provenance stamping is enabled

// ---- Diagnostic dumps (gpu_debug.c) ---------------------------------------------------------
void gpu_scene_dump(FILE* out, uint32_t madr);   // classify an OT's display list (PSXPORT_SCENEDUMP)

// ---- GP0-stream trace capture (gpu_trace.c) -------------------------------------------------
void trace_record(uint32_t w);   // record one GP0 word (no-op unless armed); called from gpu_gp0
void trace_flush(void);          // write the captured frame's trace; called from gpu_present_ex

#endif // GPU_NATIVE_INTERNAL_H

#include "core.h"
// gpu_debug.c — read-only diagnostic dumps of the native GPU state (carved out of gpu_native.c).
//
// These format human-readable views of the renderer's state for the debug tooling and the live debug
// server (dbg_server.c): per-pixel primitive PROVENANCE (which prim drew a displayed pixel) and the
// classified SCENE display list (poly/rect/fill/VRAM-copy/env accounting for an ordering table). They
// read the shared GPU state defined in gpu_native.c via gpu_native_internal.h; they never mutate VRAM.
#include "gpu_native_internal.h"
#include "game.h"
#include <stdio.h>
#include "cfg.h"

// Provenance query at an ABSOLUTE VRAM coord (the differ replays into the back buffer at off=(0,256),
// so query e.g. vram y = display y + 256 — no double-buffer confound, unlike the live-run PROVAT).
// Requires PSXPORT_PROVAT to be set so put_px_b stamped s_prov during replay.
void gpu_prov_dump(Core* core, int vx, int vy) {
  GpuState& g = core->game->gpu;
  uint16_t p = *g.vram(vx, vy);
  uint32_t gid = g.s_prov[(vy & 511) * VRAM_W + (vx & 1023)];
  ProvMeta* m = &g.s_provmeta[gid % PROVRING];
  CfgLine ln; cfg_line_reset(&ln);
  cfg_line_addf(&ln, "vram(%d,%d)=%04X rgb(%d,%d,%d) ", vx, vy, p,
                (p & 31) << 3, ((p >> 5) & 31) << 3, ((p >> 10) & 31) << 3);
  if (!gid) { cfg_line_addf(&ln, "<never written>"); cfg_line_flush(&ln, "prov"); return; }
  if (m->gid != gid) { cfg_line_addf(&ln, "gid=%u <evicted>", gid); cfg_line_flush(&ln, "prov"); return; }
  cfg_line_addf(&ln, "gid=%u op=%02X tex=%d texmode=%d semi=%d blend=%d clut=(%d,%d) tp=(%d,%d) primcol=(%d,%d,%d) v0=(%d,%d) uv0=(%d,%d)",
                gid, m->op, m->tex, m->mode, m->semi, m->blend, m->clut_x, m->clut_y, m->tp_x, m->tp_y,
                m->r, m->g, m->b, m->x0, m->y0, m->u0, m->v0);
  cfg_line_flush(&ln, "prov");
}

// Present-time provenance at DISPLAY coords (qx,qy): report, for the 7x7 box around it, which prim
// last wrote each displayed pixel (op/clut/texpage/color/age). Writes to `out`. Used by both the
// PSXPORT_PROVAT env path and the live debug server (dbg_server.c). Display space sidesteps the
// double-buffer offset. Requires provenance stamping (PSXPORT_PROVAT or gpu_provat_enable()).
void gpu_provat_display(Core* core, FILE* out, int qx, int qy) {
  GpuState& g = core->game->gpu;
  const int s_frame = g.s_frame, s_disp_x = g.s_disp_x, s_disp_y = g.s_disp_y;
  fprintf(out, "[provat] f%d display (%d,%d) +/-3  (disp@%d,%d)\n", s_frame, qx, qy, s_disp_x, s_disp_y);
  if (g.s_prov_on <= 0) { fprintf(out, "  (provenance was off; now enabled — re-query after a frame)\n");
                        g.s_prov_on = 1; return; }
  for (int dy = -3; dy <= 3; dy++) for (int dx = -3; dx <= 3; dx++) {
    int vx = s_disp_x + qx + dx, vy = s_disp_y + qy + dy;
    uint16_t p = *g.vram(vx, vy);
    uint32_t gid = g.s_prov[(vy & 511) * VRAM_W + (vx & 1023)];
    ProvMeta* m = &g.s_provmeta[gid % PROVRING];
    int valid = (m->gid == gid && gid != 0);
    fprintf(out, "  (%+d,%+d) vram(%d,%d)=%04X rgb(%d,%d,%d)", dx, dy, vx, vy, p,
            (p & 31) << 3, ((p >> 5) & 31) << 3, ((p >> 10) & 31) << 3);
    if (!gid) fprintf(out, "  <never written>\n");
    else if (!valid) fprintf(out, "  gid=%u <evicted: drawn long ago = STALE>\n", gid);
    else fprintf(out, "  gid=%u age=%dframes op=%02X tex=%d mode=%d semi=%d clut=(%d,%d) tp=(%d,%d) "
                 "primcol=(%d,%d,%d) node=%08X v0=(%d,%d) uv0=(%d,%d)\n",
                 gid, (int)((uint32_t)s_frame - m->frame), m->op, m->tex, m->mode, m->semi,
                 m->clut_x, m->clut_y, m->tp_x, m->tp_y, m->r, m->g, m->b, m->node,
                 m->x0, m->y0, m->u0, m->v0);
  }
}

// --- Native scene accounting (graphics OWNERSHIP) -----------------------------------------------
// Read-only walk of the same OT DrawOTag DMAs, classifying every primitive into engine-meaningful
// categories so the port can ACCOUNT for each draw (VRAM copies = reflection/fade buffers, fills,
// large/semi overlays = fade tiles, env). PSXPORT_SCENEDUMP=N. (later-99)
static int gp0_cmd_len(uint8_t op) {
  if (op >= 0x20 && op <= 0x3F) { int nv = (op & 0x08) ? 4 : 3, per = 1 + ((op & 0x04) ? 1 : 0) + ((op & 0x10) ? 1 : 0);
    return 1 + nv * per - ((op & 0x10) ? 1 : 0); }
  if (op >= 0x40 && op <= 0x5F) return 0;
  if (op >= 0x60 && op <= 0x7F) { int t = (op & 0x04) ? 1 : 0, sz = (op >> 3) & 3; return 1 + 1 + t + (sz == 0 ? 1 : 0); }
  if (op == 0x02) return 3;
  if (op >= 0x80 && op <= 0x9F) return 4;
  if (op >= 0xA0 && op <= 0xDF) return 3;
  return 1;
}
void gpu_scene_dump(Core* core, FILE* out, uint32_t madr) {
  const int s_frame = core->game->gpu.s_frame;
  uint32_t addr = madr & 0x1FFFFC;
  int npoly = 0, nrect = 0, nline = 0, nfill = 0, ncopy = 0, nup = 0, nenv = 0;
  fprintf(out, "[scene] f%d OT@0x%08X — classified display list:\n", s_frame, 0x80000000u | addr);
  for (int g = 0; g < 0x10000; g++) {
    uint32_t hdr = core->mem_r32(addr); int n = hdr >> 24, i = 0;
    while (i < n) {
      uint32_t c = core->mem_r32(addr + 4 + i * 4); uint8_t op = c >> 24;
      int len = gp0_cmd_len(op); if (len <= 0) break;
      uint32_t w1 = (i + 1 < n) ? core->mem_r32(addr + 4 + (i + 1) * 4) : 0;
      uint32_t w2 = (i + 2 < n) ? core->mem_r32(addr + 4 + (i + 2) * 4) : 0;
      if (op == 0x02) { nfill++; fprintf(out, "  FILL rgb=(%d,%d,%d) at(%d,%d) %dx%d\n",
          c&0xFF,(c>>8)&0xFF,(c>>16)&0xFF, w1&0x3FF,(w1>>16)&0x1FF, w2&0x3FF,(w2>>16)&0x1FF); }
      else if (op >= 0x80 && op <= 0x9F) { ncopy++; uint32_t w3 = (i+3<n)?core->mem_r32(addr+4+(i+3)*4):0;
        fprintf(out, "  COPY src(%d,%d)->dst(%d,%d) %dx%d [reflection/fade]\n",
          w1&0x3FF,(w1>>16)&0x1FF, w2&0x3FF,(w2>>16)&0x1FF, w3&0x3FF,(w3>>16)&0x1FF); }
      else if (op >= 0xA0 && op <= 0xBF) nup++;
      else if (op >= 0xE1 && op <= 0xE6) nenv++;
      else if (op >= 0x20 && op <= 0x3F) { npoly++;
        if (((op>>1)&1) && !((op>>2)&1)) fprintf(out, "  POLY semi flat rgb=(%d,%d,%d) [fade/overlay?]\n",
          c&0xFF,(c>>8)&0xFF,(c>>16)&0xFF); }
      else if (op >= 0x60 && op <= 0x7F) nrect++;
      else if (op >= 0x40 && op <= 0x5F) { nline++; break; }
      i += len;
    }
    uint32_t next = hdr & 0xFFFFFF; if (next == 0xFFFFFF || next == 0) break; addr = next & 0x1FFFFC;
  }
  fprintf(out, "[scene] f%d totals: poly=%d rect=%d line=%d fill=%d vramcopy=%d upload=%d env=%d\n",
          s_frame, npoly, nrect, nline, nfill, ncopy, nup, nenv);
}
// On-demand scene dump for the live debug server (dbg_server.c): classify the CURRENT frame's
// last-submitted OT (Gpu::s_ot_madr, set by gpu_dma2_linked_list) into `out`.
void gpu_scene_dump_now(Core* core, FILE* out) { gpu_scene_dump(core, out, core->game->gpu.s_ot_madr); }

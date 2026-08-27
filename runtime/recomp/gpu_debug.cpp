#include "cfg.h"
#include "core.h"
#include "gpu_vk.h"
#include "ot_attr.h" // OtAttr::Span — `otattr` packet->submitter attribution
// gpu_debug.c — read-only diagnostic dumps of the native GPU state (carved out of gpu_native.c).
//
// These format human-readable views of the renderer's state for the debug tooling and the live debug
// server (dbg_server.c): per-pixel primitive PROVENANCE (which prim drew a displayed pixel) and the
// classified SCENE display list (poly/rect/fill/VRAM-copy/env accounting for an ordering table). They
// read the shared GPU state defined in gpu_native.c via gpu_native_internal.h; they never mutate VRAM.
#include "game.h"
#include "gpu_native_internal.h"
#include "render_queue.h"
#include <algorithm>
#include <lucent/log.h>
#include <math.h>
#include <stdio.h>

namespace {

float rqProbeX(const RqItem &item, int vertex) {
  return item.has_xyf ? item.xsf[vertex] : (float)item.xs[vertex];
}

float rqProbeY(const RqItem &item, int vertex) {
  return item.has_xyf ? item.ysf[vertex] : (float)item.ys[vertex];
}

bool rqProbeBarycentric(const RqItem &item, int triangle, float x, float y, float weights[3]) {
  const int i0 = triangle, i1 = triangle + 1, i2 = triangle + 2;
  const float x0 = rqProbeX(item, i0), y0 = rqProbeY(item, i0);
  const float x1 = rqProbeX(item, i1), y1 = rqProbeY(item, i1);
  const float x2 = rqProbeX(item, i2), y2 = rqProbeY(item, i2);
  const float denominator = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
  if (denominator == 0.0f) {
    return false;
  }
  weights[0] = ((y1 - y2) * (x - x2) + (x2 - x1) * (y - y2)) / denominator;
  weights[1] = ((y2 - y0) * (x - x2) + (x0 - x2) * (y - y2)) / denominator;
  weights[2] = 1.0f - weights[0] - weights[1];
  return weights[0] >= 0.0f && weights[1] >= 0.0f && weights[2] >= 0.0f;
}

float rqProbeInterpolate(const float weights[3], float v0, float v1, float v2) {
  return weights[0] * v0 + weights[1] * v1 + weights[2] * v2;
}

int rqProbeUv(const RqItem &item, int triangle, const float weights[3], const int values[4]) {
  const float interpolated =
      rqProbeInterpolate(weights, (float)values[triangle], (float)values[triangle + 1], (float)values[triangle + 2]);
  float lo = (float)values[triangle], hi = lo;
  for (int i = 1; i < 3; ++i) {
    const float value = (float)values[triangle + i];
    lo = value < lo ? value : lo;
    hi = value > hi ? value : hi;
  }
  const float snapped = floorf(interpolated * 4096.0f + 0.5f) / 4096.0f;
  return (int)(snapped < lo ? lo : snapped > hi ? hi : snapped);
}

RqPixelProbeWinner rqProbeWinner(const RqItem &item, const RqPixelSample &sample, uint32_t finalOrder, float d32) {
  RqPixelProbeWinner winner;
  winner.valid = true;
  winner.final_order = finalOrder;
  winner.seq = item.seq;
  winner.dbg_node = item.dbg_node;
  winner.guest_packet = item.guest_packet;
  winner.guest_ot_order = item.guest_ot_order;
  winner.sort_key = item.sort_key;
  winner.key_ord = item.key_ord;
  winner.d32 = d32;
  winner.sample = sample;
  return winner;
}

void rqProbeLogFinal(const RqPixelProbeState &probe) {
  const RqPixelProbeWinner &native = probe.shipping;
  const RqPixelProbeWinner &source = probe.source_ot;
  const RqPixelProbeWinner &guest = probe.guest_ot;
  lucent::info("primat-rq",
               "FINAL f{} @({},{}) compare={} semi_seen={} shipping(valid={} order={} seq={} "
               "node={:08X} packet={:08X} ot_order={} key={} key_ord={:.9f} D32={:.9f} texel={:04X} writes={}) "
               "source_OT(valid={} order={} seq={} node={:08X} key={} key_ord={:.9f} texel={:04X} writes={}) "
               "guest_OT(valid={} order={} seq={} packet={:08X} ot_order={} key={} texel={:04X} writes={})",
               probe.frame,
               probe.x,
               probe.y,
               gpu_vk_world_depth_compare_name(),
               probe.semi_seen,
               native.valid,
               native.final_order,
               native.seq,
               native.dbg_node,
               native.guest_packet,
               native.guest_ot_order,
               native.sort_key,
               native.key_ord,
               native.d32,
               native.sample.texel,
               native.sample.writes,
               source.valid,
               source.final_order,
               source.seq,
               source.dbg_node,
               source.sort_key,
               source.key_ord,
               source.sample.texel,
               source.sample.writes,
               guest.valid,
               guest.final_order,
               guest.seq,
               guest.guest_packet,
               guest.guest_ot_order,
               guest.sort_key,
               guest.sample.texel,
               guest.sample.writes);
}

} // namespace

GpuProvenancePacket gpu_provenance_packet(Core &core, uint32_t node) {
  GpuProvenancePacket packet;
  packet.node = node;
  if (node == 0) {
    return packet;
  }
  const uint32_t address = node & 0x1FFFFCu;
  packet.word_count = std::min(core.mem_r32(address) >> 24u, static_cast<uint32_t>(packet.words.size()));
  for (uint32_t word = 0; word < packet.word_count; ++word) {
    packet.words[word] = core.mem_r32(address + 4u + word * 4u);
  }
  return packet;
}

bool GpuState::pixel_probe_target(int &absoluteX, int &absoluteY) {
  if (!s_pixel_probe.configured) {
    s_pixel_probe.configured = true;
    const char *setting = cfg_str("PSXPORT_PRIMAT");
    if (setting) {
      sscanf(setting, "%d,%d,%d", &s_pixel_probe.x, &s_pixel_probe.y, &s_pixel_probe.from_frame);
    }
  }
  if (s_pixel_probe.x < 0 || s_frame < s_pixel_probe.from_frame) {
    return false;
  }
  absoluteX = s_disp_x + s_pixel_probe.x;
  absoluteY = s_disp_y + s_pixel_probe.y;
  return true;
}

RqPixelSample rq_probe_item_pixel(GpuState &gpu, const RqItem &item, int x, int y) {
  RqPixelSample sample;
  if (x < item.da_x0 || x > item.da_x1 || y < item.da_y0 || y > item.da_y1) {
    return sample;
  }
  float centerWeights[3];
  const int vertexCount = item.nv ? item.nv : 4;
  int triangle = rqProbeBarycentric(item, 0, (float)x + 0.5f, (float)y + 0.5f, centerWeights) ? 0 : -1;
  if (triangle < 0 && vertexCount == 4 &&
      rqProbeBarycentric(item, 1, (float)x + 0.5f, (float)y + 0.5f, centerWeights)) {
    triangle = 1;
  }
  if (triangle < 0) {
    return sample;
  }
  sample.covered = true;
  sample.triangle = triangle;
  sample.interpolated_depth =
      rqProbeInterpolate(centerWeights, item.depth[triangle], item.depth[triangle + 1], item.depth[triangle + 2]);
  if (item.mode == 3) {
    sample.writes = true;
    sample.blends = item.semi != 0;
    return sample;
  }

  float integerWeights[3];
  if (!rqProbeBarycentric(item, triangle, (float)x, (float)y, integerWeights)) {
    integerWeights[0] = centerWeights[0];
    integerWeights[1] = centerWeights[1];
    integerWeights[2] = centerWeights[2];
  }
  const int u = rqProbeUv(item, triangle, integerWeights, item.us);
  const int v = rqProbeUv(item, triangle, integerWeights, item.vs);
  const GpuTextureSample texture = gpu.sample_tex_at(
      u, v, item.tp_x, item.tp_y, item.mode, item.clut_x, item.clut_y, item.tw_mx, item.tw_my, item.tw_ox, item.tw_oy);
  sample.u = texture.u;
  sample.v = texture.v;
  sample.source_word = texture.source_word;
  sample.palette_index = texture.palette_index;
  sample.texel = texture.texel;
  sample.writes = texture.texel != 0;
  sample.blends = sample.writes && item.semi && (texture.texel & 0x8000);
  return sample;
}

bool rq_source_ot_candidate_wins(const RqItem &candidate, const RqPixelProbeWinner &current) {
  return candidate.sort_key >= 0 && (!current.valid || candidate.key_ord > current.key_ord ||
                                     (candidate.key_ord == current.key_ord && candidate.seq < current.seq));
}

void RenderQueue::pixelProbeEmit(Core *core, const RqItem &item, uint32_t finalOrder, uint32_t depthBiasOrder) {
  GpuState &gpu = core->game->gpu;
  int x = 0;
  int y = 0;
  if (!gpu.pixel_probe_target(x, y)) {
    return;
  }
  if (pixelProbe.frame != gpu.s_frame) {
    if (pixelProbe.frame >= 0) {
      rqProbeLogFinal(pixelProbe);
    }
    pixelProbe.frame = gpu.s_frame;
    pixelProbe.x = x - gpu.s_disp_x;
    pixelProbe.y = y - gpu.s_disp_y;
    pixelProbe.semi_seen = false;
    pixelProbe.shipping = {};
    pixelProbe.source_ot = {};
    pixelProbe.guest_ot = {};
  }
  const RqPixelSample sample = rq_probe_item_pixel(gpu, item, x, y);
  if (!sample.covered) {
    return;
  }
  const float d32 =
      item.order_mode == RQ_OM_DEPTH ? gpu_vk_map_ordered_3d_depth(sample.interpolated_depth, depthBiasOrder) : -1.0f;
  lucent::info("primat-rq",
               "f{} final_order={} depth_bias_order={} seq={} node={:08X} packet={:08X} ot_order={} layer={} om={} "
               "semi={} tri={} "
               "nv={} key={} key_ord={:.6f} authored={} compare={} interp={:.9f} D32={:.9f} "
               "mode={} raw={} tp=({},{}) clut=({},{}) uv=({},{}) source={:04X} index={} texel={:04X} "
               "transparent={} writes={} blends={}",
               pixelProbe.frame,
               finalOrder,
               depthBiasOrder,
               item.seq,
               item.dbg_node,
               item.guest_packet,
               item.guest_ot_order,
               item.layer,
               item.order_mode,
               item.semi,
               sample.triangle,
               item.nv,
               item.sort_key,
               (double)item.key_ord,
               item.authored_depth,
               gpu_vk_world_depth_compare_name(),
               sample.interpolated_depth,
               d32,
               item.mode,
               item.raw,
               item.tp_x,
               item.tp_y,
               item.clut_x,
               item.clut_y,
               sample.u,
               sample.v,
               sample.source_word,
               sample.palette_index,
               sample.texel,
               sample.texel == 0 && item.mode != 3,
               sample.writes,
               sample.blends);

  if (sample.blends) {
    pixelProbe.semi_seen = true;
  }
  if (sample.writes && !sample.blends && item.order_mode == RQ_OM_DEPTH &&
      (!pixelProbe.shipping.valid || gpu_vk_world_depth_test_passes(d32, pixelProbe.shipping.d32))) {
    pixelProbe.shipping = rqProbeWinner(item, sample, finalOrder, d32);
  }
  if (sample.writes && !sample.blends && rq_source_ot_candidate_wins(item, pixelProbe.source_ot)) {
    pixelProbe.source_ot = rqProbeWinner(item, sample, finalOrder, d32);
  }
  if (item.guest_packet && sample.writes && !sample.blends) {
    pixelProbe.guest_ot = rqProbeWinner(item, sample, finalOrder, d32);
  }
}

// Provenance query at an ABSOLUTE VRAM coord (the differ replays into the back buffer at off=(0,256),
// so query e.g. vram y = display y + 256 — no double-buffer confound, unlike the live-run PROVAT).
// Requires PSXPORT_PROVAT to be set so put_px_b stamped s_prov during replay.
void gpu_prov_dump(Core *core, int vx, int vy) {
  GpuState &g = core->game->gpu;
  uint16_t p = *g.vram(vx, vy);
  uint32_t gid = g.s_prov[(vy & 511) * VRAM_W + (vx & 1023)];
  ProvMeta *m = &g.s_provmeta[gid % PROVRING];
  lucent::Line ln;
  ln.add("vram({},{})={:04X} rgb({},{},{}) ", vx, vy, p, (p & 31) << 3, ((p >> 5) & 31) << 3, ((p >> 10) & 31) << 3);
  if (!gid) {
    ln.add("<never written>");
    ln.flush(lucent::Level::Info, "prov");
    return;
  }
  if (m->gid != gid) {
    ln.add("gid={} <evicted>", gid);
    ln.flush(lucent::Level::Info, "prov");
    return;
  }
  ln.add("gid={} op={:02X} tex={} texmode={} semi={} blend={} clut=({},{}) tp=({},{}) primcol=({},{},{}) v0=({},{}) "
         "uv0=({},{})",
         gid,
         m->op,
         m->tex,
         m->mode,
         m->semi,
         m->blend,
         m->clut_x,
         m->clut_y,
         m->tp_x,
         m->tp_y,
         m->r,
         m->g,
         m->b,
         m->x0,
         m->y0,
         m->u0,
         m->v0);
  ln.flush(lucent::Level::Info, "prov");
}

// Present-time provenance at DISPLAY coords (qx,qy): report, for the 7x7 box around it, which prim
// last wrote each displayed pixel (op/clut/texpage/color/age). Writes to `out`. Used by both the
// PSXPORT_PROVAT env path and the live debug server (dbg_server.c). Display space sidesteps the
// double-buffer offset. Requires provenance stamping (PSXPORT_PROVAT or gpu_provat_enable()).
void gpu_provat_display(Core *core, FILE *out, int qx, int qy) {
  GpuState &g = core->game->gpu;
  const int s_frame = g.s_frame, s_disp_x = g.s_disp_x, s_disp_y = g.s_disp_y;
  fprintf(out, "[provat] f%d display (%d,%d) +/-3  (disp@%d,%d)\n", s_frame, qx, qy, s_disp_x, s_disp_y);
  if (g.s_prov_on <= 0) {
    fprintf(out, "  (provenance was off; now enabled — re-query after a frame)\n");
    g.s_prov_on = 1;
    return;
  }
  for (int dy = -3; dy <= 3; dy++) {
    for (int dx = -3; dx <= 3; dx++) {
      int vx = s_disp_x + qx + dx, vy = s_disp_y + qy + dy;
      uint16_t p = *g.vram(vx, vy);
      uint32_t gid = g.s_prov[(vy & 511) * VRAM_W + (vx & 1023)];
      ProvMeta *m = &g.s_provmeta[gid % PROVRING];
      int valid = (m->gid == gid && gid != 0);
      fprintf(out,
              "  (%+d,%+d) vram(%d,%d)=%04X rgb(%d,%d,%d)",
              dx,
              dy,
              vx,
              vy,
              p,
              (p & 31) << 3,
              ((p >> 5) & 31) << 3,
              ((p >> 10) & 31) << 3);
      if (!gid) {
        fprintf(out, "  <never written>\n");
      } else if (!valid) {
        fprintf(out, "  gid=%u <evicted: drawn long ago = STALE>\n", gid);
      } else {
        fprintf(out,
                "  gid=%u age=%dframes op=%02X tex=%d mode=%d semi=%d clut=(%d,%d) tp=(%d,%d) "
                "primcol=(%d,%d,%d) node=%08X v0=(%d,%d) uv0=(%d,%d)\n",
                gid,
                (int)((uint32_t)s_frame - m->frame),
                m->op,
                m->tex,
                m->mode,
                m->semi,
                m->clut_x,
                m->clut_y,
                m->tp_x,
                m->tp_y,
                m->r,
                m->g,
                m->b,
                m->node,
                m->x0,
                m->y0,
                m->u0,
                m->v0);
        // The node is the guest OT packet owner, not merely a diagnostic label. Dump its bounded GP0
        // payload so the exact geometry and per-vertex colours behind the winning PSX pixel can be
        // decoded without guessing a packet-pool base/stride or relying on the VK-only polygon tee.
        // PSX packets carry their payload length in the OT tag's high byte; cap corrupt input at the
        // parser FIFO's 256-word capacity.
        if (dx == 0 && dy == 0) {
          const GpuProvenancePacket packet = gpu_provenance_packet(*core, m->node);
          fprintf(out, "    packet=%08X words=%u gp0=", packet.node, packet.word_count);
          for (uint32_t word = 0; word < packet.word_count; ++word) {
            fprintf(out, "%s%08X", word == 0 ? "" : "/", packet.words[word]);
          }
          fprintf(out, "\n");
        }
      }
    }
  }
}

// --- Native scene accounting (graphics OWNERSHIP) -----------------------------------------------
// Read-only walk of the same OT DrawOTag DMAs, classifying every primitive into engine-meaningful
// categories so the port can ACCOUNT for each draw (VRAM copies = reflection/fade buffers, fills,
// large/semi overlays = fade tiles, env). PSXPORT_SCENEDUMP=N. (later-99)
static int gp0_cmd_len(uint8_t op) {
  if (op >= 0x20 && op <= 0x3F) {
    int nv = (op & 0x08) ? 4 : 3, per = 1 + ((op & 0x04) ? 1 : 0) + ((op & 0x10) ? 1 : 0);
    return 1 + nv * per - ((op & 0x10) ? 1 : 0);
  }
  if (op >= 0x40 && op <= 0x5F) {
    return 0;
  }
  if (op >= 0x60 && op <= 0x7F) {
    int t = (op & 0x04) ? 1 : 0, sz = (op >> 3) & 3;
    return 1 + 1 + t + (sz == 0 ? 1 : 0);
  }
  if (op == 0x02) {
    return 3;
  }
  if (op >= 0x80 && op <= 0x9F) {
    return 4;
  }
  if (op >= 0xA0 && op <= 0xDF) {
    return 3;
  }
  return 1;
}
void gpu_scene_dump(Core *core, FILE *out, uint32_t madr) {
  const int s_frame = core->game->gpu.s_frame;
  uint32_t addr = madr & 0x1FFFFC;
  int npoly = 0, nrect = 0, nline = 0, nfill = 0, ncopy = 0, nup = 0, nenv = 0;
  fprintf(out, "[scene] f%d OT@0x%08X — classified display list:\n", s_frame, 0x80000000u | addr);
  for (int g = 0; g < 0x10000; g++) {
    uint32_t hdr = core->mem_r32(addr);
    int n = hdr >> 24, i = 0;
    while (i < n) {
      uint32_t c = core->mem_r32(addr + 4 + i * 4);
      uint8_t op = c >> 24;
      int len = gp0_cmd_len(op);
      if (len <= 0) {
        break;
      }
      uint32_t w1 = (i + 1 < n) ? core->mem_r32(addr + 4 + (i + 1) * 4) : 0;
      uint32_t w2 = (i + 2 < n) ? core->mem_r32(addr + 4 + (i + 2) * 4) : 0;
      if (op == 0x02) {
        nfill++;
        fprintf(out,
                "  FILL rgb=(%d,%d,%d) at(%d,%d) %dx%d\n",
                c & 0xFF,
                (c >> 8) & 0xFF,
                (c >> 16) & 0xFF,
                w1 & 0x3FF,
                (w1 >> 16) & 0x1FF,
                w2 & 0x3FF,
                (w2 >> 16) & 0x1FF);
      } else if (op >= 0x80 && op <= 0x9F) {
        ncopy++;
        uint32_t w3 = (i + 3 < n) ? core->mem_r32(addr + 4 + (i + 3) * 4) : 0;
        fprintf(out,
                "  COPY src(%d,%d)->dst(%d,%d) %dx%d [reflection/fade]\n",
                w1 & 0x3FF,
                (w1 >> 16) & 0x1FF,
                w2 & 0x3FF,
                (w2 >> 16) & 0x1FF,
                w3 & 0x3FF,
                (w3 >> 16) & 0x1FF);
      } else if (op >= 0xA0 && op <= 0xBF) {
        nup++;
      } else if (op >= 0xE1 && op <= 0xE6) {
        nenv++;
      } else if (op >= 0x20 && op <= 0x3F) {
        npoly++;
        if (((op >> 1) & 1) && !((op >> 2) & 1)) {
          fprintf(
              out, "  POLY semi flat rgb=(%d,%d,%d) [fade/overlay?]\n", c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF);
        }
      } else if (op >= 0x60 && op <= 0x7F) {
        nrect++;
      } else if (op >= 0x40 && op <= 0x5F) {
        nline++;
        break;
      }
      i += len;
    }
    uint32_t next = hdr & 0xFFFFFF;
    if (next == 0xFFFFFF || next == 0) {
      break;
    }
    addr = next & 0x1FFFFC;
  }
  fprintf(out,
          "[scene] f%d totals: poly=%d rect=%d line=%d fill=%d vramcopy=%d upload=%d env=%d\n",
          s_frame,
          npoly,
          nrect,
          nline,
          nfill,
          ncopy,
          nup,
          nenv);
}
// On-demand scene dump for the live debug server (dbg_server.c): classify the CURRENT frame's
// last-submitted OT (Gpu::s_ot_madr, set by gpu_dma2_linked_list) into `out`.
void gpu_scene_dump_now(Core *core, FILE *out) {
  gpu_scene_dump(core, out, core->game->gpu.s_ot_madr);
}

// The DISPLAY DECISION, on demand (dbg_server `disp`). Everything that decides which VRAM rectangle
// reaches the screen, plus the draw-side clip that decides what was allowed to be written into it —
// in one place, because a picture that is right except for a band at one edge is always a
// disagreement between those two rectangles, and reading them out of three different logs is how
// that gets guessed at instead of measured.
//
// The point of the "NEVER PROGRAMMED" annotations: a default that reads back like an answer is the
// worst kind of diagnostic. `disp_h = 240` means one thing if the game asked for 240 lines and the
// opposite thing if nothing ever wrote GP1(07) — and the second case is exactly when a strip of
// framebuffer the console would never scan out ends up on screen.
void gpu_disp_dump_now(Core *core, FILE *out) {
  const GpuState &g = core->game->gpu;
  const int vr = g.s_disp_vy1 - g.s_disp_vy0;
  fprintf(out,
          "[disp] f%d display VRAM rect = (%d,%d) %dx%d%s\n",
          g.s_frame,
          g.s_disp_x,
          g.s_disp_y,
          g.s_disp_w,
          g.s_disp_h,
          g.s_disp_rgb24 ? "  24-BIT" : "");
  fprintf(out, "  GP1(05) start   = (%d,%d)\n", g.s_disp_x, g.s_disp_y);
  fprintf(out,
          "  GP1(07) v-range = [%d,%d) = %d line%s%s\n",
          g.s_disp_vy0,
          g.s_disp_vy1,
          vr,
          vr == 1 ? "" : "s",
          g.s_disp_vrange_seen ? ""
                               : "   <-- NEVER PROGRAMMED: this is the framework default, not the "
                                 "game's value. Rows beyond what the game really scans out may be "
                                 "on screen here and on no console.");
  fprintf(out,
          "  GP1(08) width   = %d, %s, %s%s\n",
          g.s_disp_w,
          g.s_disp_480i ? "480i" : "non-interlaced",
          g.s_disp_pal ? "PAL" : "NTSC",
          g.s_disp_std_seen ? "" : "   <-- GP1(08) NEVER PROGRAMMED (default)");
  fprintf(out,
          "  GP0(E3/E4) draw clip = (%d,%d)..(%d,%d)   GP0(E5) offset = (%d,%d)\n",
          g.s_da_x0,
          g.s_da_y0,
          g.s_da_x1,
          g.s_da_y1,
          g.s_off_x,
          g.s_off_y);
  // The one comparison worth making for the caller, stated rather than left as arithmetic: the draw
  // clip lets the game write rows the display then shows. That is normal (the clip is usually the
  // whole buffer); it is only interesting next to a picture with a band at the bottom.
  const int shown_y1 = g.s_disp_y + g.s_disp_h - 1;
  if (g.s_da_y1 >= shown_y1) {
    fprintf(out,
            "  note: the draw clip reaches row %d and the display shows through row %d — anything "
            "the game rasterizes down there IS on screen unless it paints over it.\n",
            g.s_da_y1,
            shown_y1);
  }
}

// WHO SUBMITTED THIS FRAME'S GEOMETRY — the packet->submitter question, on the debug server.
//
// The REPL has had `otattr` for a long time and it was UNREACHABLE where the question is usually asked:
// the REPL blocks the frame loop, so it cannot attach to a live window or to a long resumed session,
// which is exactly where "what draws that thing that is missing" comes up. Same gap `renderpath` had.
//
// Two forms, both read-only:
//   otattr            aggregate the CURRENT ordering table by submitter fn
//   otattr <addr>     attribute one packet address (as printed by `provat`'s node= field)
//
// It reports its DENOMINATORS because the attribution is not total: spans are recorded as the guest
// STORES packets, so a packet whose store the span table missed (or which was written before the table
// was armed) is UNATTRIBUTED, and a table that overflowed says so. An aggregate with no denominator
// would read as "these are all the submitters" when it means "these are the ones I could name".
void gpu_otattr_dump_now(Core *core, FILE *out, uint32_t oneAddr) {
  GpuState &g = core->game->gpu;
  OtAttr &oa = core->rsub.otAttr;

  if (oneAddr) {
    OtAttr::Span sp{};
    if (oa.lookupStore(oneAddr, &sp)) {
      fprintf(out,
              "[otattr] 0x%08X <- fn=0x%08X caller=0x%08X node=0x%08X claimed=0x%08X "
              "(span [0x%08X,0x%08X))\n",
              oneAddr | 0x80000000u,
              sp.fn,
              sp.caller,
              sp.node,
              sp.claimed,
              sp.lo,
              sp.hi);
    } else {
      fprintf(out,
              "[otattr] 0x%08X — NO SPAN COVERS IT. That is 'not recorded', NOT 'nobody wrote it': "
              "%d spans this frame%s.\n",
              oneAddr | 0x80000000u,
              oa.spanCount(),
              oa.spanOverflow() ? " (TABLE OVERFLOWED — attribution is incomplete)" : "");
    }
    return;
  }

  // Aggregate the current OT. Same link walk gpu_scene_dump uses; read-only, no gpu_gp0 side effects.
  struct Row {
    uint32_t fn;
    int packets;
  };
  Row rows[64];
  int nrows = 0;
  int nodes = 0, attributed = 0, unattributed = 0, rowsDropped = 0;
  uint32_t addr = g.s_ot_madr & 0x1FFFFC;
  for (int guard = 0; guard < 0x10000; guard++) {
    const uint32_t hdr = core->mem_r32(addr);
    const unsigned n = hdr >> 24;
    if (n) {
      nodes++;
      OtAttr::Span sp{};
      if (oa.lookupStore(addr + 4, &sp)) {
        attributed++;
        int i = 0;
        for (; i < nrows; i++) {
          if (rows[i].fn == sp.fn) {
            rows[i].packets++;
            break;
          }
        }
        if (i == nrows) {
          if (nrows < 64) {
            rows[nrows++] = {sp.fn, 1};
          } else {
            rowsDropped++;
          }
        }
      } else {
        unattributed++;
      }
    }
    const uint32_t next = hdr & 0xFFFFFF;
    if (next == 0xFFFFFF || next == 0) {
      break;
    }
    addr = next & 0x1FFFFC;
  }
  // Biggest first — the ranking is the point when the question is "what draws most of this scene".
  for (int i = 0; i < nrows; i++) {
    for (int j = i + 1; j < nrows; j++) {
      if (rows[j].packets > rows[i].packets) {
        Row t = rows[i];
        rows[i] = rows[j];
        rows[j] = t;
      }
    }
  }
  fprintf(out,
          "[otattr] f%d OT@0x%08X: %d drawing nodes, %d attributed, %d UNATTRIBUTED"
          " (%d spans%s)\n",
          g.s_frame,
          0x80000000u | g.s_ot_madr,
          nodes,
          attributed,
          unattributed,
          oa.spanCount(),
          oa.spanOverflow() ? ", TABLE OVERFLOWED" : "");
  for (int i = 0; i < nrows; i++) {
    fprintf(out, "  fn=0x%08X  %d packet(s)\n", rows[i].fn, rows[i].packets);
  }
  if (rowsDropped) {
    fprintf(out, "  (+%d distinct fn(s) past the 64-row cap — NOT listed)\n", rowsDropped);
  }
  if (!nodes) {
    fprintf(out, "  the ordering table this frame has no drawing nodes at all.\n");
  }
}

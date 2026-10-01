// device_gpu.cpp — the native GPU's guest-visible state and its VRAM.
//
// WHAT IS HERE AND WHAT IS NOT, and why. Everything the GUEST can read back or that changes what it
// draws next is captured: VRAM, the GP0 draw environment, the display registers, both transfer
// cursors (CPU->VRAM and VRAM->CPU), the GP0 command FIFO with its guest source addresses, and the
// frame/OT bookkeeping.
//
// What is NOT captured is DIAGNOSTIC state: the per-pixel provenance plane (`s_prov`, 2 MB of
// "which primitive last wrote this pixel"), the prim-metadata ring behind it, the vramguard
// registry and its dedupe set, the texwatch/clutwatch payload summaries, and the primdump window.
// None of it can change a pixel or a register the guest can observe, and all of it is either off by
// default or pure counters. It is listed here rather than left implicit because "the section is
// smaller than the struct" is exactly the kind of omission that later reads as an oversight.
//
// The BEETLE GPU is not in this file. It is the independent ORACLE (runtime/psx/gpu_beetle.cpp),
// inert unless a run enabled it, and it is not the GPU the guest draws with — see
// beetle_device_state.h.
#include "device_sections.h"

#include "game.h"
#include "gpu_native_internal.h"

#include <algorithm>
#include <array>

namespace psx::state {
namespace {

constexpr std::uint8_t kGpuLayoutVersion = 1;

} // namespace

void writeGpuSection(Game &game, BlobWriter &out) {
  GpuState &gpu = game.gpu;
  out.u8(kGpuLayoutVersion);

  out.array(gpu.s_vram, std::size(gpu.s_vram));

  out.i32(gpu.s_da_x0);
  out.i32(gpu.s_da_y0);
  out.i32(gpu.s_da_x1);
  out.i32(gpu.s_da_y1);
  out.i32(gpu.s_off_x);
  out.i32(gpu.s_off_y);
  out.i32(gpu.s_tp_x);
  out.i32(gpu.s_tp_y);
  out.i32(gpu.s_tp_mode);
  out.i32(gpu.s_tp_blend);
  out.i32(gpu.s_tp_dither);
  out.i32(gpu.s_tw_mx);
  out.i32(gpu.s_tw_my);
  out.i32(gpu.s_tw_ox);
  out.i32(gpu.s_tw_oy);
  out.i32(gpu.s_clut_x);
  out.i32(gpu.s_clut_y);

  out.i32(gpu.s_disp_x);
  out.i32(gpu.s_disp_y);
  out.i32(gpu.s_disp_w);
  out.i32(gpu.s_disp_h);
  out.i32(gpu.s_disp_vy0);
  out.i32(gpu.s_disp_vy1);
  out.boolean(gpu.s_disp_vrange_seen);
  out.i32(gpu.s_disp_480i);
  out.i32(gpu.s_disp_rgb24);
  out.i32(gpu.s_disp_pal);
  out.boolean(gpu.s_disp_std_seen);

  out.array(gpu.s_fifo, std::size(gpu.s_fifo));
  out.array(gpu.s_fifo_addr, std::size(gpu.s_fifo_addr));
  out.u32(gpu.s_gp0_src);
  out.i64(gpu.s_gp0_addressed);
  out.i64(gpu.s_gp0_anon);
  out.i32(gpu.s_fcount);
  out.i32(gpu.s_fneed);
  out.i32(gpu.s_pl);
  out.i32(gpu.s_pl_g);
  out.i32(gpu.s_xfer);
  out.i32(gpu.s_xfer_x);
  out.i32(gpu.s_xfer_y);
  out.i32(gpu.s_xfer_w);
  out.i32(gpu.s_xfer_h);
  out.i32(gpu.s_xfer_px);
  out.i32(gpu.s_rd);
  out.i32(gpu.s_rd_x);
  out.i32(gpu.s_rd_y);
  out.i32(gpu.s_rd_w);
  out.i32(gpu.s_rd_h);
  out.i32(gpu.s_rd_px);

  out.i32(gpu.s_frame);
  out.i32(gpu.s_interpolated_frames);
  out.i32(gpu.s_seen3d);
  out.i32(gpu.s_prev_had3d);
  out.i32(gpu.s_seen_bg2d);
  out.i32(gpu.s_prev_had_bg2d);
  out.u32(gpu.s_prim_order);
  out.u32(gpu.s_prim_gid);
  out.i32(gpu.s_bg_nrange);
  out.i32(gpu.s_bg_frame);
  out.array(gpu.s_bg_lo, std::size(gpu.s_bg_lo));
  out.array(gpu.s_bg_hi, std::size(gpu.s_bg_hi));
  out.u32(gpu.s_cur_node);
  out.u32(gpu.s_ot_madr);
  out.u32(gpu.s_dma_src);
}

bool readGpuSection(Game &game, BlobReader &in, std::string &error) {
  const std::uint8_t layout = in.u8();
  if (layout != kGpuLayoutVersion) {
    error = "gpu section layout " + std::to_string(layout) + ", this build writes " + std::to_string(kGpuLayoutVersion);
    return false;
  }
  GpuState &gpu = game.gpu;
  std::array<std::uint16_t, VRAM_W * VRAM_H> vram{};
  in.array(vram.data(), vram.size());

  struct DrawArea {
    int x0, y0, x1, y1, offX, offY, tpX, tpY, mode, blend, dither;
    int twMx, twMy, twOx, twOy, clutX, clutY;
  } draw{};
  draw.x0 = in.i32();
  draw.y0 = in.i32();
  draw.x1 = in.i32();
  draw.y1 = in.i32();
  draw.offX = in.i32();
  draw.offY = in.i32();
  draw.tpX = in.i32();
  draw.tpY = in.i32();
  draw.mode = in.i32();
  draw.blend = in.i32();
  draw.dither = in.i32();
  draw.twMx = in.i32();
  draw.twMy = in.i32();
  draw.twOx = in.i32();
  draw.twOy = in.i32();
  draw.clutX = in.i32();
  draw.clutY = in.i32();

  struct Display {
    int x, y, w, h, vy0, vy1, interlace480, rgb24, pal;
    bool vrangeSeen, stdSeen;
  } display{};
  display.x = in.i32();
  display.y = in.i32();
  display.w = in.i32();
  display.h = in.i32();
  display.vy0 = in.i32();
  display.vy1 = in.i32();
  display.vrangeSeen = in.boolean();
  display.interlace480 = in.i32();
  display.rgb24 = in.i32();
  display.pal = in.i32();
  display.stdSeen = in.boolean();

  std::array<std::uint32_t, 256> fifo{};
  std::array<std::uint32_t, 256> fifoAddr{};
  in.array(fifo.data(), fifo.size());
  in.array(fifoAddr.data(), fifoAddr.size());
  const std::uint32_t gp0Src = in.u32();
  const std::int64_t gp0Addressed = in.i64();
  const std::int64_t gp0Anon = in.i64();

  struct Cursors {
    int fcount, fneed, pl, plG, xfer, xferX, xferY, xferW, xferH, xferPx;
    int rd, rdX, rdY, rdW, rdH, rdPx;
  } cursors{};
  cursors.fcount = in.i32();
  cursors.fneed = in.i32();
  cursors.pl = in.i32();
  cursors.plG = in.i32();
  cursors.xfer = in.i32();
  cursors.xferX = in.i32();
  cursors.xferY = in.i32();
  cursors.xferW = in.i32();
  cursors.xferH = in.i32();
  cursors.xferPx = in.i32();
  cursors.rd = in.i32();
  cursors.rdX = in.i32();
  cursors.rdY = in.i32();
  cursors.rdW = in.i32();
  cursors.rdH = in.i32();
  cursors.rdPx = in.i32();

  const std::int32_t frame = in.i32();
  const std::int32_t interpolated = in.i32();
  const std::int32_t seen3d = in.i32();
  const std::int32_t prevHad3d = in.i32();
  const std::int32_t seenBg2d = in.i32();
  const std::int32_t prevHadBg2d = in.i32();
  const std::uint32_t primOrder = in.u32();
  const std::uint32_t primGid = in.u32();
  const std::int32_t bgNrange = in.i32();
  const std::int32_t bgFrame = in.i32();
  std::array<std::uint32_t, GpuState::BG_RANGE_MAX> bgLo{};
  std::array<std::uint32_t, GpuState::BG_RANGE_MAX> bgHi{};
  in.array(bgLo.data(), bgLo.size());
  in.array(bgHi.data(), bgHi.size());
  const std::uint32_t curNode = in.u32();
  const std::uint32_t otMadr = in.u32();
  const std::uint32_t dmaSrc = in.u32();

  if (!in.ok()) {
    error = "the gpu section is " + std::to_string(in.size()) + " bytes and did not decode (" +
            std::to_string(in.offset()) + " consumed)";
    return false;
  }
  if (bgNrange < 0 || bgNrange > GpuState::BG_RANGE_MAX) {
    error = "the gpu section carries " + std::to_string(bgNrange) + " background spans, outside 0.." +
            std::to_string(GpuState::BG_RANGE_MAX);
    return false;
  }

  std::copy(vram.begin(), vram.end(), gpu.s_vram);
  gpu.s_da_x0 = draw.x0;
  gpu.s_da_y0 = draw.y0;
  gpu.s_da_x1 = draw.x1;
  gpu.s_da_y1 = draw.y1;
  gpu.s_off_x = draw.offX;
  gpu.s_off_y = draw.offY;
  gpu.s_tp_x = draw.tpX;
  gpu.s_tp_y = draw.tpY;
  gpu.s_tp_mode = draw.mode;
  gpu.s_tp_blend = draw.blend;
  gpu.s_tp_dither = draw.dither;
  gpu.s_tw_mx = draw.twMx;
  gpu.s_tw_my = draw.twMy;
  gpu.s_tw_ox = draw.twOx;
  gpu.s_tw_oy = draw.twOy;
  gpu.s_clut_x = draw.clutX;
  gpu.s_clut_y = draw.clutY;

  gpu.s_disp_x = display.x;
  gpu.s_disp_y = display.y;
  gpu.s_disp_w = display.w;
  gpu.s_disp_h = display.h;
  gpu.s_disp_vy0 = display.vy0;
  gpu.s_disp_vy1 = display.vy1;
  gpu.s_disp_vrange_seen = display.vrangeSeen;
  gpu.s_disp_480i = display.interlace480;
  gpu.s_disp_rgb24 = display.rgb24;
  gpu.s_disp_pal = display.pal;
  gpu.s_disp_std_seen = display.stdSeen;

  std::copy(fifo.begin(), fifo.end(), gpu.s_fifo);
  std::copy(fifoAddr.begin(), fifoAddr.end(), gpu.s_fifo_addr);
  gpu.s_gp0_src = gp0Src;
  gpu.s_gp0_addressed = gp0Addressed;
  gpu.s_gp0_anon = gp0Anon;
  gpu.s_fcount = cursors.fcount;
  gpu.s_fneed = cursors.fneed;
  gpu.s_pl = cursors.pl;
  gpu.s_pl_g = cursors.plG;
  gpu.s_xfer = cursors.xfer;
  gpu.s_xfer_x = cursors.xferX;
  gpu.s_xfer_y = cursors.xferY;
  gpu.s_xfer_w = cursors.xferW;
  gpu.s_xfer_h = cursors.xferH;
  gpu.s_xfer_px = cursors.xferPx;
  gpu.s_rd = cursors.rd;
  gpu.s_rd_x = cursors.rdX;
  gpu.s_rd_y = cursors.rdY;
  gpu.s_rd_w = cursors.rdW;
  gpu.s_rd_h = cursors.rdH;
  gpu.s_rd_px = cursors.rdPx;

  gpu.s_frame = frame;
  gpu.s_interpolated_frames = interpolated;
  gpu.s_seen3d = seen3d;
  gpu.s_prev_had3d = prevHad3d;
  gpu.s_seen_bg2d = seenBg2d;
  gpu.s_prev_had_bg2d = prevHadBg2d;
  gpu.s_prim_order = primOrder;
  gpu.s_prim_gid = primGid;
  gpu.s_bg_nrange = bgNrange;
  gpu.s_bg_frame = bgFrame;
  std::copy(bgLo.begin(), bgLo.end(), gpu.s_bg_lo);
  std::copy(bgHi.begin(), bgHi.end(), gpu.s_bg_hi);
  gpu.s_cur_node = curNode;
  gpu.s_ot_madr = otMadr;
  gpu.s_dma_src = dmaSrc;
  // The per-frame draw counters are deliberately not restored: they count the present being
  // assembled, and `frame_finalize` resets them at the next present anyway. The frame NUMBER is
  // restored, because a guest-visible clock is not.
  return true;
}

} // namespace psx::state
// gpu_native_api.cpp — the C-style GPU entry points. Every guest-visible access goes to the Core's
// GpuDevice; the same words also feed this Game's GpuState, which builds the native render.
#include "core.h"
#include "game.h"
#include "gpu_device.h"
#include "gpu_native_internal.h" // GpuState + GpuPresentCompletion
#include "ordering_table.h"
#include "proj_prim.h" // ProjPrim::Stats — the vertex-depth cache the run-end report reads

#include <lucent/log.h>

#include <algorithm>
#include <vector>

void gpu_gp0(Core *core, uint32_t w) {
  core->gpuDevice.gp0(w);
  core->game->gpu.gpu_gp0(core, w);
}

// Replay ONE guest packet the DrawOTag walk did not read out of guest RAM.
//
// A title's native world pass can rebuild a packet stream the guest never stored — this repository's
// Spyro terrain in-between runs the guest's own draw routine again over HOST memory and gets its
// packets back. Those packets are still guest packets: the same GP0 words, the same primitive kinds,
// in the same order the guest's ordering table would have walked them. The only thing missing is the
// place to read them from, and the only two things the submit path takes from that place are the
// packet's guest-shaped address (recorded as the item's guest packet, and read by the
// screen-coverage background classification) and each word's address (the FIFO's own source stamp).
// So the caller supplies both, as guest-shaped addresses, and everything after that — the FIFO state
// machine, texpage/CLUT/draw-area resolution, layer classification, the emit-or-queue funnel — runs
// exactly where it has always run.
//
// alternative, decoding each packet into resolved quad data and calling RenderQueue::emitOrQueue
// directly, is what this function exists to avoid: it would put a SECOND copy of texpage/CLUT/
// draw-area/blend resolution in a title, and the two copies would drift.
void gpu_replay_guest_packet(Core *core, uint32_t nodeAddress, const uint32_t *words, unsigned count) {
  core->game->gpu.replayGuestPacket(core, nodeAddress, words, count);
}
void gpu_gp1(Core *core, uint32_t w) {
  core->gpuDevice.gp1(w, core->game->timing.emulatedCpuTicks());
  core->game->gpu.gpu_gp1(w);
}

void gpu_dma2_linked_list(Core *core, uint32_t madr) {
  psx::gpu::submitOrderingTable(*core, core->gpuDevice, madr);
  core->game->gpu.gpu_dma2_linked_list(core, madr);
}
void gpu_dma2_block(Core *core, uint32_t madr, int count, int to_gpu) {
  const uint32_t base = psx::gpu::mainRamOffsetOf(madr);
  for (int i = 0; i < count; i++) {
    const uint32_t address = base + static_cast<uint32_t>(i) * 4u;
    if (to_gpu) {
      core->gpuDevice.gp0(core->mem_r32(address), psx::gpu::guestAddressOf(address));
    } else {
      core->mem_w32(address, core->gpuDevice.read());
    }
  }
  if (to_gpu) {
    core->game->gpu.gpu_dma2_block(core, madr, count);
  }
}
uint32_t gpu_read_word(Core *core) {
  return core->gpuDevice.read();
}
void gpu_present(Core *core) {
  core->game->gpu.gpu_present(core);
}
void gpu_present_ex(Core *core, int do_blit) {
  core->game->gpu.gpu_present_ex(core, do_blit, GpuPresentCompletion::MainFrame);
}
// differential test per-core frame finalize: the readback grab renders + reads this core's frame but skips gpu_present,
// so it must run the same per-frame reset/bookkeeping standalone's present does (else s_prim_order etc.
// never reset — see GpuState::frame_finalize). Replaces the bare gpu_vk_frame_end grabPane used to call.
void gpu_present_finalize(Core *core) {
  core->game->gpu.frame_finalize(core);
}
// differential test accessors: each core's CPU front-buffer (s_vram) + its current display region, so the differential
// test composite can present each core's frame into its own pane (gpu_vk_present_sbs2). GpuState is a plain struct
// (all-public), so these reach the members directly.
const uint16_t *gpu_vram_ptr(Core *core) {
  return core->game->gpu.s_vram;
}
void gpu_disp_region(Core *core, int *sx, int *sy, int *w, int *h) {
  GpuState &g = core->game->gpu;
  if (sx) {
    *sx = g.s_disp_x;
  }
  if (sy) {
    *sy = g.s_disp_y;
  }
  if (w) {
    *w = g.s_disp_w;
  }
  if (h) {
    *h = g.s_disp_h;
  }
}
void gpu_clear_display(Core *core) {
  core->game->gpu.gpu_clear_display(core);
}
void gpu_native_load_image(Core *core, int x, int y, int w, int h, uint32_t src) {
  std::vector<uint16_t> pixels(static_cast<size_t>(std::max(w, 0)) * static_cast<size_t>(std::max(h, 0)));
  for (size_t i = 0; i < pixels.size(); i++) {
    pixels[i] = core->mem_r16(src + static_cast<uint32_t>(i * 2));
  }
  core->gpuDevice.loadImage(x, y, w, h, pixels);
  core->game->gpu.gpu_native_load_image(core, x, y, w, h, src, pixels);
}
int gpu_native_load_vram(Core *core, const char *path) {
  return core->game->gpu.gpu_native_load_vram(path);
}
void gpu_native_shot(Core *core, const char *path) {
  core->game->gpu.gpu_native_shot(core, path);
}
int gpu_frame_no(Core *core) {
  return core->game->gpu.gpu_frame_no();
}
uint16_t gpu_vram_peek(Core *core, int x, int y) {
  return core->game->gpu.gpu_vram_peek(x, y);
}
void gpu_vram_load(Core *core, const uint16_t *src) {
  core->game->gpu.gpu_vram_load(src);
}
void gpu_vram_save(Core *core, uint16_t *dst) {
  core->game->gpu.gpu_vram_save(dst);
}

// ── render_depth_coverage_report — see render_stats.h for why this exists. ────────────────────────
void render_depth_coverage_report(Core *core, const char *why) {
  const long long d3 = core->rsub.stats.nd3dTotal, d2 = core->rsub.stats.nd2dTotal;
  const long long tot = d3 + d2;
  const ProjPrim::Stats pp = core->rsub.projprim.totals();
  if (tot == 0) {
    lucent::warn("ndepth",
                 "depth coverage ({}): NO PRIMITIVES WERE CLASSIFIED AT ALL this run — not 0% 3D, "
                 "but nothing measured. The run drew no polygons through the native classifier, so "
                 "it says nothing about whether depth works. (vertex-depth cache: {} record(s), {} "
                 "hit(s), {} miss(es).)",
                 why,
                 pp.set,
                 pp.hit,
                 pp.miss);
    return;
  }
  lucent::info("ndepth",
               "depth coverage ({}): {} of {} prim(s) carried REAL per-vertex depth = {:.2f}% 3D; "
               "the other {} fell to the deferred 2D order band. Vertex-depth cache over the same "
               "run: {} record(s), {} lookup hit(s), {} miss(es) ({:.2f}% of lookups hit).",
               why,
               d3,
               tot,
               100.0 * (double)d3 / (double)tot,
               d2,
               pp.set,
               pp.hit,
               pp.miss,
               (pp.hit + pp.miss) ? 100.0 * (double)pp.hit / (double)(pp.hit + pp.miss) : 0.0);
  lucent::info("ndepth",
               "  of those {} miss(es), {} were STALE (the address was ours, but the guest had "
               "overwritten the word, so the entry no longer described a vertex and was refused) "
               "and {} were ABSENT (no depth was ever recorded there). They want opposite fixes: "
               "stale means entry lifetime outran the buffer, absent means the tap never fired.",
               pp.miss,
               pp.stale,
               pp.miss - pp.stale);
  // WHERE the misses landed, over the whole run rather than one sampled frame. "Records climb, hits
  // do not" has two different causes — wrong buffer entirely, or right buffer wrong word — and the
  // ratio above cannot separate them. Needs PSXPORT_DEBUG=pznear; it says so itself when off.
  long long ctry = 0, ccar = 0;
  void gte_copy_pz_counts(long long *, long long *);
  gte_copy_pz_counts(&ctry, &ccar);
  lucent::info("ndepth",
               "  buffer-to-buffer depth carry: {} copy site(s) ran, {} found a depth at the source "
               "and carried it ({:.2f}%). A large gap here means the staged vertices are not where "
               "the copy thinks they are; a small one means the carry works and the misses are "
               "elsewhere.",
               ctry,
               ccar,
               ctry ? 100.0 * (double)ccar / (double)ctry : 0.0);
  core->rsub.projprim.nearReport("run-end");
}

// GP0 differ — "ours" side. Replays a captured GP0 trace (PSXPORT_GPUTRACE, format "GP0TRC01")
// through OUR rasterizer (runtime/recomp/gpu_native.c) starting from the trace's initial VRAM,
// then writes the resulting 1024x512x16 VRAM. The Beetle side is `wide60rt -gpureplay` feeding the
// SAME trace into mednafen's GPU. Diff the two VRAM dumps (tools/gpu_differ/diff.py): identical
// input ⇒ any pixel difference is a pure rasterizer-fidelity difference (blend/modulation), with
// no live game-state alignment needed.
//
// Build: tools/gpu_differ/build.sh  (compiles gpu_native.c + this, no SDL/runtime needed).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

// gpu_native.c entry points (the GP0 path is self-contained).
void gpu_gp0(uint32_t w);
void gpu_vram_load(const uint16_t* src);
void gpu_vram_save(uint16_t* dst);
void gpu_prov_dump(int vx, int vy);   // PSXPORT_PROVAT must be set so provenance is stamped

// gpu_native.c references these only outside the GP0 path (DMA walk / window pacing / watchdog),
// which the differ never drives. Stub them so the renderer links standalone.
uint8_t  g_ram[0x200000];   // referenced by gpu_native.c debug dumps; unused in replay
uint8_t  mem_r8 (uint32_t a) { (void)a; return 0; }
uint16_t mem_r16(uint32_t a) { (void)a; return 0; }
uint32_t mem_r32(uint32_t a) { (void)a; return 0; }
// fps60 taps (gated PSXPORT_FPS60, off here) referenced by gpu_native.c's GP0/present path; stub.
void     fps60_join_poly(int px, int py) { (void)px; (void)py; }
void     ws_sx_dump(const char* tag)      { (void)tag; }
void     watchdog_pet(void)  {}
// VK backend + fps60 capture hooks gpu_native.c grew since this tool was written. The differ runs the
// SOFTWARE rasterizer only (gpu_vk_enabled()==0, g_fps60_on==0), so these are never CALLED — they just
// need to LINK. Empty-param defs link by symbol name regardless of the call sites' prototypes.
int  g_fps60_on = 0;
// cfg (env flags) + scene-dump: gpu_native.c / native_dl.c reference these outside the GP0 replay path
// (DL-mode select, SCENEDUMP). The differ replays a fixed stream with no flags, so return defaults.
const char* cfg_str(const char* k) { (void)k; return 0; }
int  cfg_on (const char* k) { (void)k; return 0; }
int  cfg_dbg(const char* k) { (void)k; return 0; }
void gpu_scene_dump() {}
void proj_probe_dump() {}      // PSXPORT_PROJPROBE / RTP-caller probes (off in replay)
void rtpcaller_dump() {}
void rtpcaller_reset() {}
// Native-depth / projprim infra (gte_beetle.c): the differ runs the SW rasterizer with native depth off,
// so these only need to link. native_depth_on/attach_enabled return 0 → the depth lookups are never hit.
int  g_pp_hit, g_pp_set, g_pp_miss;
void projprim_reset() {}
float projprim_lookup_pz() { return 0.0f; }
float proj_pz_to_ord() { return 0.0f; }
int  attach_enabled(void) { return 0; }
int  native_depth_on(void) { return 0; }
void trace_flush() {}             // GPU trace writer (capture-side only; replay doesn't re-trace)
void trace_record() {}
void trace_arm() {}
void gpu_provat_display() {}      // PSXPORT_PROVAT pixel-provenance (off in replay)
void gpu_prov_dump(int vx, int vy) { (void)vx; (void)vy; }   // provenance lives in gpu_debug.c (not linked here)
// VK depth/order/semi hooks gpu_native.c tees to — never reached with gpu_vk_enabled()==0, link-only.
void gpu_vk_semi_group() {}
void gpu_vk_set_order() {}
void gpu_vk_set_order_2d() {}
void gpu_vk_set_order_2d_bg() {}
void gpu_vk_set_order_2d_bg_n() {}
void gpu_vk_set_order_2d_n() {}
void gpu_vk_set_vd() {}
void gpu_vk_set_vd_n() {}
int  gpu_vk_wide_engine(void)   { return 0; }
int  gpu_vk_wide_engine_w(void) { return 320; }
int  gpu_vk_enabled(void) { return 0; }
void gpu_vk_draw_tritri() {}
void gpu_vk_draw_semi() {}
void gpu_vk_dirty() {}
void gpu_vk_present() {}
void gpu_vk_frame_end() {}
void gpu_vk_dump() {}
void fps60_cap_poly() {}
void fps60_cap_sprite() {}
void fps60_cap_line() {}
void fps60_rtp() {}

int main(int argc, char** argv) {
  if (argc < 3) { fprintf(stderr, "usage: %s <trace.bin> <out.vram>\n", argv[0]); return 2; }
  FILE* f = fopen(argv[1], "rb");
  if (!f) { perror("open trace"); return 1; }
  char magic[8] = {0}; uint32_t meta[4] = {0};
  if (fread(magic, 1, 8, f) != 8 || memcmp(magic, "GP0TRC01", 8) != 0 || fread(meta, 4, 4, f) != 4) {
    fprintf(stderr, "[replay_ours] bad trace header\n"); fclose(f); return 1;
  }
  uint32_t frame = meta[0], n = meta[1], vw = meta[2], vh = meta[3];
  if (vw != 1024 || vh != 512) { fprintf(stderr, "[replay_ours] unexpected dims %ux%u\n", vw, vh); fclose(f); return 1; }
  uint16_t* init  = (uint16_t*)malloc((size_t)vw * vh * 2);
  uint32_t* words = (uint32_t*)malloc((size_t)n * 4);
  if (fread(init, 2, (size_t)vw * vh, f) != (size_t)vw * vh || fread(words, 4, n, f) != n) {
    fprintf(stderr, "[replay_ours] truncated trace\n"); fclose(f); return 1;
  }
  fclose(f);

  gpu_vram_load(init);
  for (uint32_t i = 0; i < n; i++) gpu_gp0(words[i]);

  uint16_t* out = (uint16_t*)malloc((size_t)vw * vh * 2);
  gpu_vram_save(out);
  FILE* o = fopen(argv[2], "wb");
  if (!o) { perror("open out"); return 1; }
  fwrite(out, 2, (size_t)vw * vh, o);
  fclose(o);
  fprintf(stderr, "[replay_ours] replayed f%u (%u words) -> %s\n", frame, n, argv[2]);

  // Optional provenance query: `replay_ours trace out VX VY` prints which prim our renderer used
  // to write absolute VRAM pixel (VX,VY). Set PSXPORT_PROVAT=1 so stamping is enabled.
  if (argc >= 5) gpu_prov_dump(atoi(argv[3]), atoi(argv[4]));
  return 0;
}

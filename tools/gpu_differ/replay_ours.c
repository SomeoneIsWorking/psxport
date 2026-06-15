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
uint8_t  mem_r8 (uint32_t a) { (void)a; return 0; }
uint16_t mem_r16(uint32_t a) { (void)a; return 0; }
uint32_t mem_r32(uint32_t a) { (void)a; return 0; }
void     watchdog_pet(void)  {}

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

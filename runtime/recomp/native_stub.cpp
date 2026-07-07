// PC-native boot entry (replaces interpreting the disc's PSX boot stub SCUS_944.54).
//
// The REAL PSX boot is: BIOS -> boot executable (the SCUS stub) -> stub draws the SCEA
// "Sony Computer Entertainment America Presents" screen, then BIOS-LoadExec's cdrom:\MAIN.EXE;1.
// We no longer interpret that stub: its CD/VSync busy-waits depended on the override mechanism
// (removed 2026-06-22, top-down PC-driven rebuild). Instead BootStub::run renders SCEA PC-native
// from a baked asset (native_scea_splash + gpu_scea_load_asset), then loads MAIN.EXE itself and
// enters the native MAIN boot (native_boot.cpp), which takes over for the intro FMVs + the menu.
#include "core.h"
#include "game.h"   // class BootStub lives on Game (game->stub); this TU implements its run()
#include "c_subsys.h"
#include "cfg.h"
#include "scea_asset.h"   // SCEA_DISP_W/H (the decoded RGBA splash dims)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
void native_boot_run(Core* c);
void interp_trace_open(const char* path);

static uint32_t rd32(const uint8_t* p) { return p[0] | p[1]<<8 | p[2]<<16 | (uint32_t)p[3]<<24; }

// Load a PS-X EXE image into g_ram and set the initial register file (gp/sp/fp/ra), matching the
// BIOS loader. Returns the entry PC. (Same contract as boot.c's load_exe; duplicated here so the
// stub path is self-contained — the stub overwrites MAIN's low text, and the MAIN reload at
// hand-off restores it, exactly as the real boot does.)
static uint32_t load_exe_image(const char* path, Core* c) {
  FILE* f = fopen(path, "rb");
  if (!f) { perror(path); exit(1); }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t* buf = (uint8_t*)malloc(n);
  if (fread(buf, 1, n, f) != (size_t)n) { fprintf(stderr, "short read %s\n", path); exit(1); }
  fclose(f);
  uint32_t entry = rd32(buf+0x10), gp = rd32(buf+0x14);
  uint32_t load = rd32(buf+0x18), tsize = rd32(buf+0x1C), sp = rd32(buf+0x30);
  memcpy(&c->ram[load & 0x1FFFFF], buf + 0x800, tsize);
  free(buf);
  c->r[28] = gp;
  c->r[29] = sp ? sp : 0x801FFFF0u;
  c->r[30] = c->r[29];
  c->r[31] = 0xDEAD0000u;               // top-level return sentinel
  fprintf(stderr, "[stub] loaded %s: entry 0x%08X load 0x%08X text 0x%X sp 0x%08X\n",
          path, entry, load, tsize, c->r[29]);
  return entry;
}

// PC-native SCEA license screen (replaces the interpreted PSX boot stub SCUS_944.54). The stub's
// only jobs were: draw SCEA, then CdInit + LoadExec MAIN. Its CD/VSync waits used to be unstalled by
// the override mechanism (now removed), so we no longer interpret it. We render SCEA directly from the
// baked asset (scea_asset.h, via gpu_scea_load_asset) with a fade-in over a fixed duration, skippable
// with Start (loading is PC-native, so it IS skippable now), then BootStub::run LoadExec's MAIN.
// Fade envelope over the splash: fade in, hold at full, fade back out (the PSX SCEA does the same).
// Durations measured from the REAL SCUS_944.54 stub vs the oracle (docs/journal.md later-46/48):
// total ~305 frames (~5.1s @ 60Hz) — a ~57-frame linear grey->white fade-in, a long full-bright hold,
// then a ~2-3 frame fade-out at the tail. (The HLE's old count*2 / 160-hold were guesses, discarded.)
#define SCEA_FADE_IN   57                                  // frames to ramp 0 -> 128 (linear grey->white)
#define SCEA_FADE_OUT  3                                   // short fade-out at the very end (f304-305)
#define SCEA_HOLD      245                                 // frames held at full brightness (~305 total)
#define SCEA_FRAMES    (SCEA_FADE_IN + SCEA_HOLD + SCEA_FADE_OUT)
// Dump a faded SCEA RGBA buffer to a PPM (headless verification of the decode/layout). rgba is
// SCEA_DISP_W x SCEA_DISP_H RGBA8; `fade01` (0..1) scales rgb exactly as the shader does on screen.
static void scea_dump_ppm(const uint8_t* rgba, float fade01, const char* path) {
  FILE* f = fopen(path, "wb"); if (!f) { perror(path); return; }
  fprintf(f, "P6\n%d %d\n255\n", SCEA_DISP_W, SCEA_DISP_H);
  for (int i = 0; i < SCEA_DISP_W * SCEA_DISP_H; i++) {
    const uint8_t* p = rgba + (size_t)i * 4;
    unsigned char o[3] = { (unsigned char)(p[0] * fade01), (unsigned char)(p[1] * fade01),
                           (unsigned char)(p[2] * fade01) };
    fwrite(o, 1, 3, f);
  }
  fclose(f);
  fprintf(stderr, "[scea] wrote %s (%dx%d, fade %.2f)\n", path, SCEA_DISP_W, SCEA_DISP_H, fade01);
}

static void native_scea_splash(Core* c) {
  void gpu_scea_decode_rgba(uint8_t*);
  void gpu_gpu_present_image(Core*, const uint8_t*, int, int, float);
  void gpu_pace_frame(Core*); void gpu_clear_display(Core*);
  // Decode the baked SCEA asset into a PC-native RGBA8 screen image ONCE (no PSX VRAM / GP0 / CLUT path).
  uint8_t* scea_rgba = (uint8_t*)malloc((size_t)SCEA_DISP_W * SCEA_DISP_H * 4);
  gpu_scea_decode_rgba(scea_rgba);
  int dumped = 0;
  for (int f = 0; f < SCEA_FRAMES; f++) {
#ifdef PSXPORT_SDL
    { if (gpu_windowed()) c->game->pad.pollSdl(); }
#endif
    if ((c->game->pad.buttons & 0x0008u) == 0) {          // Start = skip the license screen
      fprintf(stderr, "[scea] skipped (Start) at frame %d\n", f); break; }
    int fade;                                             // 0..128: fade in, hold, fade out
    if (f < SCEA_FADE_IN)                  fade = f * 128 / SCEA_FADE_IN;
    else if (f < SCEA_FADE_IN + SCEA_HOLD) fade = 128;
    else                                   fade = (SCEA_FRAMES - 1 - f) * 128 / SCEA_FADE_OUT;
    gpu_gpu_present_image(c, scea_rgba, SCEA_DISP_W, SCEA_DISP_H, fade / 128.0f);
    // Headless: present_image only uploads (no swapchain) — dump the CPU rgba (faded) at the hold midpoint
    // so the decode/layout can be verified offline.
    if (!dumped && f == SCEA_FADE_IN + SCEA_HOLD / 2) {
      scea_dump_ppm(scea_rgba, fade / 128.0f, "scratch/screenshots/scea_native_check.ppm");
      dumped = 1;
    }
    gpu_pace_frame(c);                                    // paces to ~60 Hz when windowed; headless = fast
    watchdog_pet();
  }
  gpu_clear_display(c);                                   // hard black after the fade-out (clean cut to FMV)
  free(scea_rgba);
}

void BootStub::run(const char* main_exe_path) {
  Core* c = &game->core;
  main_path = main_exe_path;
  interp_trace_open(cfg_str("PSXPORT_INTERP_TRACE"));

  // PC-native boot: render SCEA natively (no interpreted PSX stub), then load MAIN.EXE ourselves and
  // enter the native MAIN boot. (The PSX stub SCUS_944.54 is no longer run — see native_scea_splash.)
  native_scea_splash(c);
  load_exe_image(main_path, c);   // load MAIN.EXE image + initial registers into the Core
  fprintf(stderr, "[stub] entering native MAIN boot\n");
  native_boot_run(c);
}

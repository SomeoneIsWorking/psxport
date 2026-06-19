// S2 boot driver: allocate ONE Core instance, load MAIN.EXE into its RAM, and run the boot stub
// (which draws SCEA then hands off to the native MAIN boot). The runtime is now object-oriented —
// the machine state lives in a `Core` (core.h), reached explicitly (no global). main() owns the
// instance; everything it calls receives `c`.
#include "core.h"
#include "game.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// C subsystems (compiled as C) reached across the boundary — declare with C linkage.
extern "C" {
  void watchdog_init(void); void mdec_init(void); void spu_init(void);
  void spu_audio_init(void); void cdc_init(void);
}

static uint32_t rd32(const uint8_t* p) { return p[0] | p[1]<<8 | p[2]<<16 | (uint32_t)p[3]<<24; }

static void load_exe(const char* path, Core* c) {
  FILE* f = fopen(path, "rb");
  if (!f) { perror(path); exit(1); }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t* buf = (uint8_t*)malloc(n);
  if (fread(buf, 1, n, f) != (size_t)n) { fprintf(stderr, "short read\n"); exit(1); }
  fclose(f);
  uint32_t entry = rd32(buf+0x10), gp = rd32(buf+0x14);
  uint32_t load = rd32(buf+0x18), tsize = rd32(buf+0x1C), sp = rd32(buf+0x30);
  memcpy(&c->ram[load & 0x1FFFFF], buf + 0x800, tsize);
  free(buf);
  c->r[28] = gp;                       // gp
  c->r[29] = sp ? sp : 0x801FFFF0u;    // sp
  c->r[30] = c->r[29];                 // fp
  c->r[31] = 0xDEAD0000u;              // ra sentinel (top-level return)
  fprintf(stderr, "loaded %s: entry 0x%08X load 0x%08X text 0x%X sp 0x%08X\n",
          path, entry, load, tsize, c->r[29]);
}

// rec_dispatch_miss now lives in hle.c (routes A0/B0/C0 to the HLE BIOS).

int main(int argc, char** argv) {
  const char* path = argc > 1 ? argv[1] : "scratch/bin/tomba2/MAIN.EXE";
  void gpu_vk_tritest(void);
  gpu_vk_tritest();           // PSXPORT_VK_TRITEST=1: GPU triangle-rasterizer self-test, then exit
  Game* game = new Game();    // the whole machine (owns the Core + every subsystem's state — no globals)
  Core* c = &game->core;      // the CPU/RAM handle threaded through the interp (2 MB RAM lives in Game)
  void watchdog_init(void);
  watchdog_init();            // PSXPORT_WATCHDOG=<sec>: abort+backtrace if a frame stalls
  load_exe(path, c);
  void cd_overrides_init(void);
  void timing_init(void);
  void games_tomba2_init(void);
  void sync_overrides_init(void);
  void pad_overrides_init(Core*);
  void card_overrides_init(void);
  void threads_init(Core*);
  void threads_register_overrides(void);
  void gte_init(void);
  void gpu_native_init(void);
  void mdec_init(void);
  void spu_init(void);
  void spu_audio_init(void);
  gte_init();               // GTE (COP2) coprocessor, lifted from Beetle
  mdec_init();              // MDEC video decoder (FMV), lifted from Beetle
  spu_init();               // SPU audio core, lifted from Beetle
  spu_audio_init();         // SDL audio output sink (PSXPORT_NOAUDIO to disable)
  gpu_native_init();        // native GPU renderer (parses the game's GP0 stream)
  void cdc_init(void);
  cdc_init();               // native CD controller registers (0x1F801800-3) for raw-CD code
  cd_overrides_init();      // native CD: drive-ready + by-LBA read (S3)
  timing_init();            // native VBlank/VSync source (S3)
  games_tomba2_init();      // Tomba2 per-game overrides (vblank pacing)
  sync_overrides_init();    // convert HW sync/wait stalls to native non-stall
  pad_overrides_init(c);    // native controller input (per-VBlank pad read override)
  card_overrides_init();    // native memory card (synchronous file-backed libcard I/O)
  threads_init(c);          // native BIOS threads (ucontext); main = slot 0
  threads_register_overrides();
  c->r[4] = 1; c->r[5] = 0;  // a0=argc-ish, a1=argv (BIOS sets these; minimal)

  // Replicate the REAL PSX boot path. The disc's boot executable is the SCUS_944.54 *stub* (not
  // MAIN.EXE): it draws the SCEA "…America Presents" screen itself, then BIOS-LoadExec's
  // cdrom:\MAIN.EXE;1 and jumps to MAIN's entry. We run the stub as the real entry (interpreted —
  // it isn't recompiled) and intercept its LoadExec to hand off to the native MAIN boot
  // (native_boot.c, later 33/34). See docs/journal.md "later 34" + [[psxport-scea-boot-stub]].
  void native_stub_run(Core*, const char* main_exe_path);
  native_stub_run(c, path);              // stub draws SCEA, then hands off to native MAIN boot
  fprintf(stderr, "[boot] native_stub_run returned\n");
  return 0;
}

// runtime/recomp/boot.cpp (framework) — the MAIN.EXE loader. The process entry point main() is GAME-side
// (game/core/main.cpp): it installs the game seam then constructs+drives the framework machine. This file
// keeps only load_exe (the PS-EXE loader), which the game main AND the harnesses (DualCore/Sbs) all call.
// The framework provides no main(): the standalone psxport_smoke supplies its own.
#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const uint8_t* p) { return p[0] | p[1]<<8 | p[2]<<16 | (uint32_t)p[3]<<24; }

void load_exe(const char* path, Core* c) {   // non-static: the dual-core harness loads two cores
  FILE* f = fopen(path, "rb");
  if (!f) { perror(path); exit(1); }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t* buf = (uint8_t*)malloc(n);
  if (fread(buf, 1, n, f) != (size_t)n) { cfg_loge("boot", "short read on %s", path); exit(1); }
  fclose(f);
  uint32_t entry = rd32(buf+0x10), gp = rd32(buf+0x14);
  uint32_t load = rd32(buf+0x18), tsize = rd32(buf+0x1C), sp = rd32(buf+0x30);
  memcpy(&c->ram[load & 0x1FFFFF], buf + 0x800, tsize);
  free(buf);
  c->r[28] = gp;                       // gp
  c->r[29] = sp ? sp : 0x801FFFF0u;    // sp
  c->r[30] = c->r[29];                 // fp
  c->r[31] = 0xDEAD0000u;              // ra sentinel (top-level return)
  cfg_logi("boot", "loaded %s: entry 0x%08X load 0x%08X text 0x%X sp 0x%08X",
           path, entry, load, tsize, c->r[29]);
}

// rec_dispatch_miss now lives in hle.c (routes A0/B0/C0 to the HLE BIOS).

// S2 boot driver (reconnaissance): load MAIN.EXE into RAM, enter the recompiled entry
// function, and log where execution first reaches the BIOS vectors / overlays. Dispatch
// misses are counted and the run aborts at a budget so a missing-effect path can't spin.
// This maps the HLE surface the boot path actually needs before implementing it.
#include "r3000.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void func_800896E0(R3000*);  // MAIN.EXE entry

static uint32_t rd32(const uint8_t* p) { return p[0] | p[1]<<8 | p[2]<<16 | (uint32_t)p[3]<<24; }

static void load_exe(const char* path, R3000* c) {
  FILE* f = fopen(path, "rb");
  if (!f) { perror(path); exit(1); }
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  uint8_t* buf = malloc(n);
  if (fread(buf, 1, n, f) != (size_t)n) { fprintf(stderr, "short read\n"); exit(1); }
  fclose(f);
  uint32_t entry = rd32(buf+0x10), gp = rd32(buf+0x14);
  uint32_t load = rd32(buf+0x18), tsize = rd32(buf+0x1C), sp = rd32(buf+0x30);
  memcpy(&g_ram[load & 0x1FFFFF], buf + 0x800, tsize);
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
  R3000 c = {0};
  load_exe(path, &c);
  c.r[4] = 1; c.r[5] = 0;   // a0=argc-ish, a1=argv (BIOS sets these; minimal)
  func_800896E0(&c);
  fprintf(stderr, "[exit] entry function returned to top level\n");
  return 0;
}

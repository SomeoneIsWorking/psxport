// dualcore.cpp — PSX-vs-native dual-core diff harness (PSXPORT_DUALCORE=1).
//
// Boots TWO Game instances from the same MAIN.EXE: core A with psx_fallback=1 (the working PSX-recomp
// baseline) and core B native (psx_fallback=0). Both run HEADLESS in diff_mode (guest-state work only,
// host output skipped). The cores run SEQUENTIALLY — A fully to its sync point, then B — because the
// Beetle GTE/SPU backends are process-global singletons; sequential avoids cross-core clobbering and lets
// us snapshot each core's SPU sound-RAM (where the VAB instrument samples live). We compare:
//   * the per-core MUSIC TIMELINE (current-song 0x800BED80 over the run) — does native trigger the same
//     BGM as PSX, at the same point? (the conversation-music question)
//   * main RAM + scratchpad + SPU RAM at the sync point — where does native's state diverge, including
//     the SPU samples the main-RAM diff is blind to.
//
// The durable PSX-vs-PC comparison instrument: no oracle emulator, just the psx_fallback gate.

#include "game.h"
#include "c_subsys.h"
#include <cstdio>
#include <cstring>
#include <cstdlib>

// boot.cpp / native_boot.cpp / pad_input.cpp / Beetle SPU (C) hooks.
void load_exe(const char* path, Core* c);
void dc_boot_init(Core* c);
void dc_step_frame(Core* c, uint32_t f);
void pad_repl_tap(Core* c, uint16_t active_low_mask, int n);
extern "C" void SPU_PeekRAM(uint8_t* dst);   // Beetle spu.c — snapshot 512 KB SPU RAM
extern "C" void SPU_Power(void);             // Beetle SPU reset (between sequential runs)

namespace {

constexpr uint32_t GAME_ENTRY  = 0x8010637Cu;  // task0 entry while the GAME stage runs (in the field)
constexpr uint32_t TASK0_ENTRY = 0x801fe00cu;  // task0 obj +0xc = current stage entry
constexpr uint32_t SND_SONG    = 0x800BED80u;  // libsnd current-song halfword (s16; 0xFFFF = none)
constexpr uint32_t SPU_RAM_SZ  = 0x80000u;     // 512 KB SPU sound RAM

enum Phase { REACH_GAME, SKIP_DIALOG, SETTLE, DONE };
struct Nav { Phase phase = REACH_GAME; int t = 0; };

bool nav_step(Core* c, Nav& nv, uint32_t f, const char* tag) {
  switch (nv.phase) {
    case REACH_GAME:
      if (c->mem_r32(TASK0_ENTRY) == GAME_ENTRY) {
        fprintf(stderr, "[dualcore] %s reached GAME at f%u\n", tag, f); nv.phase = SKIP_DIALOG; nv.t = 0;
      } else if ((f % 12u) == 0) pad_repl_tap(c, (uint16_t)(0xFFFF & ~0x4000), 6);   // tap Cross
      break;
    case SKIP_DIALOG:
      if ((f % 24u) == 0) pad_repl_tap(c, (uint16_t)(0xFFFF & ~0x0008), 6);          // tap Start
      if (++nv.t >= 400) { fprintf(stderr, "[dualcore] %s dialog span done at f%u\n", tag, f);
                           nv.phase = SETTLE; nv.t = 0; }
      break;
    case SETTLE: if (++nv.t >= 90) nv.phase = DONE; break;
    case DONE: return true;
  }
  return false;
}

// Run ONE core to its sync point, logging its music (current-song) timeline. Returns the frame reached.
uint32_t run_core(Game* g, const char* tag) {
  Nav nv; int last_song = -2; uint32_t f = 0;
  const uint32_t MAXF = 4000;
  fprintf(stderr, "[dualcore] --- running %s ---\n", tag);
  for (; f < MAXF; f++) {
    bool done = nav_step(&g->core, nv, f, tag);
    if (!done) dc_step_frame(&g->core, f);
    int song = (int16_t)g->core.mem_r16(SND_SONG);
    if (song != last_song) { fprintf(stderr, "[music] %s f%u song %d -> %d\n", tag, f, last_song, song);
                             last_song = song; }
    if (done) break;
  }
  fprintf(stderr, "[dualcore] %s settled at f%u (final song=%d)\n", tag, f, (int16_t)g->core.mem_r16(SND_SONG));
  return f;
}

// Coalesced byte-range diff of two buffers; reports regions where B is all-zero (suspect missing) first,
// then the largest others. `gbase` is the guest base address for labeling (0x80000000 for RAM, 0 for SPU).
void diff_buf(const char* name, const uint8_t* a, const uint8_t* b, uint32_t n, uint32_t gbase) {
  const uint32_t GAP = 256u;
  struct R { uint32_t s, e; int a0, b0, used; };
  static R rg[4096]; int nr = 0; uint32_t total = 0;
  uint32_t i = 0;
  while (i < n) {
    if (a[i] == b[i]) { i++; continue; }
    uint32_t s = i, last = i, gap = 0; i++;
    while (i < n && gap < GAP) { if (a[i] != b[i]) { last = i; gap = 0; } else gap++; i++; }
    if (nr < 4096) {
      int aa = 1, bb = 1; for (uint32_t k = s; k <= last; k++) { if (a[k]) aa = 0; if (b[k]) bb = 0; }
      rg[nr++] = { s, last + 1, aa, bb, 0 }; }
    total += last + 1 - s;
  }
  fprintf(stderr, "\n[%s] %d divergent region(s), %u bytes:\n", name, nr, total);
  int sus = 0;
  for (int k = 0; k < nr; k++) if (rg[k].b0 && !rg[k].a0) {
    fprintf(stderr, "  ZERO-IN-NATIVE  0x%08X .. 0x%08X  (%u B)\n", gbase | rg[k].s, gbase | rg[k].e, rg[k].e - rg[k].s);
    rg[k].used = 1; sus++;
  }
  if (!sus) fprintf(stderr, "  (no zero-in-native regions)\n");
  for (int shown = 0; shown < 12; shown++) {
    int best = -1; uint32_t bl = 0;
    for (int k = 0; k < nr; k++) { if (rg[k].used) continue; uint32_t l = rg[k].e - rg[k].s; if (l > bl) { bl = l; best = k; } }
    if (best < 0) break;
    fprintf(stderr, "  diff  0x%08X .. 0x%08X  (%u B)%s\n", gbase | rg[best].s, gbase | rg[best].e, bl,
            rg[best].a0 ? "  [A-zero]" : "");
    rg[best].used = 1;
  }
}

} // namespace

void dualcore_run(const char* exe_path) {
  fprintf(stderr, "[dualcore] PSX-vs-native diff harness (exe=%s)\n", exe_path);
  watchdog_suspend();

  uint8_t* spuA = (uint8_t*)malloc(SPU_RAM_SZ);

  // --- Core A: PSX-fallback baseline ---
  Game* A = new Game(); A->psx_fallback = 1; A->diff_mode = 1;
  load_exe(exe_path, &A->core); dc_boot_init(&A->core);
  run_core(A, "A(PSX)");
  SPU_PeekRAM(spuA);                 // snapshot A's SPU RAM before B clobbers the shared backend

  // --- Core B: native (reset the shared SPU first so B starts clean) ---
  SPU_Power();
  Game* B = new Game(); B->psx_fallback = 0; B->diff_mode = 1;
  load_exe(exe_path, &B->core); dc_boot_init(&B->core);
  run_core(B, "B(native)");
  uint8_t* spuB = (uint8_t*)malloc(SPU_RAM_SZ);
  SPU_PeekRAM(spuB);

  // --- compare ---
  fprintf(stderr, "\n========== DUALCORE DIFF (A=PSX-fallback  B=native) ==========\n");
  fprintf(stderr, "libsnd: open-seq count A=%d B=%d | playmask A=0x%04X B=0x%04X | cur-song A=%d B=%d\n",
          (int16_t)A->core.mem_r16(0x801054B0u), (int16_t)B->core.mem_r16(0x801054B0u),
          A->core.mem_r32(0x80104C28u) & 0xFFFF, B->core.mem_r32(0x80104C28u) & 0xFFFF,
          (int16_t)A->core.mem_r16(SND_SONG), (int16_t)B->core.mem_r16(SND_SONG));
  diff_buf("MAIN RAM", A->core.ram, B->core.ram, 0x200000u, 0x80000000u);
  diff_buf("SCRATCHPAD", A->core.scratch, B->core.scratch, 0x400u, 0x1F800000u);
  diff_buf("SPU RAM", spuA, spuB, SPU_RAM_SZ, 0x0u);
  fprintf(stderr, "==============================================================\n");
  free(spuA); free(spuB);
}

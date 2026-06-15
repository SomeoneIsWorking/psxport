// Memory + CPU-support runtime for the recompiled MAIN.EXE.
// Faithful-first: a flat 2 MB RAM + 1 KB scratchpad. Hardware I/O (0x1F801xxx) and
// the BIOS region are stubbed here and will route to peripheral modules (GPU/SPU/CD/
// DMA/timers, lifted from Beetle) in S5. Host is little-endian (PSX is LE), so word
// access is a memcpy.
#include "r3000.h"
#include <stdio.h>
#include <stdlib.h>
#include <execinfo.h>

uint8_t g_ram[0x200000];
uint8_t g_scratch[0x400];

// PSXPORT_CW="lo,hi" — host-backtrace watchpoint: when ANY store (8/16/32) lands in physical
// byte range [lo,hi), dump a C backtrace (recompiled fn names need -rdynamic). Finds the runtime
// code that clobbers a region the decompressor wrote correctly (gameplay CLUT/texture corruption).
static int      s_cw_init = 0;
static uint32_t s_cw_lo = 0, s_cw_hi = 0;
static int      s_cw_n = 0;
static void cw_check(uint32_t a, uint32_t v, int width) {
  if (!s_cw_init) {
    s_cw_init = 1;
    const char* w = getenv("PSXPORT_CW");
    if (w) { unsigned long lo=0, hi=0; if (sscanf(w, "%lx,%lx", &lo, &hi) == 2) { s_cw_lo=lo; s_cw_hi=hi; } }
  }
  extern volatile uint32_t g_interp_pc;
  uint32_t p = a & 0x1FFFFFFF;
  if (s_cw_hi && p >= s_cw_lo && p < s_cw_hi) {
    s_cw_n++;
    if (getenv("PSXPORT_CW_BT") && s_cw_n <= 24) {
      fprintf(stderr, "[cw] #%d store w%d [%08X]=%08X  interp_pc=%08X\n", s_cw_n, width, 0x80000000u|p, v, g_interp_pc);
      void* bt[32]; int n = backtrace(bt, 32); backtrace_symbols_fd(bt, n, 2);
      fprintf(stderr, "----\n");
    }
  }
}

// Map a virtual address to a RAM/scratchpad host pointer, or NULL for I/O.
// KUSEG/KSEG0/KSEG1 all alias the same physical space: strip the top 3 bits.
static inline uint8_t* host_ptr(uint32_t a, uint32_t bytes) {
  const uint32_t p = a & 0x1FFFFFFF;
  if (p + bytes <= 0x200000) return &g_ram[p];
  if (p >= 0x1F800000 && p + bytes <= 0x1F800400) return &g_scratch[p - 0x1F800000];
  return 0;
}

// I/O model. The GPU ports + DMA channel 2 route to the NATIVE renderer (gpu_native.c):
// the game's GP0 primitive stream is rendered by our own rasterizer, not a PSX GPU emulator.
static int g_io_verbose = 0;  // PSXPORT_IO_VERBOSE=1 to log every stray access

void gpu_gp0(uint32_t w);
void gpu_gp1(uint32_t w);
uint32_t cdc_read(uint32_t p);
void     cdc_write(uint32_t p, uint8_t v);
void gpu_dma2_linked_list(uint32_t madr);
void gpu_dma2_block(uint32_t madr, int count, int to_gpu);
// MDEC (native, lifted from Beetle via mdec_beetle.c). DMA0 = in (compressed), DMA1 = out (pixels).
void     mdec_write(uint32_t addr, uint32_t val);
uint32_t mdec_read(uint32_t addr);
void     mdec_dma_in(const uint32_t* words, int count);
int      mdec_dma_out(uint32_t* words, int count);
// SPU (native, lifted from Beetle via spu_beetle.c). DMA channel 4 = SPU-RAM transfer.
void     spu_write(uint32_t addr, uint32_t val);
uint32_t spu_read(uint32_t addr);
void     spu_dma_write(const uint32_t* words, int count);
int      spu_dma_read(uint32_t* words, int count);

static uint32_t s_dma0_madr, s_dma0_bcr, s_dma0_chcr;  // DMA0 MDEC-in
static uint32_t s_dma1_madr, s_dma1_bcr, s_dma1_chcr;  // DMA1 MDEC-out
static uint32_t s_dma2_madr, s_dma2_bcr, s_dma2_chcr;  // DMA2 GPU
static uint32_t s_dma4_madr, s_dma4_bcr, s_dma4_chcr;  // DMA4 SPU
static uint32_t s_dma6_madr, s_dma6_bcr, s_dma6_chcr;  // DMA6 OTC (ordering-table clear)
static uint32_t s_dma_buf[0x10000];                    // staging for block DMA

static int dma_block_words(uint32_t bcr) {  // sync-mode-1 block DMA total word count
  uint32_t bs = bcr & 0xFFFF, bc = bcr >> 16;
  return (int)(bs * (bc ? bc : 1));
}

static uint32_t io_read(uint32_t a, uint32_t bytes) {
  const uint32_t p = a & 0x1FFFFFFF;
  // GPUSTAT (0x1F801814 read): report ready — DMA/cmd-ready (bit 26), VRAM-ready (27),
  // DMA-ready (28); toggle bit 31 (even/odd line) so odd/even spin loops also progress.
  if (p == 0x1F801814) {
    static uint32_t toggle = 0;
    toggle ^= 0x80000000u;
    return 0x1C000000u | toggle;
  }
  if (p >= 0x1F801800 && p <= 0x1F801803) return cdc_read(p);   // CD controller registers
  if (p == 0x1F801810) return 0;                 // GPUREAD (VRAM-store path: minimal)
  if (p == 0x1F801820 || p == 0x1F801824) return mdec_read(p);  // MDEC0 data / MDEC1 status
  // SPUSTAT (0x1F801DAE read): report idle/transfer-complete — bits 0x7FF cleared. The libspu
  // reset (FUN_80096BF0) spins `while (SPUSTAT & 0x7FF)` and the SPU-RAM upload (FUN_80096E70)
  // spins `while (SPUSTAT & 0x400)` (DMA/data-transfer busy), bailing to "SPU T_O" on timeout —
  // those busy bits are cleared by the transfer-done IRQ the no-IRQ runtime never raises, and our
  // SPU DMA4 (io_write below) completes synchronously, so the transfer is always already done.
  // Returning 0 makes every `& 0x7FF`/`& 0x400` poll fall through on its first check (mirrors the
  // GPUSTAT ready-fake above). Only the SPUSTAT word is faked; all other SPU regs go to Beetle.
  if (p == 0x1F801DAE) return 0;
  if (p >= 0x1F801C00 && p <= 0x1F801FFF) return spu_read(p);    // SPU register file
  if (p == 0x1F8010C0) return s_dma4_madr;
  if (p == 0x1F8010C4) return s_dma4_bcr;
  if (p == 0x1F8010C8) return s_dma4_chcr;
  if (p == 0x1F801080) return s_dma0_madr;
  if (p == 0x1F801084) return s_dma0_bcr;
  if (p == 0x1F801088) return s_dma0_chcr;
  if (p == 0x1F801090) return s_dma1_madr;
  if (p == 0x1F801094) return s_dma1_bcr;
  if (p == 0x1F801098) return s_dma1_chcr;
  if (p == 0x1F8010A0) return s_dma2_madr;        // DMA2 MADR
  if (p == 0x1F8010A4) return s_dma2_bcr;         // DMA2 BCR
  if (p == 0x1F8010A8) return s_dma2_chcr;        // DMA2 CHCR (busy bit already cleared)
  if (p == 0x1F8010E0) return s_dma6_madr;        // DMA6 OTC MADR
  if (p == 0x1F8010E4) return s_dma6_bcr;         // DMA6 OTC BCR
  if (p == 0x1F8010E8) return s_dma6_chcr;        // DMA6 OTC CHCR (busy bit already cleared)
  if (g_io_verbose)
    fprintf(stderr, "[io] read%u @ 0x%08X -> 0\n", bytes * 8, a);
  return 0;
}
static void io_write(uint32_t a, uint32_t v, uint32_t bytes) {
  const uint32_t p = a & 0x1FFFFFFF;
  if (p >= 0x1F801800 && p <= 0x1F801803) { cdc_write(p, (uint8_t)v); return; }  // CD controller
  if (p == 0x1F801810) { gpu_gp0(v); return; }    // GP0 (direct)
  if (p == 0x1F801814) { gpu_gp1(v); return; }    // GP1 (display/control)
  if (p == 0x1F801820 || p == 0x1F801824) { mdec_write(p, v); return; }  // MDEC0 cmd / MDEC1 ctrl
  if (p >= 0x1F801C00 && p <= 0x1F801FFF) { spu_write(p, v); return; }    // SPU register file
  if (p == 0x1F8010C0) { s_dma4_madr = v; return; }
  if (p == 0x1F8010C4) { s_dma4_bcr = v; return; }
  if (p == 0x1F8010C8) {                           // DMA4 CHCR: SPU-RAM transfer
    s_dma4_chcr = v;
    if (v & 0x01000000u) {
      int n = dma_block_words(s_dma4_bcr); if (n > 0x10000) n = 0x10000;
      uint32_t a = s_dma4_madr & 0x1FFFFC;
      if (v & 1) {                                 // RAM -> SPU
        for (int i = 0; i < n; i++) s_dma_buf[i] = mem_r32(a + i * 4);
        spu_dma_write(s_dma_buf, n);
      } else {                                     // SPU -> RAM
        int got = spu_dma_read(s_dma_buf, n);
        for (int i = 0; i < got; i++) mem_w32(a + i * 4, s_dma_buf[i]);
      }
      s_dma4_chcr &= ~0x01000000u;
    }
    return;
  }
  if (p == 0x1F801080) { s_dma0_madr = v; return; }
  if (p == 0x1F801084) { s_dma0_bcr = v; return; }
  if (p == 0x1F801088) {                           // DMA0 CHCR: MDEC-in (RAM -> MDEC)
    s_dma0_chcr = v;
    if (v & 0x01000000u) {
      int n = dma_block_words(s_dma0_bcr); if (n > 0x10000) n = 0x10000;
      uint32_t a = s_dma0_madr & 0x1FFFFC;
      for (int i = 0; i < n; i++) s_dma_buf[i] = mem_r32(a + i * 4);
      mdec_dma_in(s_dma_buf, n);
      s_dma0_chcr &= ~0x01000000u;
    }
    return;
  }
  if (p == 0x1F801090) { s_dma1_madr = v; return; }
  if (p == 0x1F801094) { s_dma1_bcr = v; return; }
  if (p == 0x1F801098) {                           // DMA1 CHCR: MDEC-out (MDEC -> RAM)
    s_dma1_chcr = v;
    if (v & 0x01000000u) {
      int n = dma_block_words(s_dma1_bcr); if (n > 0x10000) n = 0x10000;
      for (int i = 0; i < n; i++) s_dma_buf[i] = 0;     // clear: mdec_dma_out scatters into buf
      mdec_dma_out(s_dma_buf, n);                       // places macroblock words at buf[i+offs]
      uint32_t a = s_dma1_madr & 0x1FFFFC;              // copy the whole post-scatter region
      for (int i = 0; i < n; i++) mem_w32(a + i * 4, s_dma_buf[i]);
      s_dma1_chcr &= ~0x01000000u;
    }
    return;
  }
  if (p == 0x1F8010A0) { s_dma2_madr = v; return; }
  if (p == 0x1F8010A4) { s_dma2_bcr = v; return; }
  if (p == 0x1F8010A8) {                           // DMA2 CHCR: start triggers the transfer
    s_dma2_chcr = v;
    if (v & 0x01000000u) {                         // start/busy
      int sync = (v >> 9) & 3, to_gpu = v & 1;
      if (sync == 2) gpu_dma2_linked_list(s_dma2_madr);            // ordering-table linked list
      else if (sync == 1) gpu_dma2_block(s_dma2_madr,              // block: BC = blocks*size
               (int)((s_dma2_bcr & 0xFFFF) * (s_dma2_bcr >> 16)), to_gpu);
      else gpu_dma2_block(s_dma2_madr, (int)(s_dma2_bcr & 0xFFFF), to_gpu);  // immediate
      s_dma2_chcr &= ~0x01000000u;                 // clear busy -> game's DMA-done poll passes
    }
    return;
  }
  if (p == 0x1F8010E0) { s_dma6_madr = v; return; }
  if (p == 0x1F8010E4) { s_dma6_bcr = v; return; }
  if (p == 0x1F8010E8) {                           // DMA6 CHCR: OTC (ordering-table clear)
    s_dma6_chcr = v;
    // OTC builds a reverse-linked empty ordering table in RAM (this is what ClearOTagR/ClearOTag
    // use: FUN_80082424 sets MADR=&ot[n-1] (highest entry), BCR=n, then writes 0x11000002 here).
    // It writes n words descending from MADR: each entry -> the next-LOWER entry, and the lowest
    // entry -> 0x00FFFFFF (terminator). DrawOTag(&ot[n-1]) then walks ot[n-1]..ot[0]->end. Without
    // this, the OT entries keep stale values -> a malformed/cyclic chain (DrawOTag never ends).
    if (v & 0x01000000u) {                         // start/busy
      uint32_t n = s_dma6_bcr & 0xFFFF; if (n == 0) n = 0x10000;
      uint32_t madr = s_dma6_madr & 0x1FFFFC;      // highest entry address (&ot[n-1])
      for (uint32_t i = 0; i < n; i++) {
        uint32_t addr = madr - i * 4;
        uint32_t word = (i == n - 1) ? 0x00FFFFFFu : ((addr - 4) & 0x00FFFFFFu);
        mem_w32(addr, word);
      }
      s_dma6_chcr &= ~0x01000000u;                 // clear busy -> ClearOTagR's busy-poll passes
    }
    return;
  }
  if (g_io_verbose)
    fprintf(stderr, "[io] write%u @ 0x%08X = 0x%08X\n", bytes * 8, a, v);
}
#define io_log_r(a, b) io_read((a), (b))
#define io_log_w(a, v, b) io_write((a), (v), (b))

uint8_t mem_r8(uint32_t a) {
  uint8_t* p = host_ptr(a, 1);
  return p ? *p : (uint8_t)io_log_r(a, 1);
}
uint16_t mem_r16(uint32_t a) {
  uint8_t* p = host_ptr(a, 2);
  if (!p) return (uint16_t)io_log_r(a, 2);
  uint16_t v; memcpy(&v, p, 2); return v;
}
uint32_t mem_r32(uint32_t a) {
  uint8_t* p = host_ptr(a, 4);
  if (!p) return io_log_r(a, 4);
  uint32_t v; memcpy(&v, p, 4); return v;
}
void mem_w8(uint32_t a, uint8_t v) {
  uint8_t* p = host_ptr(a, 1);
  cw_check(a, v, 1);
  if (p) *p = v; else io_log_w(a, v, 1);
}
void mem_w16(uint32_t a, uint16_t v) {
  uint8_t* p = host_ptr(a, 2);
  cw_check(a, v, 2);
  if (p) memcpy(p, &v, 2); else io_log_w(a, v, 2);
}
// PSXPORT_WWATCH=lo,hi — log the interpreter PC of any store landing in [lo,hi). Used to find
// the game code that builds a specific primitive/struct in RAM (e.g. the spurious red quad).
extern volatile uint32_t g_interp_pc;
static int      s_ww_init = 0;
static uint32_t s_ww_lo = 0, s_ww_hi = 0;
static void wwatch_check(uint32_t a, uint32_t v) {
  if (!s_ww_init) {
    s_ww_init = 1;
    const char* w = getenv("PSXPORT_WWATCH");
    if (w) { unsigned long lo=0, hi=0; if (sscanf(w, "%lx,%lx", &lo, &hi) == 2) { s_ww_lo=lo; s_ww_hi=hi; } }
  }
  uint32_t ka = a | 0x80000000u;
  if (s_ww_hi && ka >= s_ww_lo && ka < s_ww_hi)
    fprintf(stderr, "[wwatch] store [%08X]=%08X by pc=%08X stage=%08X\n", ka, v, g_interp_pc, mem_r32(0x801fe00c));
}
void mem_w32(uint32_t a, uint32_t v) {
  uint8_t* p = host_ptr(a, 4);
  wwatch_check(a, v);
  cw_check(a, v, 4);
  if (p) memcpy(p, &v, 4); else io_log_w(a, v, 4);
}

// lwl/lwr/swl/swr: little-endian unaligned word merge.
// lwl loads the most-significant bytes (up to the word boundary above the address),
// lwr the least-significant; together they assemble an unaligned word into a register.
uint32_t mem_lwl(uint32_t cur, uint32_t a) {
  const uint32_t aligned = mem_r32(a & ~3u);
  const uint32_t sh = (a & 3) * 8;          // bytes within the word
  const uint32_t keep = (0x00FFFFFFu >> sh); // low bytes of cur to preserve
  return (cur & keep) | (aligned << (24 - sh));
}
uint32_t mem_lwr(uint32_t cur, uint32_t a) {
  const uint32_t aligned = mem_r32(a & ~3u);
  const uint32_t sh = (a & 3) * 8;
  const uint32_t keep = sh ? (0xFFFFFF00u << (24 - sh)) : 0;
  return (cur & keep) | (aligned >> sh);
}
void mem_swl(uint32_t a, uint32_t v) {
  const uint32_t base = a & ~3u;
  const uint32_t sh = (a & 3) * 8;
  const uint32_t aligned = mem_r32(base);
  const uint32_t keep = 0xFFFFFF00u << sh;
  mem_w32(base, (aligned & keep) | (v >> (24 - sh)));
}
void mem_swr(uint32_t a, uint32_t v) {
  const uint32_t base = a & ~3u;
  const uint32_t sh = (a & 3) * 8;
  const uint32_t aligned = mem_r32(base);
  // keep = the low `sh` bits (bytes BELOW the store address, which SWR must preserve).
  // Was 0x00FFFFFFu (wrong): that zeroed the preserved low byte(s) on every unaligned SWR,
  // corrupting unaligned word stores — i.e. the SWL/SWR memcpy/decompression idiom — which
  // garbled gameplay assets wholesale (journal later-59). Correct mask is 0xFFFFFFFF>>(32-sh).
  const uint32_t keep = sh ? (0xFFFFFFFFu >> (32 - sh)) : 0;
  mem_w32(base, (aligned & keep) | (v << sh));
}

// R3000 integer division semantics (no traps; defined results for /0 and overflow).
void cpu_div(R3000* c, uint32_t n, uint32_t d) {
  int32_t sn = (int32_t)n, sd = (int32_t)d;
  if (sd == 0) { c->lo = sn < 0 ? 1u : 0xFFFFFFFFu; c->hi = (uint32_t)sn; }
  else if (n == 0x80000000u && sd == -1) { c->lo = 0x80000000u; c->hi = 0; }
  else { c->lo = (uint32_t)(sn / sd); c->hi = (uint32_t)(sn % sd); }
}
void cpu_divu(R3000* c, uint32_t n, uint32_t d) {
  if (d == 0) { c->lo = 0xFFFFFFFFu; c->hi = n; }
  else { c->lo = n / d; c->hi = n % d; }
}

// Memory + CPU-support runtime for the recompiled MAIN.EXE.
// Faithful-first: a flat 2 MB RAM + 1 KB scratchpad. Hardware I/O (0x1F801xxx) and
// the BIOS region are stubbed here and will route to peripheral modules (GPU/SPU/CD/
// DMA/timers, lifted from Beetle) in S5. Host is little-endian (PSX is LE), so word
// access is a memcpy.
#include "r3000.h"
#include <stdio.h>

uint8_t g_ram[0x200000];
uint8_t g_scratch[0x400];

// Map a virtual address to a RAM/scratchpad host pointer, or NULL for I/O.
// KUSEG/KSEG0/KSEG1 all alias the same physical space: strip the top 3 bits.
static inline uint8_t* host_ptr(uint32_t a, uint32_t bytes) {
  const uint32_t p = a & 0x1FFFFFFF;
  if (p + bytes <= 0x200000) return &g_ram[p];
  if (p >= 0x1F800000 && p + bytes <= 0x1F800400) return &g_scratch[p - 0x1F800000];
  return 0;
}

// Minimal I/O model (S2): just enough to keep boot moving until the real peripheral
// modules (GPU/SPU/CD/DMA/timers, lifted from Beetle) land in S5. Documented, not magic.
static int g_io_verbose = 0;  // PSXPORT_IO_VERBOSE=1 to log every stray access

static uint32_t io_read(uint32_t a, uint32_t bytes) {
  const uint32_t p = a & 0x1FFFFFFF;
  // GPUSTAT (0x1F801814): report the GPU permanently "ready" so the boot ready-poll
  // (waits on cmd/VRAM/DMA-ready bits 26-28) advances; toggle bit 31 (drawing even/odd
  // line) each read so vsync/odd-even spin loops also progress. No real GPU yet.
  if (p == 0x1F801814) {
    static uint32_t toggle = 0;
    toggle ^= 0x80000000u;
    return 0x1C000000u | toggle;  // bits 26/27/28 ready + alternating bit31
  }
  if (g_io_verbose)
    fprintf(stderr, "[io] read%u @ 0x%08X -> 0\n", bytes * 8, a);
  return 0;
}
static void io_write(uint32_t a, uint32_t v, uint32_t bytes) {
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
  if (p) *p = v; else io_log_w(a, v, 1);
}
void mem_w16(uint32_t a, uint16_t v) {
  uint8_t* p = host_ptr(a, 2);
  if (p) memcpy(p, &v, 2); else io_log_w(a, v, 2);
}
void mem_w32(uint32_t a, uint32_t v) {
  uint8_t* p = host_ptr(a, 4);
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
  const uint32_t keep = sh ? (0x00FFFFFFu >> (32 - sh)) : 0;
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

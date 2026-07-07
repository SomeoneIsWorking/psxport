// Memory + CPU-support for one Core instance (OOP: these are Core methods, so the per-instance
// RAM/scratchpad/DMA state lives in `this`, reached with no global). PSX 2 MB main RAM mirrored
// across KUSEG/KSEG0/KSEG1 + 1 KB scratchpad; hardware I/O (0x1F801xxx) routes to the per-game
// peripheral modules. Host is little-endian (PSX is LE), so word access is a memcpy.
#include "core.h"
#include "game.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <execinfo.h>

// Subsystem entry points reached from the I/O map. gpu_dma2_* read/write this instance's RAM and
// take the Core (declared in core.h); the rest operate on words/buffers handed to them.
// gpu_gp0/gpu_gp1 live in the C++ renderer (gpu_native.cpp); gpu_gp0 takes the Core (CPU->VRAM
// uploads + the DMA linked-list walk read this instance's RAM).
void gpu_gp0(Core* core, uint32_t w);
void gpu_gp1(Core*, uint32_t w);
extern "C" {
#include "cdc_state.h"
void     mdec_write(uint32_t addr, uint32_t val);
uint32_t mdec_read(uint32_t addr);
void     mdec_dma_in(const uint32_t* words, int count);
int      mdec_dma_out(uint32_t* words, int count);
void     spu_write(uint32_t addr, uint32_t val);
uint32_t spu_read(uint32_t addr);
void     spu_dma_write(const uint32_t* words, int count);
int      spu_dma_read(uint32_t* words, int count);
}

// Core::Core / ~Core moved to runtime/recomp/core.cpp (lifetime + subsystem wiring live there).

// PSXPORT_CW="lo,hi" — host-backtrace watchpoint: when ANY store lands in physical byte range
// [lo,hi), dump a C backtrace. Finds runtime code that clobbers a region the decompressor wrote.
void Core::mem_set_watch(uint32_t lo, uint32_t hi) {
  s_cw_init = 1; s_cw_lo = lo & 0x1FFFFFFF; s_cw_hi = hi & 0x1FFFFFFF; s_cw_n = 0;
  fprintf(stderr, "[cw] watch [%08X,%08X)\n", 0x80000000u | s_cw_lo, 0x80000000u | s_cw_hi);
}
int Core::mem_watch_hits() { return s_cw_n; }

void Core::cw_check(uint32_t a, uint32_t v, int width) {
  if (!s_cw_init) {
    s_cw_init = 1;
    const char* w = cfg_str("PSXPORT_CW");
    if (w) { unsigned long lo=0, hi=0; if (sscanf(w, "%lx,%lx", &lo, &hi) == 2) { s_cw_lo=lo; s_cw_hi=hi; } }
  }
  uint32_t p = a & 0x1FFFFFFF;
  if (s_cw_hi && p >= s_cw_lo && p < s_cw_hi) {
    s_cw_n++;
    if (s_cw_n <= 64) {
      fprintf(stderr, "[cw] #%d store w%d [%08X]=%08X  interp_pc=%08X sp=%08X\n", s_cw_n, width, 0x80000000u|p, v, pc, r[29]);
      if (cfg_str("PSXPORT_CW_BT")) { void* bt[32]; int n = backtrace(bt, 32); backtrace_symbols_fd(bt, n, 2); fprintf(stderr, "----\n"); }
    }
  }
}

// Normalize any incoming addr to kernel-segment form so callers can pass either raw scratchpad
// (0x1F800xxx) or KSEG0/KSEG1 addrs and get the same behavior. wwatch_check ORs 0x80000000 into
// the store's address before comparing, so lo/hi must be in the SAME form or scratchpad watches
// silently never fire (0x1F80017C armed vs 0x9F80017C store -> miss). Pin both to KSEG1-style.
void Core::wwatch_arm(uint32_t lo, uint32_t hi) {
  s_ww_init = 1;
  s_ww_lo = lo | 0x80000000u;
  s_ww_hi = hi ? (hi | 0x80000000u) : 0u;
}

// PSXPORT_WWATCH=lo,hi — log the interpreter PC of any store landing in [lo,hi). Also fires
// storeWatchCb (programmatic arm via wwatch_arm) for the SBS write-site backtrace.
void Core::wwatch_check(uint32_t a, uint32_t v) {
  if (!s_ww_init) {
    s_ww_init = 1;
    const char* w = cfg_str("PSXPORT_WWATCH");
    if (w) { unsigned long lo=0, hi=0; if (sscanf(w, "%lx,%lx", &lo, &hi) == 2) { s_ww_lo=lo; s_ww_hi=hi; } }
  }
  uint32_t ka = a | 0x80000000u;
  if (s_ww_hi && ka >= s_ww_lo && ka < s_ww_hi) {
    if (cfg_str("PSXPORT_WWATCH"))
      fprintf(stderr, "[wwatch] core=%p store [%08X]=%08X by pc=%08X ra=%08X stage=%08X\n",
              (void*)this, ka, v, pc, r[31], mem_r32(0x801fe00c));
    if (storeWatchCb) storeWatchCb(this, ka, v);
  }
}

// Map a virtual address to a RAM/scratchpad host pointer, or NULL for I/O.
uint8_t* Core::host_ptr(uint32_t a, uint32_t bytes) {
  const uint32_t p = a & 0x1FFFFFFF;
  if (p + bytes <= 0x200000) return &ram[p];
  if (p >= 0x1F800000 && p + bytes <= 0x1F800400) return &scratch[p - 0x1F800000];
  return 0;
}

static int s_io_verbose = 0;  // PSXPORT_IO_VERBOSE=1 to log every stray access (diagnostic only)

static int dma_block_words(uint32_t bcr) {  // sync-mode-1 block DMA total word count
  uint32_t bs = bcr & 0xFFFF, bc = bcr >> 16;
  return (int)(bs * (bc ? bc : 1));
}

uint32_t Core::io_read(uint32_t a, uint32_t bytes) {
  const uint32_t p = a & 0x1FFFFFFF;
  if (p == 0x1F801814) {                           // GPUSTAT: report ready; toggle even/odd line
    io_gpustat_toggle ^= 0x80000000u;              // per-instance (Core member), not a shared static
    return 0x1C000000u | io_gpustat_toggle;
  }
  if (p >= 0x1F801800 && p <= 0x1F801803) return cdc_read(&game->cdc, p);   // CD controller registers
  if (p == 0x1F801810) return 0;                 // GPUREAD (VRAM-store path: minimal)
  if (p == 0x1F801820 || p == 0x1F801824) return mdec_read(p);  // MDEC0 data / MDEC1 status
  if (p == 0x1F801DAE) return 0;                 // SPUSTAT: report idle/transfer-complete
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
  if (s_io_verbose)
    fprintf(stderr, "[io] read%u @ 0x%08X -> 0\n", bytes * 8, a);
  return 0;
}

void Core::io_write(uint32_t a, uint32_t v, uint32_t bytes) {
  const uint32_t p = a & 0x1FFFFFFF;
  if (p >= 0x1F801800 && p <= 0x1F801803) { cdc_write(&game->cdc, p, (uint8_t)v); return; }  // CD controller
  if (p == 0x1F801810) { gpu_gp0(this, v); return; }    // GP0 (direct)
  if (p == 0x1F801814) { gpu_gp1(this, v); return; }    // GP1 (display/control)
  if (p == 0x1F801820 || p == 0x1F801824) { mdec_write(p, v); return; }  // MDEC0 cmd / MDEC1 ctrl
  if (p == 0x1F801DA6) s_spu_xfer_addr = (v & 0xFFFF) << 3;               // SPU transfer-start addr (bytes)
  if (p >= 0x1F801C00 && p <= 0x1F801FFF) { spu_write(p, v); return; }    // SPU register file
  if (p == 0x1F8010C0) { s_dma4_madr = v; return; }
  if (p == 0x1F8010C4) { s_dma4_bcr = v; return; }
  if (p == 0x1F8010C8) {                           // DMA4 CHCR: SPU-RAM transfer
    s_dma4_chcr = v;
    if (v & 0x01000000u) {
      int n = dma_block_words(s_dma4_bcr); if (n > 0x10000) n = 0x10000;
      uint32_t da = s_dma4_madr & 0x1FFFFC;
      if (v & 1) {                                 // RAM -> SPU
        for (int i = 0; i < n; i++) s_dma_buf[i] = mem_r32(da + i * 4);
        if (cfg_str("PSXPORT_SPUDMA")) {           // log VAB/sample transfers: source -> SPU dest, size
          fprintf(stderr, "[spudma] RAM 0x%08X -> SPU 0x%06X  %d words (%d B)  pc=%08X stage=%08X\n",
                  0x80000000u | da, s_spu_xfer_addr, n, n * 4, pc, mem_r32(0x801fe00c));
          if (n > 20000) {                         // big VAB bank: dump engine-range guest return addrs
            uint32_t sp = r[29] & 0x1FFFFFFF; fprintf(stderr, "  [vab-caller-chain]");
            for (uint32_t o = 0; o < 0x800 && sp + o + 4 <= 0x200000; o += 4) {
              uint32_t w; memcpy(&w, &ram[sp + o], 4); uint32_t p = w & 0x1FFFFFFF;
              if ((w & 0xFFE00000u) == 0x80000000u && (w & 3) == 0 && p >= 0x1E000 && p < 0x82000)
                fprintf(stderr, " %08X", w);
            }
            fprintf(stderr, "\n");
          }
        }
        spu_dma_write(s_dma_buf, n);
      } else {                                     // SPU -> RAM
        int got = spu_dma_read(s_dma_buf, n);
        for (int i = 0; i < got; i++) mem_w32(da + i * 4, s_dma_buf[i]);
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
      uint32_t da = s_dma0_madr & 0x1FFFFC;
      for (int i = 0; i < n; i++) s_dma_buf[i] = mem_r32(da + i * 4);
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
      uint32_t da = s_dma1_madr & 0x1FFFFC;             // copy the whole post-scatter region
      for (int i = 0; i < n; i++) mem_w32(da + i * 4, s_dma_buf[i]);
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
      if (sync == 2) gpu_dma2_linked_list(this, s_dma2_madr, /*twoDOnly=*/false);   // ordering-table linked list — full walk
      else if (sync == 1) gpu_dma2_block(this, s_dma2_madr,         // block: BC = blocks*size
               (int)((s_dma2_bcr & 0xFFFF) * (s_dma2_bcr >> 16)), to_gpu);
      else gpu_dma2_block(this, s_dma2_madr, (int)(s_dma2_bcr & 0xFFFF), to_gpu);  // immediate
      s_dma2_chcr &= ~0x01000000u;                 // clear busy -> game's DMA-done poll passes
    }
    return;
  }
  if (p == 0x1F8010E0) { s_dma6_madr = v; return; }
  if (p == 0x1F8010E4) { s_dma6_bcr = v; return; }
  if (p == 0x1F8010E8) {                           // DMA6 CHCR: OTC (ordering-table clear)
    s_dma6_chcr = v;
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
  if (s_io_verbose)
    fprintf(stderr, "[io] write%u @ 0x%08X = 0x%08X\n", bytes * 8, a, v);
}

uint8_t Core::mem_r8(uint32_t a) {
  uint8_t* p = host_ptr(a, 1);
  return p ? *p : (uint8_t)io_read(a, 1);
}
uint16_t Core::mem_r16(uint32_t a) {
  uint8_t* p = host_ptr(a, 2);
  if (!p) return (uint16_t)io_read(a, 2);
  uint16_t v; memcpy(&v, p, 2); return v;
}
uint32_t Core::mem_r32(uint32_t a) {
  uint8_t* p = host_ptr(a, 4);
  if (!p) return io_read(a, 4);
  uint32_t v; memcpy(&v, p, 4); return v;
}
// PC-native per-OBJECT depth: while armed (during one render-command dispatch, engine_submit ov_render_cmd),
// record the address span of stores landing in the packet/OT pool [0x800BFE68,0x800E7E68) (RE'd render-buffer
// map). The unowned overlay renderers write their GP0 packets HERE but WITHOUT advancing the pool pointer
// 0x800BF544 — so the pointer can't bound them, but the stores can. The span is then tagged with the object's
// world-position depth so its 2D billboard prims occlude for real at the deferred OT walk (gpu_native).
// g_pkt_track/lo/hi retired — per-Core Render::pktSpan (a real PktSpan class, method-oriented).
// Every store routes through Core::mem_w* -> pktSpan.track(addr, bytes); the class is a no-op unless
// armed by a PktSpanSession or ffspan span.
#include "render/render.h"
static inline void pkt_track(Core* c, uint32_t a, uint32_t bytes) {
  c->mRender->pktSpan.track(a, bytes);
}
void Core::mem_w8(uint32_t a, uint8_t v) {
  uint8_t* p = host_ptr(a, 1);
  wwatch_check(a, v);
  cw_check(a, v, 1); pkt_track(this, a, 1);
  if (p) *p = v; else io_write(a, v, 1);
}
void Core::mem_w16(uint32_t a, uint16_t v) {
  uint8_t* p = host_ptr(a, 2);
  wwatch_check(a, v);
  cw_check(a, v, 2); pkt_track(this, a, 2);
  if (p) memcpy(p, &v, 2); else io_write(a, v, 2);
}
void Core::mem_w32(uint32_t a, uint32_t v) {
  uint8_t* p = host_ptr(a, 4);
  wwatch_check(a, v);
  cw_check(a, v, 4); pkt_track(this, a, 4);
  if (p) memcpy(p, &v, 4); else io_write(a, v, 4);
}

// lwl/lwr/swl/swr: little-endian unaligned word merge.
uint32_t Core::mem_lwl(uint32_t cur, uint32_t a) {
  const uint32_t aligned = mem_r32(a & ~3u);
  const uint32_t sh = (a & 3) * 8;
  const uint32_t keep = (0x00FFFFFFu >> sh);
  return (cur & keep) | (aligned << (24 - sh));
}
uint32_t Core::mem_lwr(uint32_t cur, uint32_t a) {
  const uint32_t aligned = mem_r32(a & ~3u);
  const uint32_t sh = (a & 3) * 8;
  const uint32_t keep = sh ? (0xFFFFFF00u << (24 - sh)) : 0;
  return (cur & keep) | (aligned >> sh);
}
void Core::mem_swl(uint32_t a, uint32_t v) {
  const uint32_t base = a & ~3u;
  const uint32_t sh = (a & 3) * 8;
  const uint32_t aligned = mem_r32(base);
  const uint32_t keep = 0xFFFFFF00u << sh;
  mem_w32(base, (aligned & keep) | (v >> (24 - sh)));
}
void Core::mem_swr(uint32_t a, uint32_t v) {
  const uint32_t base = a & ~3u;
  const uint32_t sh = (a & 3) * 8;
  const uint32_t aligned = mem_r32(base);
  // keep = the low `sh` bits (bytes BELOW the store address, preserved by SWR). 0xFFFFFFFF>>(32-sh).
  const uint32_t keep = sh ? (0xFFFFFFFFu >> (32 - sh)) : 0;
  mem_w32(base, (aligned & keep) | (v << sh));
}

// R3000 integer division semantics (no traps; defined results for /0 and overflow).
extern "C" void cpu_div(Core* c, uint32_t n, uint32_t d) {
  int32_t sn = (int32_t)n, sd = (int32_t)d;
  if (sd == 0) { c->lo = sn < 0 ? 1u : 0xFFFFFFFFu; c->hi = (uint32_t)sn; }
  else if (n == 0x80000000u && sd == -1) { c->lo = 0x80000000u; c->hi = 0; }
  else { c->lo = (uint32_t)(sn / sd); c->hi = (uint32_t)(sn % sd); }
}
extern "C" void cpu_divu(Core* c, uint32_t n, uint32_t d) {
  if (d == 0) { c->lo = 0xFFFFFFFFu; c->hi = n; }
  else { c->lo = n / d; c->hi = n % d; }
}

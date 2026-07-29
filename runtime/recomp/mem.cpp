// Memory + CPU-support for one Core instance (OOP: these are Core methods, so the per-instance
// RAM/scratchpad/DMA state lives in `this`, reached with no global). PSX 2 MB main RAM mirrored
// across KUSEG/KSEG0/KSEG1 + 1 KB scratchpad; hardware I/O (0x1F801xxx) routes to the per-game
// peripheral modules. Host is little-endian (PSX is LE), so word access is a memcpy.
#include "core.h"
#include "game.h"
#include "recomp_iface.h"   // seam: psxport_recomp()->guestMemset_gen (generated gen_func_8009A420)
#include "cfg.h"
#include "render_substrate.h"     // RenderMode::displayPassArmed() — pc_render read-only-overlay guard
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
  cfg_logi("cw", "watch [%08X,%08X)", 0x80000000u | s_cw_lo, 0x80000000u | s_cw_hi);
}
int Core::mem_watch_hits() { return s_cw_n; }

void Core::cw_check_slow(uint32_t a, uint32_t v, int width) {
  if (!s_cw_init) {
    s_cw_init = 1;
    const char* w = cfg_str("PSXPORT_CW");
    if (w) { unsigned long lo=0, hi=0; if (sscanf(w, "%lx,%lx", &lo, &hi) == 2) { s_cw_lo=lo; s_cw_hi=hi; } }
  }
  uint32_t p = a & 0x1FFFFFFF;
  if (s_cw_hi && p >= s_cw_lo && p < s_cw_hi) {
    s_cw_n++;
    // Cap is configurable: the default 64 silently truncates exactly when the store you care about
    // happens late in a run (chasing the save-sign confirm write, every visible hit was load-time
    // noise). PSXPORT_CW_MAX=0 means unlimited.
    static int s_cw_max = -1;
    if (s_cw_max < 0) s_cw_max = cfg_int("PSXPORT_CW_MAX", 64);
    if (s_cw_max == 0 || s_cw_n <= s_cw_max) {
      cfg_logi("cw", "#%d store w%d [%08X]=%08X  interp_pc=%08X sp=%08X", s_cw_n, width, 0x80000000u|p, v, pc, r[29]);
      if (cfg_str("PSXPORT_CW_BT")) { void* bt[32]; int n = backtrace(bt, 32); backtrace_symbols_fd(bt, n, 2); cfg_logi("mem", "----"); }
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
void Core::wwatch_check_slow(uint32_t a, uint32_t v, uint32_t w) {
  if (!s_ww_init) {
    s_ww_init = 1;
    const char* w = cfg_str("PSXPORT_WWATCH");
    if (w) { unsigned long lo=0, hi=0; if (sscanf(w, "%lx,%lx", &lo, &hi) == 2) { s_ww_lo=lo; s_ww_hi=hi; } }
  }
  uint32_t ka = a | 0x80000000u;
  if (s_ww_hi && ka >= s_ww_lo && ka < s_ww_hi) {
    if (cfg_str("PSXPORT_WWATCH")) {
      extern int gpu_frame_no(Core*);
      cfg_logi("wwatch", "f%d core=%p store [%08X]=%08X by pc=%08X ra=%08X stage=%08X", gpu_frame_no(this), (void*)this, ka, v, pc, r[31], mem_r32(0x801fe00c));
      // PSXPORT_WWATCH_BT=1 — host backtrace per hit. Names the gen_func_*/native call chain even
      // for static gen-to-gen calls (where guest pc/ra go stale under native execution).
      if (cfg_str("PSXPORT_WWATCH_BT")) {
        cfg_logi("wwatch-regs", "a0=%08X a1=%08X a2=%08X a3=%08X s0=%08X s1=%08X s2=%08X s3=%08X s4=%08X s5=%08X s6=%08X s7=%08X", r[4], r[5], r[6], r[7], r[16], r[17], r[18], r[19], r[20], r[21], r[22], r[23]);
        void* bt[32]; int n = backtrace(bt, 32); backtrace_symbols_fd(bt, n, 2); cfg_logi("mem", "----");
      }
    }
    if (storeWatchCb) storeWatchCb(this, ka, v, w);
  }
}

// Map a virtual address to a RAM/scratchpad host pointer, or NULL for I/O.
uint8_t* Core::host_ptr(uint32_t a, uint32_t bytes) {
  const uint32_t p = a & 0x1FFFFFFF;
  // PSX main RAM is 2 MB, and the memory controller MIRRORS it four times across the low 8 MB of
  // each segment — 0x000000, 0x200000, 0x400000 and 0x600000 are the same storage. Games use the
  // mirror deliberately; Spider-Man (SLUS_008.75) ships a stack-top constant of 0x00800000, so its
  // crt0 computes sp = 0x807FFFF8 and the ENTIRE guest stack lives in the top mirror.
  //
  // Modelling only the first 2 MB made every such access resolve to NULL, and NULL here falls
  // through to io_write/io_read — an unmapped-I/O path with no handler for a RAM address. So the
  // accesses were SILENTLY DISCARDED: stack writes vanished and stack reads returned 0. That
  // presents as callee-saved registers being lost across calls (a saved register restored as 0),
  // which is arbitrarily far from the real cause and cost a long investigation to trace back.
  if (p < 0x800000) {
    const uint32_t off = p & 0x1FFFFF;
    if (off + bytes <= 0x200000) return &ram[off];
    return 0;                                    // straddles the mirror wrap — not a valid access
  }
  if (p >= 0x1F800000 && p + bytes <= 0x1F800400) return &scratch[p - 0x1F800000];
  return 0;
}

static uint32_t s_dma3_madr, s_dma3_bcr, s_dma3_chcr;   // DMA3: CDROM data FIFO -> RAM

static int s_io_verbose = 0;  // PSXPORT_IO_VERBOSE=1 to log every stray access (diagnostic only)

static int dma_block_words(uint32_t bcr) {  // sync-mode-1 block DMA total word count
  uint32_t bs = bcr & 0xFFFF, bc = bcr >> 16;
  return (int)(bs * (bc ? bc : 1));
}

// An address that looks like MAIN RAM but did not resolve through host_ptr is a MEMORY-MODEL DEFECT,
// not a stray peripheral poke, and the two must not share a diagnostic. An unimplemented I/O register
// reaching here is expected and routine; a RAM address reaching here means the access is being
// silently DISCARDED (writes vanish, reads return 0) while the guest believes its memory works.
//
// That distinction is not academic. PSX RAM is 2 MB mirrored across the low 8 MB, and until 2026-07-28
// host_ptr modelled only the first mirror — so a game whose stack sat in the top mirror had its ENTIRE
// stack silently dropped. The symptom (a callee-saved register restored as 0, several frames from any
// memory code) took most of a session to trace back, because the only diagnostic was an `io` verbose
// line that is off by default and indistinguishable from ordinary unmapped-peripheral chatter.
//
// Reported once per distinct address so a hot loop cannot drown the log, and NOT gated behind the io
// verbosity knob: if this fires, something is wrong that the user needs to see without knowing to ask.
static bool ram_range_addr(uint32_t p) { return p < 0x1F000000u; }   // below the I/O window == RAM space
// FAIL-FAST on an access to RAM space the memory model does not map.
//
// This used to warn once per distinct address and then DISCARD the access — writes vanished and reads
// returned 0. That is fabricating behaviour: it lets a run continue on data it did not actually
// store, so the failure surfaces arbitrarily far from its cause. It cost real debugging time on the
// Spider-Man port, where the visible end state was a module loader being handed a garbage name
// string — a pointer that had been corrupted thousands of discarded accesses earlier — plus a 1 GB
// log of warnings in 90 seconds.
//
// A discarded guest access is never recoverable, so there is nothing to trade off: abort at the FIRST
// one, with a backtrace, so the submit path that produced the address is visible. If a game
// legitimately needs an address in this range (the PSX mirrors its 2 MB of RAM four times across
// KSEG0, so 0x80800000 aliases physical 0), the fix is to MAP that range in host_ptr — not to let the
// access evaporate.
static void warn_unmapped_ram(Core* gc, uint32_t a, uint32_t bytes, const char* how) {
  const uint32_t p = a & 0x1FFFFFFF;
  if (!ram_range_addr(p)) return;
  // GUEST context, not just the host backtrace. A bad address is almost always a bad POINTER, and the
  // register that held it (plus gp, which this game uses as a table base) says far more about where it
  // came from than the C call stack does — the host frames are confounded by -foptimize-sibling-calls
  // anyway. Printing it here means the next occurrence is diagnosable from one run instead of needing
  // a bespoke probe.
  if (gc) {
    // `pc` is NOT the faulting instruction and must not be labelled as if it were. Core::pc is
    // written only by the wrapper prologue emit.py emits at each function ENTRY, and never restored
    // on return — so it names the last recompiled function that was entered, which can be a callee
    // that has already returned. It named FUN_800197A4 for a fault whose real site was inside
    // FUN_800195FC, and an investigation took that at face value. Label it for what it is.
    cfg_loge("mem", "  guest: last-fn-entered=0x%08X (NOT the faulting pc — see comment) "
                    "ra=0x%08X sp=0x%08X gp=0x%08X fp=0x%08X",
             gc->pc, gc->r[31], gc->r[29], gc->r[28], gc->r[30]);
    cfg_loge("mem", "  args : a0=0x%08X a1=0x%08X a2=0x%08X a3=0x%08X  v0=0x%08X v1=0x%08X",
             gc->r[4], gc->r[5], gc->r[6], gc->r[7], gc->r[2], gc->r[3]);
    cfg_loge("mem", "  temps: t0=0x%08X t1=0x%08X t2=0x%08X t3=0x%08X  s0=0x%08X s1=0x%08X",
             gc->r[8], gc->r[9], gc->r[10], gc->r[11], gc->r[16], gc->r[17]);
  }
  cfg_loge("mem", "\nFATAL: UNMAPPED RAM %s%u @ 0x%08X (phys 0x%08X) — fail-fast.\n"
                  "  The memory model does not map this address, so the access cannot be honoured. "
                  "Discarding it would corrupt silently.\n"
                  "  Main RAM is 2 MB (phys 0x00000000..0x001FFFFF). If the guest legitimately reaches "
                  "the KSEG0 mirrors, map them in host_ptr;\n"
                  "  if it does not, this address is the real bug and the backtrace below names the "
                  "path that produced it. Backtrace:", how, bytes * 8, a, p);
  void* bt[32]; int nbt = backtrace(bt, 32); backtrace_symbols_fd(bt, nbt, 2);
  fflush(stderr);
  abort();
}

uint32_t Core::io_read(uint32_t a, uint32_t bytes) {
  const uint32_t p = a & 0x1FFFFFFF;
  if (p == 0x1F801814) {                           // GPUSTAT: report ready; toggle even/odd line
    io_gpustat_toggle ^= 0x80000000u;              // per-instance (Core member), not a shared static
    return 0x1C000000u | io_gpustat_toggle;
  }
  if (p == 0x1F801070 || p == 0x1F801074) {       // I_STAT / I_MASK — see hle.h
    const uint32_t rv = (p == 0x1F801070) ? irqStatLatch() : game->hle.i_mask;
    // `PSXPORT_DEBUG=irq` — the interrupt controller's whole traffic. Worth a channel because both
    // of the questions this subsystem raises are invisible otherwise: whether a guest VERIFIER is
    // even reaching I_STAT (before this model existed the read fell through to unmapped I/O and
    // returned 0, so every verifier rejected and nothing said so), and whether a bit the framework
    // asserted was ever acknowledged. `ra` names the verifier or handler doing the read.
    if (cfg_dbg("irq"))
      cfg_logf("irq", "r %s = 0x%03X (stat=0x%03X mask=0x%03X) ra=%08X",
               p == 0x1F801070 ? "I_STAT" : "I_MASK", rv, game->hle.i_stat, game->hle.i_mask, r[31]);
    return rv;
  }
  if (p >= 0x1F801800 && p <= 0x1F801803) {                                 // CD controller registers
    const uint32_t rv = cdc_read(&game->cdc, p);
    irqStatLatch();   // the controller may have raised during this access; I_STAT is set THEN, not
                        // when the CPU next looks — and latching here is also what makes the edge
                        // observable on the `irq` channel in a run where nothing reads I_STAT.
    // `PSXPORT_DEBUG=cdcr` — the READ counterpart of cdcw. Without it you can see every command and
    // parameter a game writes but not whether it ever comes back for the DATA, which is the question
    // that decides how a port serves reads: a game that pops the data FIFO (register 2) transfers
    // sectors with its OWN code and only needs the FIFO filled, whereas one that never touches it is
    // taking delivery some other way and needs a destination. Distinguishing those two by reading
    // disassembly is guesswork; one run of this settles it.
    // Same pc/ra caveat as cdcw: static gen-to-gen calls do not refresh guest pc/ra, so treat them as
    // hints and use cdcbt when the caller identity matters.
    if (cfg_dbg("cdcr"))
      cfg_logf("cdcr", "r[%04X]=%02X bank=%d %s pc=%08X ra=%08X",
               (unsigned)(p & 0xFFFF), (unsigned)(rv & 0xFF), game->cdc.index,
               (p & 3) == 2 ? "DATA-FIFO" : (p & 3) == 1 ? "resp" : (p & 3) == 0 ? "status" : "irq",
               pc, r[31]);
    return rv;
  }
  if (p == 0x1F801810) return 0;                 // GPUREAD (VRAM-store path: minimal)
  if (p == 0x1F801820 || p == 0x1F801824) return mdec_read(p);  // MDEC0 data / MDEC1 status
  if (p == 0x1F801DAE) return 0;                 // SPUSTAT: report idle/transfer-complete
  if (p >= 0x1F801C00 && p <= 0x1F801FFF) return spu_read(p);    // SPU register file
  if (p == 0x1F8010B0) return s_dma3_madr;        // DMA3 CDROM->RAM
  if (p == 0x1F8010B4) return s_dma3_bcr;
  if (p == 0x1F8010B8) return s_dma3_chcr;        // busy bit already cleared by the write handler
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
  warn_unmapped_ram(this, a, bytes, "read");
  if (s_io_verbose)
    cfg_logi("io", "read%u @ 0x%08X -> 0", bytes * 8, a);
  return 0;
}

// Fold any interrupt edge the CD controller has raised since the last look into I_STAT, then return
// it. Bit 2 is EDGE-triggered on real hardware: the guest acks the CD controller at 0x1F801803 and
// acks I_STAT separately by writing a 0 to the bit, so deriving the bit LEVEL-style from "is the
// response queue non-empty" would be wrong in both directions — it would re-assert after an I_STAT
// ack and drop while a response is still pending. Called from every I_STAT read and write so the
// latch cannot be missed regardless of which the guest does first.
uint32_t Core::irqStatLatch() {
  if (game->cdc.irq_edge) {
    game->cdc.irq_edge = 0;
    game->hle.i_stat |= 1u << 2;
    pending_work |= PW_IRQ;             // arm the per-function-entry delivery gate
    if (cfg_dbg("irq"))
      cfg_logf("irq", "CD raised IRQ2 -> I_STAT=0x%03X (mask=0x%03X, %s)", game->hle.i_stat,
               game->hle.i_mask, (game->hle.i_mask & 4) ? "ENABLED" : "masked off by the guest");
  }
  return game->hle.i_stat;
}

void Core::io_write(uint32_t a, uint32_t v, uint32_t bytes) {
  const uint32_t p = a & 0x1FFFFFFF;
  if (p == 0x1F801070) {                          // I_STAT: acknowledge. A bit written as 0 is
    irqStatLatch();                             // cleared; a bit written as 1 is left alone. This
    const uint32_t before = game->hle.i_stat;     // is the PSX's semantic, NOT write-1-to-clear.
    game->hle.i_stat &= v & 0x7FFu;
    if (cfg_dbg("irq"))
      cfg_logf("irq", "w I_STAT 0x%03X: 0x%03X -> 0x%03X ra=%08X",
               v & 0x7FFu, before, game->hle.i_stat, r[31]);
    return;
  }
  if (p == 0x1F801074) {                                             // I_MASK
    game->hle.i_mask = v & 0x7FFu;
    pending_work |= PW_IRQ;             // unmasking may have made a latched bit deliverable
    if (cfg_dbg("irq")) cfg_logf("irq", "w I_MASK 0x%03X ra=%08X", game->hle.i_mask, r[31]);
    return;
  }
  if (p >= 0x1F801800 && p <= 0x1F801803) {        // CD controller
    // `PSXPORT_DEBUG=cdcw` — WHO wrote a CD register. The cdc channel (cdc_native.c) can say WHAT
    // command arrived but not where it came from: that file is plain C with no Core, so it has no
    // guest pc/ra. This is the same store one level up, where the caller context still exists, and
    // it is what turns "the model saw command 0x00" into "this guest site issued it".
    // Logged BEFORE the write so the bank shown is the one in effect when the store lands.
    // CAUTION on pc/ra below: under static gen-to-gen calls the recomp does not refresh the guest
    // pc/ra, so they can name a function that is nowhere near the store (observed: a 3-instruction
    // getter "issuing" a CD command, with ra=0 — the tell). Trust them only when ra is plausible.
    // `PSXPORT_DEBUG=cdcbt` adds a host backtrace, which names the real gen_func_* chain; that is the
    // identification to rely on.
    if (cfg_dbg("cdcw")) {
      cfg_logf("cdcw", "w[%04X]=%02X bank=%d a0=%08X s1=%08X pc=%08X ra=%08X",
               (unsigned)(p & 0xFFFF), (unsigned)(v & 0xFF), game->cdc.index, r[4], r[17], pc, r[31]);
      if (cfg_dbg("cdcbt") && (p & 3) == 1 && game->cdc.index == 0) {   // command-register write only
        void* bt[24]; int n = backtrace(bt, 24); backtrace_symbols_fd(bt, n, 2);
      }
    }
    cdc_write(&game->cdc, p, (uint8_t)v);
    irqStatLatch();   // a command usually completes here and raises IRQ2 — latch it now, not on
                        // whatever unrelated access happens to read I_STAT next.
    return;
  }
  if (p == 0x1F801810) { gpu_gp0(this, v); return; }    // GP0 (direct)
  if (p == 0x1F801814) { gpu_gp1(this, v); return; }    // GP1 (display/control)
  if (p == 0x1F801820 || p == 0x1F801824) { mdec_write(p, v); return; }  // MDEC0 cmd / MDEC1 ctrl
  if (p == 0x1F801DA6) s_spu_xfer_addr = (v & 0xFFFF) << 3;               // SPU transfer-start addr (bytes)
  if (p >= 0x1F801C00 && p <= 0x1F801FFF) { spu_write(p, v); return; }    // SPU register file
  // DMA3 — CDROM sector data -> RAM. This channel was simply ABSENT: an unmapped CHCR read returns
  // 0, which reads as "busy clear", so a guest that started a transfer and polled for completion was
  // told the transfer finished while not one byte had moved. Stock Sony libcd fetches every sector
  // this way (CD_getsector drives 0x1F8010B0/B4/B8 through its own pointer globals), so no
  // libcd-based game could read from disc at all.
  if (p == 0x1F8010B0) { s_dma3_madr = v; return; }
  if (p == 0x1F8010B4) { s_dma3_bcr  = v; return; }
  if (p == 0x1F8010B8) {
    s_dma3_chcr = v;
    if (v & 0x01000000u) {                         // start/busy
      int n = dma_block_words(s_dma3_bcr);
      if (n > 0x10000) n = 0x10000;
      const uint32_t da = s_dma3_madr & 0x1FFFFC;
      const int got = cdc_dma_read(&game->cdc, s_dma_buf, n);
      for (int i = 0; i < got; i++) mem_w32(da + i * 4, s_dma_buf[i]);
      // A short drain means the sector FIFO ran dry. Report it and write NOTHING for the missing
      // words — filling them would hand the guest fabricated data that looks like a real read.
      if (got < n)
        cfg_loge("cdc", "DMA3 underrun: guest asked %d words, FIFO held %d — the missing words were "
                        "NOT written, so this read is genuinely incomplete", n, got);
      if (cfg_dbg("cdc"))
        cfg_logf("cdc", "DMA3 %d words -> 0x%08X (head now LBA %u)", got, 0x80000000u | da,
                 game->cdc.loc_lba);
      s_dma3_chcr &= ~0x01000000u;                 // clear busy: the completion poll must pass
      irqStatLatch();                              // draining a sector queues the next INT1
      // Announce completion. Deferred to a function-entry boundary rather than dispatched here: we
      // are inside a guest store, with native code mid-mutation.
      game->cd.dma_done_pending = 1;
      pending_work |= PW_IRQ;
    }
    return;
  }
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
          cfg_logi("spudma", "RAM 0x%08X -> SPU 0x%06X  %d words (%d B)  pc=%08X stage=%08X", 0x80000000u | da, s_spu_xfer_addr, n, n * 4, pc, mem_r32(0x801fe00c));
          if (n > 20000) {                         // big VAB bank: dump engine-range guest return addrs
            uint32_t sp = r[29] & 0x1FFFFFFF;
            CfgLine ln; cfg_line_reset(&ln); cfg_line_addf(&ln, "  [vab-caller-chain]");
            for (uint32_t o = 0; o < 0x800 && sp + o + 4 <= 0x200000; o += 4) {
              uint32_t w; memcpy(&w, &ram[sp + o], 4); uint32_t p = w & 0x1FFFFFFF;
              if ((w & 0xFFE00000u) == 0x80000000u && (w & 3) == 0 && p >= 0x1E000 && p < 0x82000)
                cfg_line_addf(&ln, " %08X", w);
            }
            cfg_line_flush(&ln, "spudma");
          }
        }
        spu_dma_write(s_dma_buf, n);
      } else {                                     // SPU -> RAM
        int got = spu_dma_read(s_dma_buf, n);
        for (int i = 0; i < got; i++) mem_w32(da + i * 4, s_dma_buf[i]);
      }
      s_dma4_chcr &= ~0x01000000u;
      // SPU transfer complete. On hardware this raises IRQ9 and the BIOS turns it into
      // DeliverEvent(HwSPU) — which is what a guest waiting on the transfer is testing for. This
      // model performs the DMA SYNCHRONOUSLY, so the event is due the moment the words have moved;
      // announcing it here is the faithful equivalent, not an invented completion.
      //
      // Without it, a guest that opens the SPU event and polls TestEvent (the ordinary way to wait
      // for a VAB/sample upload) spins forever, several frames of init after the upload actually
      // finished — with nothing in any log pointing at the SPU.
      // 0xF0000009 is the PSX's fixed HwSPU class, hardware nomenclature rather than a game value,
      // so it belongs here rather than in GameConfig. The spec mask is left wide because the class
      // identifies the source; the guest's own OpenEvent spec decides which slots match.
      game->hle.deliverEvent(0xF0000009u, 0xFFFFFFFFu);
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
      if (sync == 2) gpu_dma2_linked_list(this, s_dma2_madr);   // ordering-table linked list — full walk
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
  warn_unmapped_ram(this, a, bytes, "write");
  if (s_io_verbose)
    cfg_logi("io", "write%u @ 0x%08X = 0x%08X", bytes * 8, a, v);
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
// OT/GTE submission attribution (`debug otattr`, game/render/ot_attr.h): attribute a packet-pool store
// to the current otattr-shadow-stack fn + render-walk node. No-op unless the channel is on.
static inline void pkt_track(Core* c, uint32_t a, uint32_t bytes) {
  c->rsub.otAttr.trackStore(c, a, bytes);
}

// Mirror-verify write journal (game/core/verify_harness.h): while a strictCheck invocation is
// in-flight (PSXPORT_MIRROR_VERIFY), every MAIN-RAM store's pre-write byte gets journaled (once per
// byte per invocation — dirty-bitmap gated) so strictCheck can snapshot/rewind/compare only the
// touched bytes instead of the whole 2 MB buffer. Scratchpad stores are NOT journaled — strictCheck
// always full-compares the 1 KB scratchpad (already cheap). `!armed` is the hot-path no-op: a single
// bool read gates everything else, so this is free when MIRROR_VERIFY is unset (matches the existing
// display_pass_write_guard/pkt_track pattern on this same path).
static inline void journal_track(Core* c, uint8_t* p, uint32_t bytes) {
  if (!c->game || !c->game->verify.journalIsArmed()) return;
  if (p < c->ram || p + bytes > c->ram + 0x200000) return;   // not main RAM (scratchpad/none) — skip
  uint32_t off = (uint32_t)(p - c->ram);
  for (uint32_t i = 0; i < bytes; i++) c->game->verify.journalTrack(off + i, p[i]);
}

// FAIL-FAST guard for the pc_render READ-ONLY-OVERLAY invariant (CLAUDE.md): pc_render's native
// display pass (DisplayPassGuard-armed scope in game_tomba2.cpp's Engine::drawOTag) reads guest RAM
// + engine state and draws to HOST memory only — it must NEVER write guest main RAM or scratchpad.
// Checked at the top of every guest store; the `!armed` branch is the hot-path no-op (single
// predicted-false bool read gates the address test, so this is cheap when the flag is off, which is
// always except during the pc_render display pass on this Core).
extern "C" void guest_backtrace_to(Core*, FILE*);   // sync_overrides.cpp — guest-stack backtrace
static void display_pass_write_guard(Core* c, uint32_t a, uint32_t v, int width) {
  if (!c->rsub.mode.displayPassArmed()) return;
  const uint32_t p = a & 0x1FFFFFFF;
  const bool guest_ram   = p < 0x200000;
  const bool scratchpad  = p >= 0x1F800000 && p < 0x1F800400;
  if (!guest_ram && !scratchpad) return;
  cfg_logi("mem", "\n[pc_render VIOLATION] guest write to 0x%08X (val 0x%X, width %d) during pc_render display pass — pc_render MUST be a read-only overlay (CLAUDE.md). interp_pc=%08X sp=%08X", 0x80000000u | p, v, width, c->pc, c->r[29]);
  guest_backtrace_to(c, stderr);
  fflush(stderr);
  abort();
}

void Core::mem_w8(uint32_t a, uint8_t v) {
  uint8_t* p = host_ptr(a, 1);
  display_pass_write_guard(this, a, v, 1);
  wwatch_check(a, v, 1);
  cw_check(a, v, 1); pkt_track(this, a, 1);
  if (p) journal_track(this, p, 1);
  if (p) *p = v; else io_write(a, v, 1);
}
void Core::mem_w16(uint32_t a, uint16_t v) {
  uint8_t* p = host_ptr(a, 2);
  display_pass_write_guard(this, a, v, 2);
  wwatch_check(a, v, 2);
  cw_check(a, v, 2); pkt_track(this, a, 2);
  if (p) journal_track(this, p, 2);
  if (p) memcpy(p, &v, 2); else io_write(a, v, 2);
}
void Core::mem_w32(uint32_t a, uint32_t v) {
  uint8_t* p = host_ptr(a, 4);
  display_pass_write_guard(this, a, v, 4);
  wwatch_check(a, v, 4);
  cw_check(a, v, 4); pkt_track(this, a, 4);
  if (p) journal_track(this, p, 4);
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

// 0x8009A420 FUN_8009A420 — psyq libc `memset`, statically linked into MAIN.EXE (confirmed by
// its position immediately below `rand` at 0x8009A450 in the psyq libc block, per
// docs/engine_re.md). WIRED 2026-07-16 (guest_memset_install below; the pool.cpp callsite's
// rec_dispatch routes here). §9 re-verify against gen_func_8009A420 (generated/shard_1.c:19508)
// found ONE draft bug, fixed: dst NULL -> return 0; **n<=0 -> ALSO return 0** (the gen delay slot
// loads r2=dst but the fall-through overwrites it with 0 — the earlier draft note claiming
// "return dst as-is" was wrong); else byte-fill [dst, dst+n) and return the ORIGINAL dst (the
// guest loop advances a copy of a0, never the value it returns). Leaf, no stack frame; a0/a2 are
// caller-saved so their gen-side advance needs no mirror.
uint32_t Core::guestMemset(uint32_t dst, uint8_t val, int32_t n) {
  if (dst == 0u) return 0u;
  if (n <= 0) return 0u;
  uint32_t cur = dst;
  for (int32_t i = 0; i < n; i++, cur++) mem_w8(cur, val);
  return dst;
}

// Override wrapper + install for the memset leaf (guest ABI: a0=dst, a1=val, a2=n, v0=return).
namespace {
void ov_guestMemset(Core* c) {
  const uint32_t dst = c->r[4];
  const int32_t  n   = (int32_t)c->r[6];
  c->r[2] = c->guestMemset(dst, (uint8_t)c->r[5], n);
  // Mirror gen's exit clobbers of the (caller-saved) a0/a2: the fill loop leaves r4 = dst+n and
  // r6 = 0. Callers may spill them before reassigning — a stale value there is a guest-stack
  // divergence (wave-3 lesson: live registers at call/return boundaries are part of the state).
  if (dst != 0u && n > 0) { c->r[4] = dst + (uint32_t)n; c->r[6] = 0u; }
}
}
void guest_memset_install() {
  static bool done = false;
  if (done) return;
  done = true;
  // The guest-memset gen body (generated gen_func_8009A420) is reached through the RecompRegistry seam
  // (recomp_iface.h), so the framework names no generated symbol.
  extern void engine_set_override_main(uint32_t, OverrideFn, OverrideFn);
  engine_set_override_main(0x8009A420u, ov_guestMemset, psxport_recomp()->guestMemset_gen);
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

// Memory + CPU-support for one Core instance (OOP: these are Core methods, so the per-instance
// RAM/scratchpad/DMA state lives in `this`, reached with no global). PSX 2 MB main RAM mirrored
// across KUSEG/KSEG0/KSEG1 + 1 KB scratchpad; hardware I/O (0x1F801xxx) routes to the per-game
// peripheral modules. Host is little-endian (PSX is LE), so word access is a memcpy.
#include "cfg.h"
#include "core.h"
#include "dma_irq.h" // DPCR/DICR semantics — which DMA completions the guest hears about
#include "game.h"
#include "recomp_iface.h"     // seam: psxport_recomp()->guestMemset_gen (generated gen_func_8009A420)
#include "render_substrate.h" // RenderMode::displayPassArmed() — pc_render read-only-overlay guard
#include <execinfo.h>
#include <lucent/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Subsystem entry points reached from the I/O map. gpu_dma2_* read/write this instance's RAM and
// take the Core (declared in core.h); the rest operate on words/buffers handed to them.
// gpu_gp0/gpu_gp1 live in the C++ renderer (gpu_native.cpp); gpu_gp0 takes the Core (CPU->VRAM
// uploads + the DMA linked-list walk read this instance's RAM).
void gpu_gp0(Core *core, uint32_t w);
void gpu_gp1(Core *, uint32_t w);
uint32_t gpu_read_word(Core *core); // GPUREAD drain of an armed GP0(0xC0) VRAM->CPU readback
extern "C" {
#include "cdc_state.h"
void mdec_write(uint32_t addr, uint32_t val);
uint32_t mdec_read(uint32_t addr);
int mdec_dma_in(const uint32_t *words, int count); // returns words actually fed (short = deferral)
uint32_t mdec_dma_read_word(uint32_t *offs);       // pop one OutFIFO word + its scatter displacement
void mdec_step(void);                              // run the decoder until it parks at a FIFO gate
bool mdec_dma_can_write(void);
bool mdec_dma_can_read(void);
void spu_write(uint32_t addr, uint32_t val);
uint32_t spu_read(uint32_t addr);
void spu_dma_write(const uint32_t *words, int count);
int spu_dma_read(uint32_t *words, int count);
}

// Core::Core / ~Core moved to runtime/recomp/core.cpp (lifetime + subsystem wiring live there).

// PSXPORT_CW="lo,hi" — host-backtrace watchpoint: when ANY store lands in physical byte range
// [lo,hi), dump a C backtrace. Finds runtime code that clobbers a region the decompressor wrote.
void Core::mem_set_watch(uint32_t lo, uint32_t hi) {
  s_cw_init = 1;
  s_cw_lo = lo & 0x1FFFFFFF;
  s_cw_hi = hi & 0x1FFFFFFF;
  s_cw_n = 0;
  lucent::info("cw", "watch [{:08X},{:08X})", 0x80000000u | s_cw_lo, 0x80000000u | s_cw_hi);
}
int Core::mem_watch_hits() {
  return s_cw_n;
}

void Core::cw_check_slow(uint32_t a, uint32_t v, int width) {
  if (!s_cw_init) {
    s_cw_init = 1;
    const char *w = cfg_str("PSXPORT_CW");
    if (w) {
      unsigned long lo = 0, hi = 0;
      if (sscanf(w, "%lx,%lx", &lo, &hi) == 2) {
        s_cw_lo = lo;
        s_cw_hi = hi;
      }
    }
  }
  uint32_t p = a & 0x1FFFFFFF;
  if (s_cw_hi && p >= s_cw_lo && p < s_cw_hi) {
    s_cw_n++;
    // Cap is configurable: the default 64 silently truncates exactly when the store you care about
    // happens late in a run (chasing the save-sign confirm write, every visible hit was load-time
    // noise). PSXPORT_CW_MAX=0 means unlimited.
    static int s_cw_max = -1;
    if (s_cw_max < 0) {
      s_cw_max = cfg_int("PSXPORT_CW_MAX", 64);
    }
    if (s_cw_max == 0 || s_cw_n <= s_cw_max) {
      lucent::info("cw",
                   "#{} store w{} [{:08X}]={:08X}  interp_pc={:08X} sp={:08X}",
                   s_cw_n,
                   width,
                   0x80000000u | p,
                   v,
                   pc,
                   r[29]);
      if (cfg_str("PSXPORT_CW_BT")) {
        void *bt[32];
        int n = backtrace(bt, 32);
        backtrace_symbols_fd(bt, n, 2);
        lucent::info("mem", "----");
      }
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
    const char *w = cfg_str("PSXPORT_WWATCH");
    if (w) {
      unsigned long lo = 0, hi = 0;
      if (sscanf(w, "%lx,%lx", &lo, &hi) == 2) {
        s_ww_lo = lo;
        s_ww_hi = hi;
      }
    }
  }
  uint32_t ka = a | 0x80000000u;
  if (s_ww_hi && ka >= s_ww_lo && ka < s_ww_hi) {
    if (cfg_str("PSXPORT_WWATCH")) {
      extern int gpu_frame_no(Core *);
      lucent::info("wwatch",
                   "f{} core={} store [{:08X}]={:08X} by pc={:08X} ra={:08X} stage={:08X}",
                   gpu_frame_no(this),
                   (void *)this,
                   ka,
                   v,
                   pc,
                   r[31],
                   mem_r32(0x801fe00c));
      // PSXPORT_WWATCH_BT=1 — host backtrace per hit. Names the gen_func_*/native call chain even
      // for static gen-to-gen calls (where guest pc/ra go stale under native execution).
      if (cfg_str("PSXPORT_WWATCH_BT")) {
        lucent::info("wwatch-regs",
                     "a0={:08X} a1={:08X} a2={:08X} a3={:08X} s0={:08X} s1={:08X} s2={:08X} s3={:08X} s4={:08X} "
                     "s5={:08X} s6={:08X} s7={:08X}",
                     r[4],
                     r[5],
                     r[6],
                     r[7],
                     r[16],
                     r[17],
                     r[18],
                     r[19],
                     r[20],
                     r[21],
                     r[22],
                     r[23]);
        void *bt[32];
        int n = backtrace(bt, 32);
        backtrace_symbols_fd(bt, n, 2);
        lucent::info("mem", "----");
      }
    }
    if (storeWatchCb) {
      storeWatchCb(this, ka, v, w);
    }
  }
}

// Map a virtual address to a RAM/scratchpad host pointer, or NULL for I/O.
uint8_t *Core::host_ptr(uint32_t a, uint32_t bytes) {
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
    if (off + bytes <= 0x200000) {
      return &ram[off];
    }
    return 0; // straddles the mirror wrap — not a valid access
  }
  if (p >= 0x1F800000 && p + bytes <= 0x1F800400) {
    return &scratch[p - 0x1F800000];
  }
  return 0;
}

static uint32_t s_dma3_madr, s_dma3_bcr, s_dma3_chcr; // DMA3: CDROM data FIFO -> RAM

// DPCR (0x1F8010F0) and DICR (0x1F8010F4) — the DMA controller's channel-enable and interrupt
// registers. Rules in dma_irq.h; state here, alongside the per-channel registers it governs.
//
// DPCR is storage only: no transfer is gated on it. That is deliberate and measured, not laziness —
// two of the three consuming ports never write DPCR at all, so a channel-enable gate would read
// their untouched 0 as "every channel disabled" and stop every DMA in the port. Its power-on value
// is published so a guest that read-modify-writes it (which is the only way it is ever used) sees
// the other channels' bits rather than zeros.
static uint32_t s_dpcr = DPCR_RESET;
static uint32_t s_dicr = 0;

// Channels that finished a transfer the guest armed and have not had their callback run yet.
// Deferred to a guest FUNCTION-ENTRY boundary (Hle::irqPoll), never dispatched from inside the store
// that finished the transfer: native runtime code is mid-mutation there, and re-entering guest code
// from a store is how a controller gets re-entered halfway through a command.
static DmaDone s_dma_done;

void dma_irq_ack(int ch) {
  s_dicr &= ~(1u << (24 + ch));
}
bool dma_done_owed(int ch) {
  return s_dma_done.owed(ch);
}
void dma_done_taken(int ch) {
  s_dma_done.taken(ch);
}
bool dma_done_any() {
  return s_dma_done.mask != 0;
}

// One completion point per channel, all identical: the DICR gate decides, never the channel number.
// (mem.cpp is where the transfers finish, so this is where the set is populated.)
// Returns true if the guest is now owed a callback, so the caller can raise its deferred-work bit
// (the completion sites are all Core methods; this is deliberately not one, so it cannot touch
// anything else on the Core by accident).
static bool dma_completed(int ch) {
  const bool owed = s_dma_done.complete(s_dicr, ch);
  lucent::debug("dmairq", "DMA{} complete: armed={} (DICR {:08X})", ch, owed ? 1 : 0, dma_dicr_read(s_dicr));
  return owed;
}

static int dma_block_words(uint32_t bcr) { // sync-mode-1 block DMA total word count
  uint32_t bs = bcr & 0xFFFF, bc = bcr >> 16;
  return (int)(bs * (bc ? bc : 1));
}

// MDEC pending-channel pump — the ping-pong real hardware gets from DMA0 (MDEC-in) and DMA1
// (MDEC-out) running CONCURRENTLY around the decoder. In vendor beetle-psx dma.c both channels sit
// with CHCR bit 24 set and each block transfer is gated by ChCan() — MDEC_DMACanWrite() for CH0,
// MDEC_DMACanRead() for CH1 — so DMA0 stalls while a macroblock decodes and DMA1 drains the output
// that lets the next macroblock retire. This model's transfers are synchronous CHCR-write handlers,
// which made each transfer atomic: whichever channel started first ran to completion before the
// other could begin, and a whole-frame decode (whose output is far larger than the 0x20-word
// OutFIFO) could NEVER complete — the measured RE-03c wedge ("6 of 1824 words written,
// outfifo_has_data=1"). The fix is the missing hardware concept, a PENDING channel: a start that
// cannot complete latches its remainder (busy stays set, as hardware shows), and this pump moves
// both channels forward together. It is called from both CHCR starts and from every guest-visible
// poll (DMA0/DMA1 CHCR reads, MDEC status/data-port reads, MDEC port writes), so every guest spin
// loop drives progress instead of observing a frozen model.
void Core::mdec_dma_pump() {
  for (;;) {
    bool progress = false;

    // DMA0 (MDEC-in): feed the pending remainder while the decoder can accept. MDEC_DMACanWrite()
    // is true only when >= 0x20 InFIFO words are free (i.e. the FIFO is empty), so a 0x10-word
    // chunk is never silently dropped — the same constraint native_fmv.cpp documents and relies on.
    while (s_mdec0_left > 0 && mdec_dma_can_write()) {
      uint32_t chunk_buf[0x10];
      int chunk = s_mdec0_left < 0x10 ? s_mdec0_left : 0x10;
      for (int k = 0; k < chunk; k++) {
        chunk_buf[k] = mem_r32((s_mdec0_addr + (uint32_t)k * 4) & 0x1FFFFCu);
      }
      int fed = mdec_dma_in(chunk_buf, chunk);
      s_mdec0_addr = (s_mdec0_addr + (uint32_t)fed * 4) & 0xFFFFFFu;
      s_mdec0_left -= fed;
      if (fed > 0) {
        progress = true;
      }
      if (fed < chunk) {
        break; // decoder refused mid-chunk: fall through to classify
      }
    }

    // DMA1 (MDEC-out): drain in hardware-shaped bursts. Real DMA1 re-checks MDEC_DMACanRead() once
    // per BLOCK (WordCounter reload, vendor dma.c) and then transfers the whole block, so the gate
    // is per-burst, not per-word. Each word is stored EXACTLY as dma.c CH_MDEC_OUT stores it:
    //     MainRAM[(CurAddr + (voffs << 2)) & 0x1FFFFC] = word;   CurAddr = (CurAddr + 4) & 0xFFFFFF;
    // i.e. dest_word(i) = MADR_word + i + (int32)voffs_i, with i cumulative over THIS channel's
    // transfer (the voffs placement contract, mdec_beetle.c). Per-word DIRECT stores — never the
    // old zero-fill-stage-then-copy-all-n, which on a partial drain wrote zeros over guest RAM at
    // every position the scatter's forward reach had not covered yet.
    while (s_mdec1_left > 0 && mdec_dma_can_read()) {
      int bs = (int)(s_dma1_bcr & 0xFFFF);
      if (bs == 0) {
        bs = 0x10000;
      }
      int burst = bs < s_mdec1_left ? bs : s_mdec1_left;
      for (int k = 0; k < burst; k++) {
        uint32_t offs = 0;
        uint32_t val = mdec_dma_read_word(&offs);
        mem_w32((s_mdec1_addr + (offs << 2)) & 0x1FFFFCu, val);
        s_mdec1_addr = (s_mdec1_addr + 4) & 0xFFFFFFu;
      }
      s_mdec1_left -= burst;
      progress = true;
    }

    // Channel completion: busy (bit 24) clears ONLY when the word count is exhausted — the same
    // moment hardware clears it. (DecDCTinSync-style waits poll MDEC1 status bit 29 = InCommand,
    // which Beetle derives itself, so completion needs no extra signalling here. No DMA IRQ is due
    // on these two channels: DICR is modelled now (dma_irq.h) and nothing in the runtime turns an
    // MDEC channel completion into a guest callback, so arming them would have nothing to signal.)
    if (s_mdec0_left == 0 && (s_dma0_chcr & 0x01000000u)) {
      s_dma0_chcr &= ~0x01000000u;
      lucent::debug("mdecdma", "DMA0(in)  complete");
      if (dma_completed(0)) {
        pending_work |= PW_IRQ;
      }
    }
    if (s_mdec1_left == 0 && (s_dma1_chcr & 0x01000000u)) {
      s_dma1_chcr &= ~0x01000000u;
      lucent::debug("mdecdma", "DMA1(out) complete");
      if (dma_completed(1)) {
        pending_work |= PW_IRQ;
      }
    }
    if (s_mdec0_left == 0 && s_mdec1_left == 0) {
      return; // nothing pending
    }
    if (progress) {
      continue;
    }

    // No progress with work still pending: hand the decoder its clock budget once (the state
    // machine only moves inside MDEC_Run; one call runs it to the next genuine FIFO gate) and
    // classify what it parked at. A silent return here would be the old bug wearing a new coat, so
    // every exit below is either a TRACED deferral (guest action will resume it) or a LOUD wedge.
    mdec_step();
    if ((s_mdec0_left > 0 && mdec_dma_can_write()) || (s_mdec1_left > 0 && mdec_dma_can_read())) {
      continue; // the step freed a FIFO — keep pumping
    }

    const uint32_t st = mdec_read(0x1F801824u); // MDEC1 status
    const int in_cmd = (st >> 29) & 1;          // bit 29: a decode command is consuming input
    const int out_has = !((st >> 31) & 1);      // bit 31 clear: OutFIFO non-empty

    if (s_mdec0_left > 0 && !in_cmd && (st & 0xFFFF) == 0xFFFF) {
      // The decode command RETIRED (InCounter wrapped to 0xFFFF) with input still pending: the
      // guest oversized DMA0 against the command's word count, and MDEC_DMACanWrite requires
      // InCommand, so this transfer can never complete. Hardware would hang the channel busy
      // forever; keep that state (a later command write re-pumps and may legally consume it),
      // but say so ONCE — never spin on it, never fake completion.
      if (!s_mdec_stall_reported) {
        s_mdec_stall_reported = 1;
        lucent::error("mdec",
                      "DMA0 in: {} word(s) pending @{:08X} but the decode command has retired "
                      "(InCommand clear, InCounter=0xFFFF) — input remainder outlives the "
                      "command; transfer cannot complete. MDEC1 status={:08X}",
                      s_mdec0_left,
                      s_mdec0_addr,
                      st);
      }
      return;
    }
    // Deferral traces fire on state CHANGE only (reason + pending count), never per poll: the
    // measured guest abandons its final DMA0 remainder without a forced stop and then touches the
    // MDEC/DMA registers millions of times over a session — a per-poll trace wrote a 654 MB log.
    if (s_mdec0_left > 0 && !in_cmd) {
      // No decode command in flight (fresh reset / between commands): a command write to the MDEC
      // port resumes this via the port-write pump point. Faithful wait, traced.
      const uint32_t note = 0x01000000u | (uint32_t)s_mdec0_left;
      if (s_mdec_defer_note != note) { // state CHANGE, not per poll — see above
        s_mdec_defer_note = note;
        lucent::debug("mdecdma", "DMA0(in)  deferred: {} word(s) pending, no decode command in flight", s_mdec0_left);
      }
      return;
    }
    if (s_mdec0_left > 0 && s_mdec1_left == 0 && out_has) {
      // FAITHFUL DEFERRAL — the measured live path (DMA0-first): the decoder is blocked on output
      // and the guest has not started DMA1 yet. DMA0 stays busy/pending; the DMA1 start (or any
      // guest poll) resumes it. On hardware this is exactly the DMA0 stall in dma.c's ping-pong.
      const uint32_t note = 0x02000000u | (uint32_t)s_mdec0_left;
      if (s_mdec_defer_note != note) { // state CHANGE, not per poll — see above
        s_mdec_defer_note = note;
        lucent::debug("mdecdma", "DMA0(in)  deferred: {} word(s) pending, decoder blocked on output", s_mdec0_left);
      }
      return;
    }
    if (s_mdec1_left > 0 && s_mdec0_left == 0) {
      // Deferral: output has not reached a DMA burst (>= 0x20 words) and there is no pending input
      // to make more. Either the guest feeds another DMA0 (which resumes this), or it oversized
      // DMA1 against the frame — in which case hardware would hang busy identically and the
      // guest's own sync timeout is the correct observer.
      const uint32_t note = 0x03000000u | (uint32_t)s_mdec1_left;
      if (s_mdec_defer_note != note) { // state CHANGE, not per poll — see above
        s_mdec_defer_note = note;
        lucent::debug("mdecdma", "DMA1(out) deferred: {} word(s) pending, awaiting decoder output", s_mdec1_left);
      }
      return;
    }
    // Work pending on both sides (or input-side with an active command), gates still shut after a
    // full clock budget: a genuine wedge — decoder state bug, or DMA enable bits (Control 30/29)
    // dropped mid-transfer. Report ONCE per latched start, with both pending counts and the raw
    // status so the two causes are distinguishable. Never silently.
    if (!s_mdec_stall_reported) {
      s_mdec_stall_reported = 1;
      lucent::error("mdec",
                    "DMA pump wedged: no progress possible. DMA0 {} word(s) pending @{:08X}, "
                    "DMA1 {} word(s) pending @{:08X}, MDEC1 status={:08X} (InCommand={} "
                    "outfifo_has_data={})",
                    s_mdec0_left,
                    s_mdec0_addr,
                    s_mdec1_left,
                    s_mdec1_addr,
                    st,
                    in_cmd,
                    out_has);
    }
    return;
  }
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
static bool ram_range_addr(uint32_t p) {
  return p < 0x1F000000u;
} // below the I/O window == RAM space

// A bad address is almost always a bad POINTER, and when the guest is in string-handling code (a
// module/file lookup, an asset name) the STRING the pointers refer to is the single most identifying
// piece of evidence at the fault — it names WHICH asset the run died on, which no register value can.
//
// Capturing it at the PRODUCER is what does not work. On the Spider-Man port the producer builds the
// name in a stack buffer, and both obvious probes are disqualified there for opposite reasons: an
// override or entry probe changes the call-chain shape and the fault disappears, while a store
// watchpoint over the stack window logged ~9M stores and starved the run before it ever reached the
// fault. The CONSUMER is free of both problems — this reporter fires exactly once, at the moment of
// interest, with the registers still live, so it cannot flood and it perturbs nothing.
//
// The NEGATIVE is the part that has to carry information. "No strings" printed bare would be
// indistinguishable from "this code never looked", so the report always states its denominator (how
// many GPRs were examined, how many resolved into mapped RAM) and its blind spot: it can only see a
// string whose pointer is STILL LIVE in a GPR at fault time. A name that was built in a stack buffer
// whose pointer has already been overwritten is invisible here, and that must not read as "there was
// no name".
void Core::dumpStringishRegs() {
  Core *const gc = this;
  static const char *kReg[32] = {"zero", "at", "v0", "v1", "a0", "a1", "a2", "a3", "t0", "t1", "t2",
                                 "t3",   "t4", "t5", "t6", "t7", "s0", "s1", "s2", "s3", "s4", "s5",
                                 "s6",   "s7", "t8", "t9", "k0", "k1", "gp", "sp", "fp", "ra"};
  int examined = 0, mapped = 0, printed = 0;
  for (int i = 1; i < 32; i++) {
    if (i == 28 || i == 29 || i == 30) {
      continue; // gp/sp/fp are bases, not string pointers
    }
    examined++;
    const uint32_t a = gc->r[i];
    uint8_t *p = gc->host_ptr(a, 1);
    if (!p) {
      continue;
    }
    mapped++;
    // Printable, NUL-terminated, and long enough that random bytes are unlikely to qualify. Bounded
    // by what host_ptr will still resolve, so the scan cannot walk off the end of the mirror.
    char buf[64];
    int n = 0;
    while (n < 63) {
      uint8_t *q = gc->host_ptr(a + (uint32_t)n, 1);
      if (!q) {
        break;
      }
      const uint8_t c = *q;
      if (c == 0) {
        break;
      }
      if (c < 0x20 || c > 0x7E) {
        n = 0;
        break;
      } // non-printable => not a string, reject outright
      buf[n++] = (char)c;
    }
    // A rejected pointer is not nothing. The first run of this instrument answered "0 looked like C
    // strings" for a fault whose whole question was *which name was passed*, with the suspected name
    // pointer (`s1`) sitting right there in the mapped list — so "rejected" alone left the question
    // exactly where it was. Dump the bytes for every mapped GPR instead: it fires once, it is 20-odd
    // lines, and it distinguishes "the pointer is stale/garbage" from "the string is there but the
    // classifier is too strict", which the verdict alone cannot.
    if (n >= 4) {
      buf[n] = 0;
      lucent::error("mem", "  string: {}=0x{:08X} -> \"{}\"", kReg[i], a, buf);
      printed++;
    }
    char hex[16 * 3 + 1], asc[17];
    int hn = 0, an = 0;
    for (int k = 0; k < 16; k++) {
      uint8_t *q = gc->host_ptr(a + (uint32_t)k, 1);
      if (!q) {
        break;
      }
      hn += snprintf(hex + hn, sizeof hex - (size_t)hn, "%02X ", *q);
      asc[an++] = (*q >= 0x20 && *q <= 0x7E) ? (char)*q : '.';
    }
    hex[hn] = 0;
    asc[an] = 0;
    lucent::error("mem", "  bytes : {:<4}=0x{:08X}  {:<48} |{}|", kReg[i], a, hex, asc);
  }
  lucent::error("mem",
                "  strings: examined {} GPRs (excl gp/sp/fp), {} resolved into mapped RAM, "
                "{} looked like C strings. BLIND SPOT: only sees a string whose pointer is still "
                "live in a GPR here — one reached via a stack buffer or a struct field is NOT "
                "visible, so {} does not mean there was no name.",
                examined,
                mapped,
                printed,
                printed ? "this list" : "an empty list");
}
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
static void warn_unmapped_ram(Core *gc, uint32_t a, uint32_t bytes, const char *how) {
  const uint32_t p = a & 0x1FFFFFFF;
  if (!ram_range_addr(p)) {
    return;
  }
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
    lucent::error("mem",
                  "  guest: last-fn-entered=0x{:08X} (NOT the faulting pc — see comment) "
                  "ra=0x{:08X} sp=0x{:08X} gp=0x{:08X} fp=0x{:08X}",
                  gc->pc,
                  gc->r[31],
                  gc->r[29],
                  gc->r[28],
                  gc->r[30]);
    lucent::error("mem",
                  "  args : a0=0x{:08X} a1=0x{:08X} a2=0x{:08X} a3=0x{:08X}  v0=0x{:08X} v1=0x{:08X}",
                  gc->r[4],
                  gc->r[5],
                  gc->r[6],
                  gc->r[7],
                  gc->r[2],
                  gc->r[3]);
    lucent::error("mem",
                  "  temps: t0=0x{:08X} t1=0x{:08X} t2=0x{:08X} t3=0x{:08X}  s0=0x{:08X} s1=0x{:08X}",
                  gc->r[8],
                  gc->r[9],
                  gc->r[10],
                  gc->r[11],
                  gc->r[16],
                  gc->r[17]);
    gc->dumpStringishRegs();
  }
  lucent::error("mem",
                "\nFATAL: UNMAPPED RAM {}{} @ 0x{:08X} (phys 0x{:08X}) — fail-fast.\n"
                "  The memory model does not map this address, so the access cannot be honoured. "
                "Discarding it would corrupt silently.\n"
                "  Main RAM is 2 MB (phys 0x00000000..0x001FFFFF). If the guest legitimately reaches "
                "the KSEG0 mirrors, map them in host_ptr;\n"
                "  if it does not, this address is the real bug and the backtrace below names the "
                "path that produced it. Backtrace:",
                how,
                bytes * 8,
                a,
                p);
  void *bt[32];
  int nbt = backtrace(bt, 32);
  backtrace_symbols_fd(bt, nbt, 2);
  fflush(stderr);
  abort();
}

uint32_t Core::io_read(uint32_t a, uint32_t bytes) {
  const uint32_t p = a & 0x1FFFFFFF;
  if (p == 0x1F801110) { // root counter 1: HBlank-clocked free-running 16-bit counter
    return game->timing.hSyncCounter();
  }
  if (p == 0x1F801814) {              // GPUSTAT: report ready; toggle even/odd line
    io_gpustat_toggle ^= 0x80000000u; // per-instance (Core member), not a shared static
    return 0x1C000000u | io_gpustat_toggle;
  }
  if (p == 0x1F801070 || p == 0x1F801074) { // I_STAT / I_MASK — see hle.h
    const uint32_t rv = (p == 0x1F801070) ? irqStatLatch() : game->hle.i_mask;
    // `PSXPORT_DEBUG=irq` — the interrupt controller's whole traffic. Worth a channel because both
    // of the questions this subsystem raises are invisible otherwise: whether a guest VERIFIER is
    // even reaching I_STAT (before this model existed the read fell through to unmapped I/O and
    // returned 0, so every verifier rejected and nothing said so), and whether a bit the framework
    // asserted was ever acknowledged. `ra` names the verifier or handler doing the read.
    lucent::debug("irq",
                  "r {} = 0x{:03X} (stat=0x{:03X} mask=0x{:03X}) ra={:08X}",
                  p == 0x1F801070 ? "I_STAT" : "I_MASK",
                  rv,
                  game->hle.i_stat,
                  game->hle.i_mask,
                  r[31]);
    return rv;
  }
  if (p >= 0x1F801800 && p <= 0x1F801803) { // CD controller registers
    const uint32_t rv = cdc_read(&game->cdc, p);
    irqStatLatch(); // the controller may have raised during this access; I_STAT is set THEN, not
                    // when the CPU next looks — and latching here is also what makes the edge
                    // observable on the `irq` channel in a run where nothing reads I_STAT.
    // `PSXPORT_DEBUG=cdcr` logs reads: register 2 means guest-owned FIFO transfer; a game that never
    // touches it takes delivery elsewhere. This distinction is measured, not guessed from disassembly.
    // Same pc/ra caveat as cdcw: static gen-to-gen calls do not refresh guest pc/ra, so treat them as
    // hints and use cdcbt when the caller identity matters.
    lucent::debug("cdcr",
                  "r[{:04X}]={:02X} bank={} {} pc={:08X} ra={:08X}",
                  (unsigned)(p & 0xFFFF),
                  (unsigned)(rv & 0xFF),
                  game->cdc.index,
                  (p & 3) == 2   ? "DATA-FIFO"
                  : (p & 3) == 1 ? "resp"
                  : (p & 3) == 0 ? "status"
                                 : "irq",
                  pc,
                  r[31]);
    return rv;
  }
  // GPUREAD. The register-polled drain of a GP0(0xC0) VRAM->CPU readback (the DMA2 drain is the other
  // one; both go through the same cursor in gpu_native.cpp). This returned a hard 0 before 2026-08-05,
  // which made every VRAM save/restore round-trip in every consuming game restore whatever its
  // destination buffer already held — silently. See spider1 issue 0007.
  if (p == 0x1F801810) {
    return gpu_read_word(this);
  }
  if (p == 0x1F801820 || p == 0x1F801824) { // MDEC0 data / MDEC1 status
    // Guest-visible poll = pump point: a DecDCTinSync-style status spin (or a data-port tail read)
    // must drive the pending MDEC DMA machinery forward, or it observes a frozen model forever.
    if (s_mdec0_left > 0 || s_mdec1_left > 0) {
      mdec_dma_pump();
    }
    return mdec_read(p);
  }
  if (p == 0x1F801DAE) {
    return 0; // SPUSTAT: report idle/transfer-complete
  }
  if (p >= 0x1F801C00 && p <= 0x1F801FFF) {
    return spu_read(p); // SPU register file
  }
  if (p == 0x1F8010B0) {
    return s_dma3_madr; // DMA3 CDROM->RAM
  }
  if (p == 0x1F8010B4) {
    return s_dma3_bcr;
  }
  if (p == 0x1F8010B8) {
    return s_dma3_chcr; // busy bit already cleared by the write handler
  }
  if (p == 0x1F8010C0) {
    return s_dma4_madr;
  }
  if (p == 0x1F8010C4) {
    return s_dma4_bcr;
  }
  if (p == 0x1F8010C8) {
    return s_dma4_chcr;
  }
  if (p == 0x1F801080) {
    return s_dma0_madr;
  }
  if (p == 0x1F801084) {
    return s_dma0_bcr;
  }
  if (p == 0x1F801088) { // DMA0 CHCR: bit 24 stays SET while input is pending
    if (s_mdec0_left > 0 || s_mdec1_left > 0) {
      mdec_dma_pump(); // a busy-poll drives progress
    }
    return s_dma0_chcr;
  }
  if (p == 0x1F801090) {
    return s_dma1_madr;
  }
  if (p == 0x1F801094) {
    return s_dma1_bcr;
  }
  if (p == 0x1F801098) { // DMA1 CHCR: bit 24 stays SET while output is pending
    if (s_mdec0_left > 0 || s_mdec1_left > 0) {
      mdec_dma_pump();
    }
    return s_dma1_chcr;
  }
  if (p == 0x1F8010A0) {
    return s_dma2_madr; // DMA2 MADR
  }
  if (p == 0x1F8010A4) {
    return s_dma2_bcr; // DMA2 BCR
  }
  if (p == 0x1F8010A8) {
    return s_dma2_chcr; // DMA2 CHCR (busy bit already cleared)
  }
  if (p >= 0x1F8010F0 && p <= 0x1F8010F3) { // DPCR — plain storage, byte-addressable
    return s_dpcr >> ((p & 3u) * 8u);
  }
  if (p >= 0x1F8010F4 && p <= 0x1F8010F7) { // DICR — bit 31 is computed, not stored
    return dma_dicr_read(s_dicr) >> ((p & 3u) * 8u);
  }
  if (p == 0x1F8010E0) {
    return s_dma6_madr; // DMA6 OTC MADR
  }
  if (p == 0x1F8010E4) {
    return s_dma6_bcr; // DMA6 OTC BCR
  }
  if (p == 0x1F8010E8) {
    return s_dma6_chcr; // DMA6 OTC CHCR (busy bit already cleared)
  }
  warn_unmapped_ram(this, a, bytes, "read");
  // `PSXPORT_DEBUG=io` — an unmapped PERIPHERAL access (a RAM address never gets here; the call above
  // fail-fasts on those). Reads back 0, which is what an unimplemented register looks like to a guest.
  lucent::debug("io", "read{} @ 0x{:08X} -> 0", bytes * 8, a);
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
    pending_work |= PW_IRQ; // arm the per-function-entry delivery gate
    lucent::debug("irq",
                  "CD raised IRQ2 -> I_STAT=0x{:03X} (mask=0x{:03X}, {})",
                  game->hle.i_stat,
                  game->hle.i_mask,
                  (game->hle.i_mask & 4) ? "ENABLED" : "masked off by the guest");
  }
  return game->hle.i_stat;
}

void Core::io_write(uint32_t a, uint32_t v, uint32_t bytes) {
  const uint32_t p = a & 0x1FFFFFFF;
  if (p == 0x1F801070) {                      // I_STAT: acknowledge. A bit written as 0 is
    irqStatLatch();                           // cleared; a bit written as 1 is left alone. This
    const uint32_t before = game->hle.i_stat; // is the PSX's semantic, NOT write-1-to-clear.
    game->hle.i_stat &= v & 0x7FFu;
    lucent::debug(
        "irq", "w I_STAT 0x{:03X}: 0x{:03X} -> 0x{:03X} ra={:08X}", v & 0x7FFu, before, game->hle.i_stat, r[31]);
    return;
  }
  if (p == 0x1F801074) { // I_MASK
    game->hle.i_mask = v & 0x7FFu;
    pending_work |= PW_IRQ; // unmasking may have made a latched bit deliverable
    lucent::debug("irq", "w I_MASK 0x{:03X} ra={:08X}", game->hle.i_mask, r[31]);
    return;
  }
  if (p >= 0x1F801800 && p <= 0x1F801803) { // CD controller
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
    lucent::debug("cdcw",
                  "w[{:04X}]={:02X} bank={} a0={:08X} s1={:08X} pc={:08X} ra={:08X}",
                  (unsigned)(p & 0xFFFF),
                  (unsigned)(v & 0xFF),
                  game->cdc.index,
                  r[4],
                  r[17],
                  pc,
                  r[31]);
    // The backtrace is an ADDENDUM to the cdcw line, so it needs cdcw on as well as cdcbt — the
    // cheap tests first, then the two channel lookups, then the (non-logging) unwind.
    if ((p & 3) == 1 && game->cdc.index == 0 && // command-register write only
        lucent::channel_on("cdcbt") && lucent::channel_on("cdcw")) {
      void *bt[24];
      int n = backtrace(bt, 24);
      backtrace_symbols_fd(bt, n, 2);
    }
    cdc_write(&game->cdc, p, (uint8_t)v);
    irqStatLatch(); // a command usually completes here and raises IRQ2 — latch it now, not on
                    // whatever unrelated access happens to read I_STAT next.
    return;
  }
  if (p == 0x1F801810) {
    gpu_gp0(this, v);
    return;
  } // GP0 (direct)
  if (p == 0x1F801814) {
    gpu_gp1(this, v);
    return;
  } // GP1 (display/control)
  if (p == 0x1F801820 || p == 0x1F801824) { // MDEC0 cmd / MDEC1 ctrl
    mdec_write(p, v);
    // A command or control write can be exactly what un-blocks a pending transfer (decode command
    // arriving after a DMA0 start; DMA enable bits set late), so it is a pump point too.
    if (s_mdec0_left > 0 || s_mdec1_left > 0) {
      mdec_dma_pump();
    }
    return;
  }
  if (p == 0x1F801DA6) {
    s_spu_xfer_addr = (v & 0xFFFF) << 3; // SPU transfer-start addr (bytes)
  }
  if (p >= 0x1F801C00 && p <= 0x1F801FFF) {
    spu_write(p, v);
    return;
  } // SPU register file
  // DMA3 — CDROM sector data -> RAM. This channel was simply ABSENT: an unmapped CHCR read returns
  // 0, which reads as "busy clear", so a guest that started a transfer and polled for completion was
  // told the transfer finished while not one byte had moved. Stock Sony libcd fetches every sector
  // this way (CD_getsector drives 0x1F8010B0/B4/B8 through its own pointer globals), so no
  // libcd-based game could read from disc at all.
  if (p == 0x1F8010B0) {
    s_dma3_madr = v;
    return;
  }
  if (p == 0x1F8010B4) {
    s_dma3_bcr = v;
    return;
  }
  if (p == 0x1F8010B8) {
    s_dma3_chcr = v;
    if (v & 0x01000000u) { // start/busy
      int n = dma_block_words(s_dma3_bcr);
      if (n > 0x10000) {
        n = 0x10000;
      }
      const uint32_t da = s_dma3_madr & 0x1FFFFC;
      const int fifo_words = cdc_dma_read(&game->cdc, s_dma_buf, n);
      for (int i = 0; i < n; i++) {
        mem_w32(da + i * 4, s_dma_buf[i]);
      }
      // CDC DMA returns zero after the FIFO empties (Beetle PS_CDC_DMARead) and still completes.
      // Keep both populations visible so a normal libstr tail flush cannot masquerade as disc data
      // and an unexpected depletion cannot disappear silently.
      lucent::debug("cdc",
                    "DMA3 {} words -> 0x{:08X}: FIFO {} + controller-zero {} (head LBA {})",
                    n,
                    0x80000000u | da,
                    fifo_words,
                    n - fifo_words,
                    game->cdc.loc_lba);
      s_dma3_chcr &= ~0x01000000u; // clear busy: the completion poll must pass
      irqStatLatch();              // draining a sector queues the next INT1
      // Announce completion — but ONLY if the guest asked to hear about THIS transfer. DICR is where
      // it says so, per channel, and a guest may deliberately run most of its transfers silent:
      // Sony's libstr streams an STR frame as N sector DMAs on this channel and enables the
      // interrupt for the LAST chunk only, so the frame-completion callback owes the guest one call
      // per FRAME. Signalling every transfer ran it once per SECTOR, which marked ring slots ready
      // before the frame was assembled (spider1 RE-07: the intro movies deadlocked on it).
      //
      // Deferred to a function-entry boundary rather than dispatched here: we are inside a guest
      // store, with native code mid-mutation.
      if (dma_completed(3)) {
        pending_work |= PW_IRQ;
      }
    }
    return;
  }
  if (p == 0x1F8010C0) {
    s_dma4_madr = v;
    return;
  }
  if (p == 0x1F8010C4) {
    s_dma4_bcr = v;
    return;
  }
  if (p == 0x1F8010C8) { // DMA4 CHCR: SPU-RAM transfer
    s_dma4_chcr = v;
    if (v & 0x01000000u) {
      int n = dma_block_words(s_dma4_bcr);
      if (n > 0x10000) {
        n = 0x10000;
      }
      uint32_t da = s_dma4_madr & 0x1FFFFC;
      if (v & 1) { // RAM -> SPU
        for (int i = 0; i < n; i++) {
          s_dma_buf[i] = mem_r32(da + i * 4);
        }
        if (cfg_str("PSXPORT_SPUDMA")) { // log VAB/sample transfers: source -> SPU dest, size
          lucent::info("spudma",
                       "RAM 0x{:08X} -> SPU 0x{:06X}  {} words ({} B)  pc={:08X} stage={:08X}",
                       0x80000000u | da,
                       s_spu_xfer_addr,
                       n,
                       n * 4,
                       pc,
                       mem_r32(0x801fe00c));
          if (n > 20000) { // big VAB bank: dump engine-range guest return addrs
            uint32_t sp = r[29] & 0x1FFFFFFF;
            lucent::Line ln;
            ln.add("  [vab-caller-chain]");
            for (uint32_t o = 0; o < 0x800 && sp + o + 4 <= 0x200000; o += 4) {
              uint32_t w;
              memcpy(&w, &ram[sp + o], 4);
              uint32_t p = w & 0x1FFFFFFF;
              if ((w & 0xFFE00000u) == 0x80000000u && (w & 3) == 0 && p >= 0x1E000 && p < 0x82000) {
                ln.add(" {:08X}", w);
              }
            }
            ln.flush(lucent::Level::Info, "spudma");
          }
        }
        spu_dma_write(s_dma_buf, n);
      } else { // SPU -> RAM
        int got = spu_dma_read(s_dma_buf, n);
        for (int i = 0; i < got; i++) {
          mem_w32(da + i * 4, s_dma_buf[i]);
        }
      }
      s_dma4_chcr &= ~0x01000000u;
      if (dma_completed(4)) {
        pending_work |= PW_IRQ;
      }
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
  if (p == 0x1F801080) {
    s_dma0_madr = v;
    return;
  }
  if (p == 0x1F801084) {
    s_dma0_bcr = v;
    return;
  }
  if (p == 0x1F801088) { // DMA0 CHCR: MDEC-in (RAM -> MDEC)
    s_dma0_chcr = v;
    if (v & 0x01000000u) {
      // WHICH MDEC CHANNEL STARTS FIRST decides what a synchronous model must defer. The guest
      // computes the DMA register base dynamically, so a static scan of the executable cannot
      // answer it (it finds no DMA0/DMA1 CHCR write at all) — but this handler sees every one.
      // Measured (PSXPORT_DEBUG=mdecdma, 2026-07-30 boot): DMA0-first, every decode.
      lucent::debug("mdecdma",
                    "DMA0(in)  start madr={:08X} bcr={:08X} words={}",
                    s_dma0_madr,
                    s_dma0_bcr,
                    dma_block_words(s_dma0_bcr));
      int n = dma_block_words(s_dma0_bcr);
      if (n > 0x10000) {
        n = 0x10000;
      }
      s_mdec0_addr = s_dma0_madr & 0xFFFFFCu;
      s_mdec0_left = n;
      s_mdec_stall_reported = 0;
      s_mdec_defer_note = 0;
      // Busy (bit 24) is NOT cleared here: mdec_dma_pump clears it when the last word is fed. A
      // decode whose output backs up leaves this channel PENDING — hardware-visible busy — and the
      // DMA1 start (or any guest poll of CHCR / the MDEC ports) resumes it.
      mdec_dma_pump();
    } else if (s_mdec0_left > 0) {
      // Forced stop (vendor dma.c DMA_Write kludge: clearing bit 24 aborts the channel).
      lucent::debug("mdecdma", "DMA0(in)  forced stop: {} word(s) abandoned", s_mdec0_left);
      s_mdec0_left = 0;
    }
    return;
  }
  if (p == 0x1F801090) {
    s_dma1_madr = v;
    return;
  }
  if (p == 0x1F801094) {
    s_dma1_bcr = v;
    return;
  }
  if (p == 0x1F801098) { // DMA1 CHCR: MDEC-out (MDEC -> RAM)
    s_dma1_chcr = v;
    if (v & 0x01000000u) {
      // Paired with the DMA0 trace above. This start usually finds DMA0's remainder pending
      // (measured live order); the pump then ping-pongs both channels to completion. A DMA1 start
      // that cannot complete no longer truncates silently — it stays PENDING with busy set (traced
      // as a deferral on this channel), exactly as hardware would show it.
      lucent::debug("mdecdma",
                    "DMA1(out) start madr={:08X} bcr={:08X} words={} canread={}",
                    s_dma1_madr,
                    s_dma1_bcr,
                    dma_block_words(s_dma1_bcr),
                    (int)mdec_dma_can_read());
      int n = dma_block_words(s_dma1_bcr);
      if (n > 0x10000) {
        n = 0x10000;
      }
      s_mdec1_addr = s_dma1_madr & 0xFFFFFFu; // dma.c CurAddr form: advances 4/word, 0x1FFFFC at store
      s_mdec1_left = n;
      s_mdec_stall_reported = 0;
      s_mdec_defer_note = 0;
      mdec_dma_pump();
    } else if (s_mdec1_left > 0) {
      lucent::debug("mdecdma", "DMA1(out) forced stop: {} word(s) abandoned", s_mdec1_left);
      s_mdec1_left = 0;
    }
    return;
  }
  if (p == 0x1F8010A0) {
    s_dma2_madr = v;
    return;
  }
  if (p == 0x1F8010A4) {
    s_dma2_bcr = v;
    return;
  }
  if (p == 0x1F8010A8) { // DMA2 CHCR: start triggers the transfer
    s_dma2_chcr = v;
    if (v & 0x01000000u) { // start/busy
      int sync = (v >> 9) & 3, to_gpu = v & 1;
      if (sync == 2) {
        gpu_dma2_linked_list(this, s_dma2_madr); // ordering-table linked list — full walk
      } else if (sync == 1) {
        gpu_dma2_block(this,
                       s_dma2_madr, // block: BC = blocks*size
                       (int)((s_dma2_bcr & 0xFFFF) * (s_dma2_bcr >> 16)),
                       to_gpu);
      } else {
        gpu_dma2_block(this, s_dma2_madr, (int)(s_dma2_bcr & 0xFFFF), to_gpu); // immediate
      }
      s_dma2_chcr &= ~0x01000000u; // clear busy -> game's DMA-done poll passes
      if (dma_completed(2)) {
        pending_work |= PW_IRQ;
      }
    }
    return;
  }
  if (p >= 0x1F8010F0 && p <= 0x1F8010F3) { // DPCR
    const uint32_t lane = dma_lane_mask(p, bytes);
    s_dpcr = (s_dpcr & ~lane) | (dma_lane_value(p, v, bytes) & lane);
    return;
  }
  if (p >= 0x1F8010F4 && p <= 0x1F8010F7) { // DICR
    s_dicr = dma_dicr_store(s_dicr, p, v, bytes);
    unsigned armed = 0;
    for (int ch = 0; ch < 7; ch++) {
      if (dma_irq_armed(s_dicr, ch)) {
        armed |= 1u << ch;
      }
    }
    lucent::debug("dmairq",
                  "w DICR{}[{}] = {:08X} -> {:08X} (armed channel mask {:02X}) ra={:08X}",
                  (p & 3u),
                  bytes,
                  v,
                  dma_dicr_read(s_dicr),
                  armed,
                  r[31]);
    return;
  }
  if (p == 0x1F8010E0) {
    s_dma6_madr = v;
    return;
  }
  if (p == 0x1F8010E4) {
    s_dma6_bcr = v;
    return;
  }
  if (p == 0x1F8010E8) { // DMA6 CHCR: OTC (ordering-table clear)
    s_dma6_chcr = v;
    if (v & 0x01000000u) { // start/busy
      uint32_t n = s_dma6_bcr & 0xFFFF;
      if (n == 0) {
        n = 0x10000;
      }
      uint32_t madr = s_dma6_madr & 0x1FFFFC; // highest entry address (&ot[n-1])
      for (uint32_t i = 0; i < n; i++) {
        uint32_t addr = madr - i * 4;
        uint32_t word = (i == n - 1) ? 0x00FFFFFFu : ((addr - 4) & 0x00FFFFFFu);
        mem_w32(addr, word);
      }
      s_dma6_chcr &= ~0x01000000u; // clear busy -> ClearOTagR's busy-poll passes
      if (dma_completed(6)) {
        pending_work |= PW_IRQ;
      }
    }
    return;
  }
  warn_unmapped_ram(this, a, bytes, "write");
  lucent::debug("io", "write{} @ 0x{:08X} = 0x{:08X}", bytes * 8, a, v); // discarded: no such register
}

uint8_t Core::mem_r8(uint32_t a) {
  uint8_t *p = host_ptr(a, 1);
  return p ? *p : (uint8_t)io_read(a, 1);
}
uint16_t Core::mem_r16(uint32_t a) {
  uint8_t *p = host_ptr(a, 2);
  if (!p) {
    return (uint16_t)io_read(a, 2);
  }
  uint16_t v;
  memcpy(&v, p, 2);
  return v;
}
uint32_t Core::mem_r32(uint32_t a) {
  uint8_t *p = host_ptr(a, 4);
  if (!p) {
    return io_read(a, 4);
  }
  uint32_t v;
  memcpy(&v, p, 4);
  return v;
}
// OT/GTE submission attribution (`debug otattr`, game/render/ot_attr.h): attribute a packet-pool store
// to the current otattr-shadow-stack fn + render-walk node. No-op unless the channel is on.
static inline void pkt_track(Core *c, uint32_t a, uint32_t bytes) {
  c->rsub.otAttr.trackStore(c, a, bytes);
}

// Mirror-verify write journal (game/core/verify_harness.h): while a strictCheck invocation is
// in-flight (PSXPORT_MIRROR_VERIFY), every MAIN-RAM store's pre-write byte gets journaled (once per
// byte per invocation — dirty-bitmap gated) so strictCheck can snapshot/rewind/compare only the
// touched bytes instead of the whole 2 MB buffer. Scratchpad stores are NOT journaled — strictCheck
// always full-compares the 1 KB scratchpad (already cheap). `!armed` is the hot-path no-op: a single
// bool read gates everything else, so this is free when MIRROR_VERIFY is unset (matches the existing
// display_pass_write_guard/pkt_track pattern on this same path).
static inline void journal_track(Core *c, uint8_t *p, uint32_t bytes) {
  if (!c->game || !c->game->verify.journalIsArmed()) {
    return;
  }
  if (p < c->ram || p + bytes > c->ram + 0x200000) {
    return; // not main RAM (scratchpad/none) — skip
  }
  uint32_t off = (uint32_t)(p - c->ram);
  for (uint32_t i = 0; i < bytes; i++) {
    c->game->verify.journalTrack(off + i, p[i]);
  }
}

// FAIL-FAST guard for the pc_render READ-ONLY-OVERLAY invariant (CLAUDE.md): pc_render's native
// display pass (DisplayPassGuard-armed scope in game_tomba2.cpp's Engine::drawOTag) reads guest RAM
// + engine state and draws to HOST memory only — it must NEVER write guest main RAM or scratchpad.
// Checked at the top of every guest store; the `!armed` branch is the hot-path no-op (single
// predicted-false bool read gates the address test, so this is cheap when the flag is off, which is
// always except during the pc_render display pass on this Core).
extern "C" void guest_backtrace_to(Core *, FILE *); // sync_overrides.cpp — guest-stack backtrace
static void display_pass_write_guard(Core *c, uint32_t a, uint32_t v, int width) {
  if (!c->rsub.mode.displayPassArmed()) {
    return;
  }
  const uint32_t p = a & 0x1FFFFFFF;
  const bool guest_ram = p < 0x200000;
  const bool scratchpad = p >= 0x1F800000 && p < 0x1F800400;
  if (!guest_ram && !scratchpad) {
    return;
  }
  lucent::info("mem",
               "\n[pc_render VIOLATION] guest write to 0x{:08X} (val 0x{:X}, width {}) during pc_render display pass — "
               "pc_render MUST be a read-only overlay (CLAUDE.md). interp_pc={:08X} sp={:08X}",
               0x80000000u | p,
               v,
               width,
               c->pc,
               c->r[29]);
  guest_backtrace_to(c, stderr);
  fflush(stderr);
  abort();
}

void Core::mem_w8(uint32_t a, uint8_t v) {
  uint8_t *p = host_ptr(a, 1);
  display_pass_write_guard(this, a, v, 1);
  wwatch_check(a, v, 1);
  cw_check(a, v, 1);
  pkt_track(this, a, 1);
  if (p) {
    journal_track(this, p, 1);
  }
  if (p) {
    *p = v;
  } else {
    io_write(a, v, 1);
  }
}
void Core::mem_w16(uint32_t a, uint16_t v) {
  uint8_t *p = host_ptr(a, 2);
  display_pass_write_guard(this, a, v, 2);
  wwatch_check(a, v, 2);
  cw_check(a, v, 2);
  pkt_track(this, a, 2);
  if (p) {
    journal_track(this, p, 2);
  }
  if (p) {
    memcpy(p, &v, 2);
  } else {
    io_write(a, v, 2);
  }
}
void Core::mem_w32(uint32_t a, uint32_t v) {
  uint8_t *p = host_ptr(a, 4);
  display_pass_write_guard(this, a, v, 4);
  wwatch_check(a, v, 4);
  cw_check(a, v, 4);
  pkt_track(this, a, 4);
  if (p) {
    journal_track(this, p, 4);
  }
  if (p) {
    memcpy(p, &v, 4);
  } else {
    io_write(a, v, 4);
  }
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
  if (dst == 0u) {
    return 0u;
  }
  if (n <= 0) {
    return 0u;
  }
  uint32_t cur = dst;
  for (int32_t i = 0; i < n; i++, cur++) {
    mem_w8(cur, val);
  }
  return dst;
}

// Override wrapper + install for the memset leaf (guest ABI: a0=dst, a1=val, a2=n, v0=return).
namespace {
void ov_guestMemset(Core *c) {
  const uint32_t dst = c->r[4];
  const int32_t n = (int32_t)c->r[6];
  c->r[2] = c->guestMemset(dst, (uint8_t)c->r[5], n);
  // Mirror gen's exit clobbers of the (caller-saved) a0/a2: the fill loop leaves r4 = dst+n and
  // r6 = 0. Callers may spill them before reassigning — a stale value there is a guest-stack
  // divergence (wave-3 lesson: live registers at call/return boundaries are part of the state).
  if (dst != 0u && n > 0) {
    c->r[4] = dst + (uint32_t)n;
    c->r[6] = 0u;
  }
}
} // namespace
void guest_memset_install() {
  static bool done = false;
  if (done) {
    return;
  }
  done = true;
  // The guest-memset gen body (generated gen_func_8009A420) is reached through the RecompRegistry seam
  // (recomp_iface.h), so the framework names no generated symbol.
  extern void engine_set_override_main(uint32_t, OverrideFn, OverrideFn);
  engine_set_override_main(0x8009A420u, ov_guestMemset, psxport_recomp()->guestMemset_gen);
}

// R3000 integer division semantics (no traps; defined results for /0 and overflow).
extern "C" void cpu_div(Core *c, uint32_t n, uint32_t d) {
  int32_t sn = (int32_t)n, sd = (int32_t)d;
  if (sd == 0) {
    c->lo = sn < 0 ? 1u : 0xFFFFFFFFu;
    c->hi = (uint32_t)sn;
  } else if (n == 0x80000000u && sd == -1) {
    c->lo = 0x80000000u;
    c->hi = 0;
  } else {
    c->lo = (uint32_t)(sn / sd);
    c->hi = (uint32_t)(sn % sd);
  }
}
extern "C" void cpu_divu(Core *c, uint32_t n, uint32_t d) {
  if (d == 0) {
    c->lo = 0xFFFFFFFFu;
    c->hi = n;
  } else {
    c->lo = n / d;
    c->hi = n % d;
  }
}

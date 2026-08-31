// SPU (Sound Processing Unit — PSX audio), lifted from the Beetle GPL-2 fork
// (mednafen/psx/spu.c, compiled as-is). All of the game's audio — 24 ADPCM voices,
// ADSR enveloping, pitch/sweeps, noise, reverb, and CD-DA mixing — flows through here;
// the SPU mixes 44.1 kHz stereo into the bound Beetle SpuState output buffer. This adapts a
// CLEAN recomp interface (spu_*) to Beetle's SPU_* API and provides definitions for the
// handful of externs spu.c references (the SPU IRQ line, the CD-DA source, savestate) so
// the mixed output matches the oracle exactly.
//
// Dependency surface (from `nm -u` on spu.o, minus libc memcpy/memset/__assert_fail):
//   spu_samples                — config: SPU update granularity in 44.1kHz samples
//   psx_spu_silent_voice_opt   — config: skip fully-silent voices (bit-identical output)
//   IRQ_Assert(int,bool)       — interrupt controller (SPU IRQ line)
//   CDC_GetCDAudioSample(s32*) — CD-DA audio source feeding the SPU mixer
//   MDFNSS_StateAction(...)    — savestate (unused)
// Each is shimmed faithful-first below; none pull in another .c file.
#include "cfg.h" // cfg_str (PSXPORT_SPUBT) — the CONFIG half of cfg.h
#include "host_backtrace.h"
#include <lucent/log.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// C LINKAGE FOR THE WHOLE FILE. This module is the C-ABI adapter between the vendored Beetle spu.c
// (compiled as C, and the caller of IRQ_Assert) and the C++ runtime, which reaches spu_init/spu_write/
// spu_render/... through its own `extern "C"` declarations (mem.cpp, spu_audio.cpp, spu_state.h). The
// file itself is C++ so it can use lucent; every symbol it defines or imports across that boundary
// still has to carry C linkage, so the whole body sits in one linkage specification.
extern "C" {

// ---------------------------------------------------------------------------
// Beetle SPU API (mednafen/psx/spu.h), declared locally to avoid pulling the
// Beetle headers (which would drag in state.h / mednafen-types.h). The struct
// StateMem is opaque here; SPU_StateAction is not exposed by the adapter.
// ---------------------------------------------------------------------------
struct StateMem;
void SPU_Init(void);
void SPU_Kill(void);
void SPU_Power(void);
void SPU_Write(int32_t timestamp, uint32_t A, uint16_t V);
uint16_t SPU_Read(int32_t timestamp, uint32_t A);
void SPU_WriteDMA(uint32_t V);
uint32_t SPU_ReadDMA(void);
int32_t SPU_UpdateFromCDC(int32_t clocks);

// Bound-state audio output ring. SPU_Render reads the buffer belonging to the state selected by
// SPU_BindState; keeping the drain inside the vendor unit prevents SBS cores from sharing a ring.
int SPU_Render(int16_t *out, int max_frames);

// ---------------------------------------------------------------------------
// Externs spu.c references — faithful-first definitions.
// ---------------------------------------------------------------------------

// Config: SPU update granularity, in 44.1 kHz samples advanced per inner step.
// Upstream default (libretro.c) is 1 = full per-sample accuracy. Faithful = 1.
// SANCTIONED VENDOR INTEROP (this + psx_spu_silent_voice_opt): the vendored Beetle spu.c reads
// these knobs by extern; both are compile-time-constant "faithful" values here.
uint8_t spu_samples = 1;

// Config: skip envelope/FIR/sweep work for voices that are fully silent (EnvLevel 0,
// in RELEASE, not voiced-on, not noise, IRQ off). spu.c documents the skipped state as
// idempotent and the audio output as bit-identical. Upstream default is true. Faithful = true.
bool psx_spu_silent_voice_opt = true;

// Interrupt controller — the SPU IRQ line. spu.c raises it when SPUCNT bit 6 is set and a voice's
// (or the transfer port's) SPU-RAM address matches IRQAddr, which is how a guest streams sample data
// into SPU RAM behind a playing voice: it arms the address one buffer ahead and refills on the
// interrupt. Only spu.c links this symbol (nothing else lifted from Beetle calls IRQ_Assert), so the
// only line that can arrive here is IRQ_SPU = 9.
//
// The line is delivered to the Core bound by spu_bind_irq_core (SpuDevice::bind, per core frame-step
// — the same instance-free bind spu_bind_log uses), which latches the rising edge into that Game's
// I_STAT bit 9 and lets the existing Hle::irqPoll chain run the guest's handler. NULL before the
// first bind and in the psxport_smoke link, where there is no Core to raise on.
void spu_irq_raise(void *core, int asserted); // hw_bind.cpp (C++ side owns Game/Hle)

static void *s_irq_core = 0;
void spu_bind_irq_core(void *core) {
  s_irq_core = core;
}

void IRQ_Assert(int which, bool asserted) {
  (void)which; // IRQ_SPU (9) is the only source that reaches this adapter — see above
  if (s_irq_core) {
    spu_irq_raise(s_irq_core, asserted ? 1 : 0);
  }
}

// CD-DA / CD-XA audio source. spu.c calls this once per output sample to fetch the stereo CD
// audio pair (then mixes it under CDVol/SPUControl). Contract (from PS_CDC::GetCDAudio):
// both channels are always written, clamped to -32768..32767. The real implementation lives
// in xa_stream.cpp (native in-game XA-ADPCM streaming) — that module owns this symbol so it can
// pull/resample the decoded XA ring. (It returns silence when nothing is streaming, which is
// the same faithful default the old stub here provided.)

// MDFNSS_StateAction (savestate, unused) is defined once in gte_beetle.cpp — shared across the
// lifted Beetle modules to avoid a multiple-definition link error.

// ---------------------------------------------------------------------------
// Clean recomp SPU interface -> Beetle SPU.
//
// SPU registers live at PSX bus 0x1F801C00..0x1F801FFF; Beetle addresses them by the
// low offset masked to 0x3FF (i.e. A relative to 0x1F801C00). We accept either a full
// PSX address or a bare offset and mask, matching how Beetle's bus dispatch hands SPU_*
// the masked address. SPU register access on the PS1 is 16-bit; we narrow val/result.
//
// SPU_Write/Read take a 'timestamp' that Beetle uses only to advance/align its event
// clock relative to the CPU; the SPU is actually mixed by SPU_UpdateFromCDC. The runtime
// drives mixing via spu_update(clocks), so register access passes timestamp 0 (the same
// convention Beetle uses for its non-timed accesses). The mix is unaffected.
// ---------------------------------------------------------------------------

void spu_init(void) {
  SPU_Init();
  SPU_Power();
}

// SBS per-Core SPU-write log. spu_bind_log (called from hw_bind::spu_bind) sets this to the
// current core's per-Game log buffer; spu_write appends (addr, val) pairs. SBS compares the
// two cores' logs at frame boundary to flag audio-relevant divergences (Issue #29). Set to
// NULL when SBS is not running; append is a cheap null-check.
typedef struct SpuWriteLog_ {
  uint32_t entries[8192]; // (addr, val) pairs; 4096 writes/frame budget
  uint32_t count;
} SpuWriteLog_;
// SANCTIONED BIND POINTER: spu_write is reached from instance-free paths (MMIO/DMA), so the active
// core's log is bound per frame-step (spu_bind_log <- SpuDevice::bind), exactly like SPU_BindState
// binds the SPU state itself. NULL when SBS is off.
static SpuWriteLog_ *s_spu_log = 0;
void spu_bind_log(void *log) {
  s_spu_log = (SpuWriteLog_ *)log;
}
void spu_log_reset(void *log) {
  if (log) {
    ((SpuWriteLog_ *)log)->count = 0;
  }
}
void *spu_new_log(void) {
  return calloc(1, sizeof(SpuWriteLog_));
}
uint32_t spu_log_count(void *log) {
  return log ? ((SpuWriteLog_ *)log)->count : 0;
}
uint32_t spu_log_entry(void *log, uint32_t i, int hi) {
  SpuWriteLog_ *l = (SpuWriteLog_ *)log;
  if (!l || i >= l->count) {
    return 0;
  }
  return l->entries[i * 2 + (hi ? 1 : 0)];
}

void spu_write(uint32_t addr, uint32_t val) {
  if (s_spu_log && s_spu_log->count * 2 + 1 < (uint32_t)(sizeof(s_spu_log->entries) / sizeof(s_spu_log->entries[0]))) {
    s_spu_log->entries[s_spu_log->count * 2 + 0] = addr;
    s_spu_log->entries[s_spu_log->count * 2 + 1] = val;
    s_spu_log->count++;
  }
  // `PSXPORT_DEBUG=spu`: surface whether the game drives the SPU at all (silent-output triage).
  // Logs total writes, key-on (KON 0x1F801D88/8A), and SPUCNT (0x1F801DAA, bit15 = SPU enable).
  // The gate guards the BLOCK (the per-category counters below are real work) and spu_write runs
  // per MMIO store, so it is an interned Channel rather than a name lookup per call.
  static const lucent::Channel spu_ch{"spu"};
  if (spu_ch) {
    // Process-wide `debug spu` print counters, consolidated in one static struct (this module is
    // instance-free at its MMIO entry; the counters are log numbering only, never machine state).
    static struct {
      long n, datacnt, voice_w, koff_w, vol_w, cdvol_w;
      uint32_t lastaddr;
    } sd;
    uint32_t off = addr & 0x3FF;
    long n = ++sd.n;
    // Per-category counters to separate "driver never runs its note path" from
    // "driver runs but keys nothing". Voice regs are 0x000..0x17F (24 voices x 0x10);
    // pitch is voice*0x10+0x04, ADSR is +0x06/+0x08, start-addr +0x06... we just count.
    if (off < 0x180) {
      sd.voice_w++; // any of the 24 voices
    } else if (off == 0x18C || off == 0x18E) {
      sd.koff_w++; // KOFF (key-off)
    } else if (off == 0x1B0 || off == 0x1B2) {
      sd.cdvol_w++; // CD input volume L/R
    } else if (off == 0x180 || off == 0x182) {
      sd.vol_w++; // main volume L/R
    }
    // Per-voice pitch/start-addr writes — with the KON lines these give the full "which sample,
    // what pitch, when" stream for A/B sequence diffs (docs/config.md `spu` channel).
    if (off < 0x180) {
      uint32_t vreg = off & 0xF, vno = off >> 4;
      if (vreg == 0x4) {
        lucent::debug(spu_ch, "[spudbg] V{:02} pitch={:04X}", vno, val & 0xFFFF);
      } else if (vreg == 0x6) {
        lucent::debug(spu_ch, "[spudbg] V{:02} start={:04X}", vno, val & 0xFFFF);
      }
    }
    if (off == 0x188 || off == 0x18A) {
      lucent::debug(spu_ch, "[spudbg] KON off={:03X} val={:04X} (writes={})", off, val & 0xFFFF, n);
    } else if (off == 0x18C || off == 0x18E) {
      lucent::debug(spu_ch, "[spudbg] KOFF off={:03X} val={:04X} (writes={})", off, val & 0xFFFF, n);
    } else if (off == 0x1AA) {
      lucent::debug(spu_ch,
                    "[spudbg] SPUCNT={:04X} enable={} cdaudio={} xfermode={} (writes={})",
                    val & 0xFFFF,
                    (val >> 15) & 1,
                    val & 1,
                    (val >> 4) & 3,
                    n);
    } else if (off == 0x1B0 || off == 0x1B2) {
      lucent::debug(spu_ch, "[spudbg] CDVOL off={:03X} val={:04X} (writes={})", off, val & 0xFFFF, n);
    } else if (off == 0x1A6) {
      sd.lastaddr = (val & 0xFFFF) << 3;
      lucent::debug(spu_ch, "[spudbg] SPU xfer ADDR=0x{:05X}", sd.lastaddr);
    } else if (off == 0x1A8) {
      sd.datacnt++;
      if ((sd.datacnt % 1000) == 1) {
        lucent::debug(spu_ch, "[spudbg] SPU DATA-port write #{} (val={:04X})", sd.datacnt, val & 0xFFFF);
      }
    }
    if ((n % 20000) == 0) {
      lucent::debug(spu_ch,
                    "[spudbg] SUMMARY writes={} voice={} koff={} mainvol={} cdvol={}",
                    n,
                    sd.voice_w,
                    sd.koff_w,
                    sd.vol_w,
                    sd.cdvol_w);
    }
  }
  // PSXPORT_SPUBT=<hex reg offset> — host backtrace on every write to that SPU register (e.g.
  // 156 = voice 21 start-addr). Guest-side attribution for SPU writes: wwatch can't see MMIO and
  // dispwatch misses static gen-to-gen calls; the host stack names the gen_func_*/native chain.
  {
    const char *bt = cfg_str("PSXPORT_SPUBT");
    if (bt && (addr & 0x3FF) == (uint32_t)strtoul(bt, 0, 16)) {
      void *frames[32];
      int n = backtrace(frames, 32);
      lucent::info("spubt", "off={:03X} val={:04X}", addr & 0x3FF, val & 0xFFFF);
      backtrace_symbols_fd(frames, n, 2);
      lucent::info("spubt", "----");
    }
  }
  SPU_Write(0, addr & 0x3FF, (uint16_t)val);
}

uint32_t spu_read(uint32_t addr) {
  return SPU_Read(0, addr & 0x3FF);
}

// DMA (channel 4) <-> SPU RAM. Beetle's SPU_WriteDMA/ReadDMA move one 32-bit word
// (two 16-bit SPU-RAM samples) per call through the SPU's DMA FIFO; we loop over the
// caller's word block.
void spu_dma_write(const uint32_t *words, int count) {
  static const lucent::Channel spu_ch{"spu"};
  if (spu_ch) { // guards the counters, not the print
    static struct {
      long calls, total;
    } sd; // process-wide `debug spu` print counters (see spu_write)
    sd.calls++;
    sd.total += count;
    lucent::debug(spu_ch, "[spudbg] SPU-RAM DMA write: {} words (call {}, total {} words)", count, sd.calls, sd.total);
  }
  int i;
  for (i = 0; i < count; i++) {
    SPU_WriteDMA(words[i]);
  }
}

int spu_dma_read(uint32_t *words, int count) {
  int i;
  for (i = 0; i < count; i++) {
    words[i] = SPU_ReadDMA();
  }
  return count;
}

// Advance the SPU by 'clocks' CPU(/CDC-domain) cycles, mixing 44.1 kHz stereo samples
// into IntermediateBuffer. Returns Beetle's value (clocks until the next SPU event).
int32_t spu_update(int32_t clocks) {
  return SPU_UpdateFromCDC(clocks);
}

// Drain mixed stereo samples accumulated since the last pull. Copies up to max_frames
// interleaved L/R int16 frames out of IntermediateBuffer and resets the write cursor,
// returning the number of frames produced. Sample rate is 44100 Hz, stereo, signed 16.
// (Buffer holds at most 4096 frames; mix often enough that it doesn't overrun.)
int spu_render(int16_t *out, int max_frames) {
  const int n = SPU_Render(out, max_frames);

  static const lucent::Channel spu_ch{"spu"};
  if (spu_ch) { // guards the peak SCAN over n*2 samples, not the print
    static long fr;
    int peak = 0; // process-wide `debug spu` print counter (see spu_write)
    for (int i = 0; i < n * 2; i++) {
      int v = out[i];
      if (v < 0) {
        v = -v;
      }
      if (v > peak) {
        peak = v;
      }
    }
    if ((++fr % 60) == 0 || peak > 0) {
      lucent::debug(spu_ch, "[spudbg] spu_render frame {}: {} stereo samples, peak={}", fr, n, peak);
    }
  }

  return n;
}

} // extern "C"

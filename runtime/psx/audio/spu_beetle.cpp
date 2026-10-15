// SPU (Sound Processing Unit — PSX audio), lifted from the Beetle GPL-2 fork
// (mednafen/psx/spu.c, compiled as-is). All of the game's audio — 24 ADPCM voices,
// ADSR enveloping, pitch/sweeps, noise, reverb, and CD-DA mixing — flows through here;
// the SPU mixes 44.1 kHz stereo into the bound Beetle SpuState output buffer. This adapts a
// clean guest interface (psx::audio::spu) to Beetle's SPU_* API and provides definitions for the
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
#include "spu_beetle.h"
#include "diagnostic_config.h" // PSXPORT_SPUBT
#include "host_backtrace.h"
#include "spu_state.h"
#include <lucent/log.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

namespace {

// `debug spu` print counters; log numbering only, never machine state.
struct SpuDebugCounters {
  long writes = 0;
  long dataPortWrites = 0;
  long voiceWrites = 0;
  long keyOffWrites = 0;
  long mainVolumeWrites = 0;
  long cdVolumeWrites = 0;
  uint32_t transferAddress = 0;
  long dmaCalls = 0;
  long dmaWords = 0;
  long renderedFields = 0;
};

constexpr lucent::Channel spuChannel{"spu"};
SpuDebugCounters spuDebug;
// The Core whose I_STAT the SPU interrupt line latches into; spu.c reaches it with no instance.
Core *irqCore = nullptr;

} // namespace

// Beetle's link-time hooks: spu.c reads these knobs and calls IRQ_Assert with C linkage.
extern "C" {
#include "mednafen/psx/irq.h" // the interrupt line numbers and IRQ_Assert's signature

// SPU update granularity in 44.1 kHz samples per inner step; 1 is upstream's full accuracy.
uint8_t spu_samples = 1;

// Skip fully silent voices; spu.c documents the output as bit-identical.
bool psx_spu_silent_voice_opt = true;

// spu.c raises IRQ_SPU on an SPU-RAM address match; gpu.c's VBlank and GP0(1Fh) lines are owned elsewhere.
void IRQ_Assert(int which, bool asserted) {
  if (which != IRQ_SPU || irqCore == nullptr) {
    return;
  }
  psx::audio::spu::raiseIrq(*irqCore, asserted);
}

} // extern "C"

// CD-DA / CD-XA audio source. spu.c calls this once per output sample to fetch the stereo CD
// audio pair (then mixes it under CDVol/SPUControl). Contract (from PS_CDC::GetCDAudio):
// both channels are always written, clamped to -32768..32767. The real implementation lives
// in xa_stream.cpp (native in-game XA-ADPCM streaming) — that module owns this symbol so it can
// pull/resample the decoded XA ring. (It returns silence when nothing is streaming, which is
// the same faithful default the old stub here provided.)

// MDFNSS_StateAction (savestate, unused) is defined once in gte_beetle.cpp — shared across the
// lifted Beetle modules to avoid a multiple-definition link error.

// ---------------------------------------------------------------------------
// Clean guest SPU interface -> Beetle SPU.
//
// SPU registers live at PSX bus 0x1F801C00..0x1F801FFF; Beetle addresses them by the
// low offset masked to 0x3FF (i.e. A relative to 0x1F801C00). We accept either a full
// PSX address or a bare offset and mask, matching how Beetle's bus dispatch hands SPU_*
// the masked address. SPU register access on the PS1 is 16-bit; we narrow val/result.
//
// SPU_Write/Read take a 'timestamp' that Beetle uses only to advance/align its event
// clock relative to the CPU; the SPU is actually mixed by SPU_UpdateFromCDC. The runtime
// drives mixing via spu::update(clocks), so register access passes timestamp 0 (the same
// convention Beetle uses for its non-timed accesses). The mix is unaffected.
// ---------------------------------------------------------------------------

namespace psx::audio::spu {

void init() {
  SPU_Init();
  SPU_Power();
}

void bindIrqCore(Core *core) {
  irqCore = core;
}

void write(uint32_t addr, uint32_t val) {
  // `PSXPORT_DEBUG=spu`: surface whether the game drives the SPU at all (silent-output triage).
  // Logs total writes, key-on (KON 0x1F801D88/8A), and SPUCNT (0x1F801DAA, bit15 = SPU enable).
  // The gate guards the BLOCK (the per-category counters below are real work) and spu_write runs
  // per MMIO store, so it is an interned Channel rather than a name lookup per call.
  if (spuChannel) {
    SpuDebugCounters &sd = spuDebug;
    uint32_t off = addr & 0x3FF;
    long n = ++sd.writes;
    // Per-category counters to separate "driver never runs its note path" from
    // "driver runs but keys nothing". Voice regs are 0x000..0x17F (24 voices x 0x10);
    // pitch is voice*0x10+0x04, ADSR is +0x06/+0x08, start-addr +0x06... we just count.
    if (off < 0x180) {
      sd.voiceWrites++; // any of the 24 voices
    } else if (off == 0x18C || off == 0x18E) {
      sd.keyOffWrites++; // KOFF (key-off)
    } else if (off == 0x1B0 || off == 0x1B2) {
      sd.cdVolumeWrites++; // CD input volume L/R
    } else if (off == 0x180 || off == 0x182) {
      sd.mainVolumeWrites++; // main volume L/R
    }
    // Per-voice pitch/start-addr writes — with the KON lines these give the full "which sample,
    // what pitch, when" stream for A/B sequence diffs (docs/config.md `spu` channel).
    if (off < 0x180) {
      uint32_t vreg = off & 0xF, vno = off >> 4;
      if (vreg == 0x4) {
        lucent::debug(spuChannel, "[spudbg] V{:02} pitch={:04X}", vno, val & 0xFFFF);
      } else if (vreg == 0x6) {
        lucent::debug(spuChannel, "[spudbg] V{:02} start={:04X}", vno, val & 0xFFFF);
      }
    }
    if (off == 0x188 || off == 0x18A) {
      lucent::debug(spuChannel, "[spudbg] KON off={:03X} val={:04X} (writes={})", off, val & 0xFFFF, n);
    } else if (off == 0x18C || off == 0x18E) {
      lucent::debug(spuChannel, "[spudbg] KOFF off={:03X} val={:04X} (writes={})", off, val & 0xFFFF, n);
    } else if (off == 0x1AA) {
      lucent::debug(spuChannel,
                    "[spudbg] SPUCNT={:04X} enable={} cdaudio={} xfermode={} (writes={})",
                    val & 0xFFFF,
                    (val >> 15) & 1,
                    val & 1,
                    (val >> 4) & 3,
                    n);
    } else if (off == 0x1B0 || off == 0x1B2) {
      lucent::debug(spuChannel, "[spudbg] CDVOL off={:03X} val={:04X} (writes={})", off, val & 0xFFFF, n);
    } else if (off == 0x1A6) {
      sd.transferAddress = (val & 0xFFFF) << 3;
      lucent::debug(spuChannel, "[spudbg] SPU xfer ADDR=0x{:05X}", sd.transferAddress);
    } else if (off == 0x1A8) {
      sd.dataPortWrites++;
      if ((sd.dataPortWrites % 1000) == 1) {
        lucent::debug(spuChannel, "[spudbg] SPU DATA-port write #{} (val={:04X})", sd.dataPortWrites, val & 0xFFFF);
      }
    }
    if ((n % 20000) == 0) {
      lucent::debug(spuChannel,
                    "[spudbg] SUMMARY writes={} voice={} koff={} mainvol={} cdvol={}",
                    n,
                    sd.voiceWrites,
                    sd.keyOffWrites,
                    sd.mainVolumeWrites,
                    sd.cdVolumeWrites);
    }
  }
  // PSXPORT_SPUBT=<hex reg offset> — host backtrace on every write to that SPU register (e.g.
  // 156 = voice 21 start-addr). Guest-side attribution for SPU writes: wwatch can't see MMIO and
  // dispwatch misses static gen-to-gen calls; the host stack names the guest_*/native chain.
  {
    const std::string bt = psx::config::spu_write_backtrace();
    if (!bt.empty() && (addr & 0x3FF) == (uint32_t)strtoul(bt.c_str(), 0, 16)) {
      void *frames[32];
      int n = backtrace(frames, 32);
      lucent::info("spubt", "off={:03X} val={:04X}", addr & 0x3FF, val & 0xFFFF);
      psxport::host::emitBacktrace(
          lucent::Level::Info, "spubt", std::span<void *const>(frames, static_cast<std::size_t>(n)).subspan(1));
      lucent::info("spubt", "----");
    }
  }
  SPU_Write(0, addr & 0x3FF, (uint16_t)val);
}

uint32_t read(uint32_t addr) {
  return SPU_Read(0, addr & 0x3FF);
}

// DMA (channel 4) <-> SPU RAM. Beetle's SPU_WriteDMA/ReadDMA move one 32-bit word
// (two 16-bit SPU-RAM samples) per call through the SPU's DMA FIFO; we loop over the
// caller's word block.
void dmaWrite(const uint32_t *words, int count) {
  if (spuChannel) { // guards the counters, not the print
    spuDebug.dmaCalls++;
    spuDebug.dmaWords += count;
    lucent::debug(spuChannel,
                  "[spudbg] SPU-RAM DMA write: {} words (call {}, total {} words)",
                  count,
                  spuDebug.dmaCalls,
                  spuDebug.dmaWords);
  }
  int i;
  for (i = 0; i < count; i++) {
    SPU_WriteDMA(words[i]);
  }
}

int dmaRead(uint32_t *words, int count) {
  int i;
  for (i = 0; i < count; i++) {
    words[i] = SPU_ReadDMA();
  }
  return count;
}

// Advance the SPU by 'clocks' CPU(/CDC-domain) cycles, mixing 44.1 kHz stereo samples
// into IntermediateBuffer. Returns Beetle's value (clocks until the next SPU event).
int32_t update(int32_t clocks) {
  return SPU_UpdateFromCDC(clocks);
}

// Drain mixed stereo samples accumulated since the last pull. Copies up to max_frames
// interleaved L/R int16 frames out of IntermediateBuffer and resets the write cursor,
// returning the number of frames produced. Sample rate is 44100 Hz, stereo, signed 16.
// (Buffer holds at most 4096 frames; mix often enough that it doesn't overrun.)
int render(int16_t *out, int max_frames) {
  const int n = SPU_Render(out, max_frames);

  if (spuChannel) { // guards the peak SCAN over n*2 samples, not the print
    int peak = 0;
    for (int i = 0; i < n * 2; i++) {
      int v = out[i];
      if (v < 0) {
        v = -v;
      }
      if (v > peak) {
        peak = v;
      }
    }
    const long fr = ++spuDebug.renderedFields;
    if ((fr % 60) == 0 || peak > 0) {
      lucent::debug(spuChannel, "[spudbg] spu_render frame {}: {} stereo samples, peak={}", fr, n, peak);
    }
  }

  return n;
}

} // namespace psx::audio::spu

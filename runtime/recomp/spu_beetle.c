// SPU (Sound Processing Unit — PSX audio), lifted from the Beetle GPL-2 fork
// (mednafen/psx/spu.c, compiled as-is). All of the game's audio — 24 ADPCM voices,
// ADSR enveloping, pitch/sweeps, noise, reverb, and CD-DA mixing — flows through here;
// the SPU mixes 44.1 kHz stereo into Beetle's global IntermediateBuffer. This adapts a
// CLEAN recomp interface (spu_*) to Beetle's SPU_* API and provides faithful-first stubs
// for the handful of externs spu.c references (IRQ controller not yet wired, CD-DA source
// not yet wired, savestate unused) so the mixed output matches the oracle exactly.
//
// Dependency surface (from `nm -u` on spu.o, minus libc memcpy/memset/__assert_fail):
//   spu_samples                — config: SPU update granularity in 44.1kHz samples
//   psx_spu_silent_voice_opt   — config: skip fully-silent voices (bit-identical output)
//   IRQ_Assert(int,bool)       — interrupt controller (SPU IRQ line)
//   CDC_GetCDAudioSample(s32*) — CD-DA audio source feeding the SPU mixer
//   MDFNSS_StateAction(...)    — savestate (unused)
// Each is shimmed faithful-first below; none pull in another .c file.
#include <stdint.h>
#include "cfg.h"
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

// ---------------------------------------------------------------------------
// Beetle SPU API (mednafen/psx/spu.h), declared locally to avoid pulling the
// Beetle headers (which would drag in state.h / mednafen-types.h). The struct
// StateMem is opaque here; SPU_StateAction is not exposed by the adapter.
// ---------------------------------------------------------------------------
struct StateMem;
void     SPU_Init(void);
void     SPU_Kill(void);
void     SPU_Power(void);
void     SPU_Write(int32_t timestamp, uint32_t A, uint16_t V);
uint16_t SPU_Read(int32_t timestamp, uint32_t A);
void     SPU_WriteDMA(uint32_t V);
uint32_t SPU_ReadDMA(void);
int32_t  SPU_UpdateFromCDC(int32_t clocks);

// Beetle's global audio output ring: SPU mixes 44.1 kHz stereo samples into this
// buffer; the frontend drains it. IntermediateBufferPos is the write cursor (in
// stereo frames). Defined in spu.c — we only read/reset it here.
extern uint32_t IntermediateBufferPos;
extern int16_t  IntermediateBuffer[4096][2];

// ---------------------------------------------------------------------------
// Externs spu.c references — faithful-first definitions.
// ---------------------------------------------------------------------------

// Config: SPU update granularity, in 44.1 kHz samples advanced per inner step.
// Upstream default (libretro.c) is 1 = full per-sample accuracy. Faithful = 1.
uint8_t spu_samples = 1;

// Config: skip envelope/FIR/sweep work for voices that are fully silent (EnvLevel 0,
// in RELEASE, not voiced-on, not noise, IRQ off). spu.c documents the skipped state as
// idempotent and the audio output as bit-identical. Upstream default is true. Faithful = true.
bool psx_spu_silent_voice_opt = true;

// Interrupt controller — SPU IRQ line (IRQ_SPU = 9 in irq.h). The interpreter/runtime
// owns interrupt delivery; until the PM wires the SPU IRQ into it, this is a faithful
// no-op (the SPU computes the assert level correctly; only delivery is deferred).
// STOPGAP: route to the runtime's interrupt controller because the SPU IRQ (e.g. used
// for SPU-RAM-address-match streaming) must reach the CPU; deferred to PM wiring.
void IRQ_Assert(int which, bool asserted)
{
   (void)which;
   (void)asserted;
}

// CD-DA / CD-XA audio source. spu.c calls this once per output sample to fetch the stereo CD
// audio pair (then mixes it under CDVol/SPUControl). Contract (from PS_CDC::GetCDAudio):
// both channels are always written, clamped to -32768..32767. The real implementation lives
// in xa_stream.c (native in-game XA-ADPCM streaming) — that module owns this symbol so it can
// pull/resample the decoded XA ring. (It returns silence when nothing is streaming, which is
// the same faithful default the old stub here provided.)

// MDFNSS_StateAction (savestate, unused) is defined once in gte_beetle.c — shared across the
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

void spu_init(void)
{
   SPU_Init();
   SPU_Power();
}

void spu_write(uint32_t addr, uint32_t val)
{
   // PSXPORT_SPU_DBG: surface whether the game drives the SPU at all (silent-output triage).
   // Logs total writes, key-on (KON 0x1F801D88/8A), and SPUCNT (0x1F801DAA, bit15 = SPU enable).
   if (cfg_dbg("spu")) {
      static long n; uint32_t off = addr & 0x3FF;
      n++;
      static long datacnt; static uint32_t lastaddr;
      // Per-category counters to separate "driver never runs its note path" from
      // "driver runs but keys nothing". Voice regs are 0x000..0x17F (24 voices x 0x10);
      // pitch is voice*0x10+0x04, ADSR is +0x06/+0x08, start-addr +0x06... we just count.
      static long voice_w, koff_w, vol_w, cdvol_w;
      if      (off < 0x180)                voice_w++;          // any of the 24 voices
      else if (off == 0x18C || off == 0x18E) koff_w++;          // KOFF (key-off)
      else if (off == 0x1B0 || off == 0x1B2) cdvol_w++;         // CD input volume L/R
      else if (off == 0x180 || off == 0x182) vol_w++;           // main volume L/R
      if (off == 0x188 || off == 0x18A) fprintf(stderr, "[spudbg] KON off=%03X val=%04X (writes=%ld)\n", off, val & 0xFFFF, n);
      else if (off == 0x18C || off == 0x18E) fprintf(stderr, "[spudbg] KOFF off=%03X val=%04X (writes=%ld)\n", off, val & 0xFFFF, n);
      else if (off == 0x1AA) fprintf(stderr, "[spudbg] SPUCNT=%04X enable=%d cdaudio=%d xfermode=%d (writes=%ld)\n", val & 0xFFFF, (val >> 15) & 1, val & 1, (val >> 4) & 3, n);
      else if (off == 0x1B0 || off == 0x1B2) fprintf(stderr, "[spudbg] CDVOL off=%03X val=%04X (writes=%ld)\n", off, val & 0xFFFF, n);
      else if (off == 0x1A6) { lastaddr = (val & 0xFFFF) << 3; fprintf(stderr, "[spudbg] SPU xfer ADDR=0x%05X\n", lastaddr); }
      else if (off == 0x1A8) { datacnt++; if ((datacnt % 1000) == 1) fprintf(stderr, "[spudbg] SPU DATA-port write #%ld (val=%04X)\n", datacnt, val & 0xFFFF); }
      if ((n % 20000) == 0)
         fprintf(stderr, "[spudbg] SUMMARY writes=%ld voice=%ld koff=%ld mainvol=%ld cdvol=%ld\n",
                 n, voice_w, koff_w, vol_w, cdvol_w);
   }
   SPU_Write(0, addr & 0x3FF, (uint16_t)val);
}

uint32_t spu_read(uint32_t addr)
{
   return SPU_Read(0, addr & 0x3FF);
}

// DMA (channel 4) <-> SPU RAM. Beetle's SPU_WriteDMA/ReadDMA move one 32-bit word
// (two 16-bit SPU-RAM samples) per call through the SPU's DMA FIFO; we loop over the
// caller's word block.
void spu_dma_write(const uint32_t *words, int count)
{
   if (cfg_dbg("spu")) {
      static long calls, total; calls++; total += count;
      fprintf(stderr, "[spudbg] SPU-RAM DMA write: %d words (call %ld, total %ld words)\n", count, calls, total);
   }
   int i;
   for (i = 0; i < count; i++)
      SPU_WriteDMA(words[i]);
}

int spu_dma_read(uint32_t *words, int count)
{
   int i;
   for (i = 0; i < count; i++)
      words[i] = SPU_ReadDMA();
   return count;
}

// Advance the SPU by 'clocks' CPU(/CDC-domain) cycles, mixing 44.1 kHz stereo samples
// into IntermediateBuffer. Returns Beetle's value (clocks until the next SPU event).
int32_t spu_update(int32_t clocks)
{
   return SPU_UpdateFromCDC(clocks);
}

// Drain mixed stereo samples accumulated since the last pull. Copies up to max_frames
// interleaved L/R int16 frames out of IntermediateBuffer and resets the write cursor,
// returning the number of frames produced. Sample rate is 44100 Hz, stereo, signed 16.
// (Buffer holds at most 4096 frames; mix often enough that it doesn't overrun.)
int spu_render(int16_t *out, int max_frames)
{
   uint32_t avail = IntermediateBufferPos;
   uint32_t n = avail;
   if (max_frames >= 0 && n > (uint32_t)max_frames)
      n = (uint32_t)max_frames;

   memcpy(out, IntermediateBuffer, (size_t)n * 2 * sizeof(int16_t));

   if (cfg_dbg("spu")) {
      static long fr; int peak = 0;
      for (uint32_t i = 0; i < n * 2; i++) { int v = out[i]; if (v < 0) v = -v; if (v > peak) peak = v; }
      if ((++fr % 60) == 0 || peak > 0)
         fprintf(stderr, "[spudbg] spu_render frame %ld: %u stereo samples, peak=%d\n", fr, n, peak);
   }

   if (n < avail)
   {
      // Keep the unread tail for the next pull (shouldn't normally happen if the PM
      // sizes max_frames >= a frame's worth, ~735 NTSC / ~882 PAL samples).
      memmove(IntermediateBuffer, &IntermediateBuffer[n],
              (size_t)(avail - n) * 2 * sizeof(int16_t));
      IntermediateBufferPos = avail - n;
   }
   else
   {
      IntermediateBufferPos = 0;
   }
   return (int)n;
}

// MDEC (Motion Decoder), lifted from the Beetle GPL-2 fork (mednafen/psx/mdec.c, compiled
// as-is). MDEC is the PSX's macroblock/JPEG-like video decoder: it does the run-length
// dequant -> IDCT -> YCbCr->RGB pipeline that decompresses FMV (STR) frames; the game DMAs
// compressed coefficient streams in (DMA0 = MDEC-in) and DMAs decoded RGB/indexed pixels
// out (DMA1 = MDEC-out). Our previous handling was absent, so FMVs were dead. This adapts a
// clean recomp-facing interface to Beetle's MDEC_* API and provides faithful-first stubs for
// the two externs mdec.c references (event-cycle budget, savestate unused) so the decode math
// matches the oracle exactly. No enhancements: same IDCT, same YCbCr coefficients, same packing.
#include <stdint.h>
#include <stdbool.h>

// Beetle MDEC API (mednafen/psx/mdec.h), declared locally to avoid pulling Beetle headers.
// (mdec.c has no MDEC_Init; MDEC_Power is the reset/construct entry point.)
void     MDEC_Power(void);
void     MDEC_Write(const int32_t timestamp, uint32_t A, uint32_t V);
uint32_t MDEC_Read(const int32_t timestamp, uint32_t A);
void     MDEC_DMAWrite(uint32_t V);
uint32_t MDEC_DMARead(uint32_t *offs);
bool     MDEC_DMACanWrite(void);
bool     MDEC_DMACanRead(void);
void     MDEC_Run(int32_t clocks);

// Externs mdec.c references — faithful-first values.
// EventCycles is the global "next event horizon" the PSX scheduler uses to bound how far the
// MDEC clock counter may run ahead in MDEC_Run (ClockCounter is clamped to it). With no real
// scheduler in this module, a large positive ceiling lets the decode state machine drain
// freely each step without artificially stalling — the FIFO depth limits already gate flow.
int32_t EventCycles = 0x7FFFFFFF;
// MDFNSS_StateAction (savestate, unused) is defined once in gte_beetle.c — shared across the
// lifted Beetle modules to avoid a multiple-definition link error.

// Recomp MDEC interface -> Beetle MDEC. The PSX exposes MDEC as two 32-bit ports:
//   0x1F801820  MDEC0 — command/parameter write, decoded-data read
//   0x1F801824  MDEC1 — control/reset write, status read
// Beetle selects between them with (A & 4): bit 2 clear = data port, set = status/control.
// We pass the raw byte address straight through (only bit 2 matters). The timestamp argument
// is unused by mdec.c's reads/writes, so 0 is faithful.
void mdec_init(void) { MDEC_Power(); }

void mdec_write(uint32_t addr, uint32_t val) { MDEC_Write(0, addr, val); }
uint32_t mdec_read(uint32_t addr)            { return MDEC_Read(0, addr); }

// DMA0 (MDEC-in): feed `count` 32-bit words of the compressed stream into the input FIFO.
// MDEC_DMAWrite pushes one word and steps the decode state machine; it silently drops words
// when the input FIFO is full (faithful — the real DMA channel honors MDEC_DMACanWrite()).
void mdec_dma_in(const uint32_t* words, int count) {
  int i;
  for (i = 0; i < count; i++)
    MDEC_DMAWrite(words[i]);
}

// DMA1 (MDEC-out): pull up to `count` decoded 32-bit words out of the output FIFO. Returns the
// number of words actually produced (stops early if the FIFO drains). The per-word `offs` that
// MDEC_DMARead emits (the macroblock-interleave RAM write offset) is consumed/ignored here:
// it only matters when scattering pixels into VRAM/RAM in macroblock order, which the caller's
// DMA-channel logic reconstructs; this linear drain matches the FIFO read order.
int mdec_dma_out(uint32_t* words, int count) {
  int i;
  for (i = 0; i < count; i++) {
    if (!MDEC_DMACanRead())
      break;
    uint32_t offs;
    words[i] = MDEC_DMARead(&offs);
  }
  return i;
}

// Status helpers wrapping Beetle's DMA-readiness predicates, for a caller that gates its own
// DMA0/DMA1 transfers (>= 0x20 words available and the matching enable bit set in Control).
bool mdec_dma_can_write(void) { return MDEC_DMACanWrite(); }
bool mdec_dma_can_read(void)  { return MDEC_DMACanRead(); }

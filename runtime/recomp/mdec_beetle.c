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

// DMA1 (MDEC-out): drain decoded 32-bit words out of the output FIFO and PLACE each one at the
// hardware macroblock-interleave position within `buf`, so after the call `buf` holds exactly
// what real PSX DMA channel 1 would have written to that RAM region (correct macroblock->raster
// layout), not the FIFO-sequential order.
//
// PLACEMENT / OFFSET CONTRACT (must match Beetle's vendor/.../psx/dma.c, CH_MDEC_OUT exactly):
// For each transferred word, real DMA1 does:
//     vtmp  = MDEC_DMARead(&voffs);                 // voffs is a per-word WORD displacement
//     waddr = (CurAddr + (voffs << 2)) & 0x1FFFFC;  // voffs<<2 == voffs words in bytes
//     MainRAM[waddr] = vtmp;                         // 32-bit store
//     CurAddr += 4;                                  // base advances ONE WORD per transferred word
// So the destination WORD index of the i-th drained word (i counted from the channel's start) is
//     dest_word(i) = (MADR_word_base) + i + voffs_i
// i.e. a linearly-advancing base (i) PLUS the MDEC's per-word interleave displacement (voffs_i).
// `voffs` is NOT an absolute index — it is a relative word offset added on top of the i-th step;
// MDEC emits it (see MDEC_DMARead) as (RAMOffsetY & 7) * RAMOffsetWWS, and for the LOWER half of
// each 16-pixel-tall macroblock column (RAMOffsetY & 8) it SUBTRACTS RAMOffsetWWS*7 — i.e. the
// offset goes NEGATIVE. Because MDEC_DMARead's out-parameter is uint32_t, that negative value
// arrives as a large unsigned word (e.g. 24bpp lower-half = (uint32_t)-42 = 0xFFFFFFD6). On
// hardware this is harmless: dma.c forms (CurAddr + (voffs<<2)) & 0x1FFFFC, so the wrap is exactly
// modular subtraction — the destination steps BACKWARD relative to the running base, landing on the
// raster rows above. RAMOffsetWWS (words per block-row) is 6 for 24bpp, 4 for 16bpp, 0 for 4/8bpp.
// This reorders the MDEC's block-sequential output (Cr,Cb,Y0..Y3 per 16x16 macroblock) into the
// contiguous raster layout the game expects.
//
// We reproduce this with buf-relative addressing: word i is stored at buf[i + (int32_t)voffs_i].
// We interpret voffs as a SIGNED 32-bit displacement so the lower-half negative offsets subtract
// (i - 42), matching the hardware modular step. Within any macroblock the running base i has always
// advanced past the upper 8 rows (>= 48 words for 24bpp / 32 for 16bpp) before a negative offset
// occurs, so i + (int32_t)voffs is always >= 0 and never underflows the buffer (verified: the set
// of destinations for a frame is exactly the contiguous range [0, total_words), a permutation with
// no gaps/overlaps — see scratch/mdecfixdev/mdecfixtest.c).
//
// `buf` therefore represents the destination RAM region as a flat 32-bit-word array whose index 0
// is the channel's MADR. Returns the number of words consumed/produced (how far the linear base i
// advanced). The PM's DMA1 in mem.c should treat buf as the post-scatter image: copy buf[k] to
// MainRAM at (MADR + k*4) for k in [0, returned_count) — equivalently MainRAM[MADR/4 + k] = buf[k].
//
// BUFFER SIZING (important): the written index i+offs can exceed the returned count-1 because the
// interleave reaches FORWARD by up to RAMOffsetWWS*7 on the upper block-rows (e.g. word 0 of a
// 24bpp macroblock writes index 0, but the upper-row stride pushes later words up to +42). Over a
// whole frame the written set is exactly [0, returned_count), so a buffer of `count` words is the
// natural span; but `buf` MUST be sized to cover the full DMA1 transfer (the game's MADR region),
// NOT merely the per-call return value, or a forward-reaching write within the last partial group
// could touch one row beyond. In practice the caller passes the channel's whole word count.
// NOTE on count: real DMA1 only fires in >=0x20-word bursts (MDEC_DMACanRead gates on
// OutFIFO.in_count >= 0x20); the trailing partial group below 0x20 is drained by the game via the
// MDEC data-port reads, not DMA. So a strict DMACanRead-gated loop here returns a multiple-of-0x20
// prefix; the caller (mem.c) replicates the same burst gating it already uses for other channels.
//
// 4bpp/8bpp: for depth 0/1, RAMOffsetWWS == 0, so voffs is always 0 and placement degrades to a
// plain linear drain (buf[i]) — which is exactly what hardware does there (no interleave).
int mdec_dma_out(uint32_t* buf, int count) {
  int i;
  for (i = 0; i < count; i++) {
    if (!MDEC_DMACanRead())
      break;
    uint32_t offs;
    uint32_t v = MDEC_DMARead(&offs);
    // hardware: MainRAM[(CurAddr + (offs<<2)) & 0x1FFFFC] = v, CurAddr += 4 each word.
    // (int32_t)offs makes the lower-half-block negative displacement subtract correctly.
    buf[i + (int32_t)offs] = v;
  }
  return i;
}

// Drain the remaining OutFIFO words WITH the voffs scatter, ignoring the 0x20 DMA-burst gate
// (MDEC_DMARead reads whenever OutFIFO is non-empty, mdec.c:899; only MDEC_DMACanRead enforces
// the 0x20 burst). This is the END-OF-FRAME remainder (< 0x20 words) that DMACanRead won't
// release but that still belongs to the last macroblock's raster positions. Caller passes
// `buf` already advanced by the words drained so far (so i + offs is the cumulative dest).
// Without this the tail fell to a linear data-port read -> wrong positions -> garbage/black
// bits in the bottom-right macroblock.
int mdec_dma_out_rest(uint32_t* buf, int count) {
  int i;
  for (i = 0; i < count; i++) {
    if (MDEC_Read(0, 0x1F801824u) & 0x80000000u) break;   // status bit31 = OutFIFO empty
    uint32_t offs;
    uint32_t v = MDEC_DMARead(&offs);
    buf[i + (int32_t)offs] = v;
  }
  return i;
}

// Status helpers wrapping Beetle's DMA-readiness predicates, for a caller that gates its own
// DMA0/DMA1 transfers (>= 0x20 words available and the matching enable bit set in Control).
bool mdec_dma_can_write(void) { return MDEC_DMACanWrite(); }
bool mdec_dma_can_read(void)  { return MDEC_DMACanRead(); }

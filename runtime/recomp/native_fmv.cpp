#include "core.h"
#include "game.h"   // class Fmv lives on Game (game->fmv); this TU implements its methods
#include "c_subsys.h"
// Native FMV player for the Tomba!2 PC port.
//
// Plays PSX .STR movies (MOVIE/LOGO.STR, MOVIE/OP.STR) entirely with our own code,
// bypassing the game's PSX StrPlayer / CD-streaming machinery. Pipeline:
//
//   disc (libchdr, disc.c)  ->  STR demux (this file)
//     ->  BS (MDEC bitstream) VLC decode (this file)
//       ->  MDEC IDCT/YCbCr (Beetle mdec.c via mdec_beetle.c)
//         ->  VRAM upload + present (gpu_native.c)
//
// WHY a VLC decoder lives here: an .STR data sector's payload is NOT raw MDEC coefficient
// words. It is the PSX "BS" (bitstream) form — the run-length + DCT coefficients VLC-coded
// exactly as the PSX BIOS/libpress DecDCTvlc expects. Beetle's MDEC (WriteImageData)
// consumes the *decoded* code stream: 16-bit words where (V>>10)=zero-run,
// sign_ext(V&0x3FF)=coefficient, 0xFE00=end-of-block. So the on-disc work here is:
// demux STR -> concat chunk payloads -> VLC-decode BS into that MDEC code stream ->
// DMA into MDEC -> DMA decoded RGB555 out -> draw to VRAM -> present.
//
// STR data-sector sub-header (32 bytes, little-endian), verified vs LOGO.STR LBA 11491:
//   [0..1]   0x0160   magic ("STR data sector")
//   [2..3]   0x8001   sub-mode marker
//   [4..5]   chunk index within this frame (0..nchunks-1)
//   [6..7]   number of chunks (sectors) making up this frame
//   [8..11]  frame number (1-based)
//   [12..15] frame BS payload size in bytes
//   [16..17] frame width in pixels   (320)
//   [18..19] frame height in pixels  (240)
//   [20..31] misc (demux id, etc.)
//   [32..]   this chunk's BS payload bytes. The 8-byte BS frame header sits at the very
//            start of chunk 0's payload.
//
// BS frame header (first 8 bytes of the concatenated payload, little-endian):
//   [0..1] number of MDEC code words in the decoded stream (informational)
//   [2..3] 0x3800 magic
//   [4..5] qscale (quantization scale, applied as the DC/AC QScale)
//   [6..7] BS version (2 here)
//
// Self-contained module; the boot/front-end sequencers call game->fmv.play().
#include <stdint.h>
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

// ---- runtime pieces we call (declared here to avoid header churn; do NOT modify them) ----

#include "sbs.h"                        // class Sbs — PSXPORT_SBS harness active: skip the (blocking) intro FMVs
int      mdec_dma_can_read(void);

// Pump the MDEC decode state machine. Beetle's MDEC only advances its decode loop when it
// is given clock cycles (MDEC_Run); mdec_write/mdec_dma_in call MDEC_Run(0), which never
// makes the internal ClockCounter positive, so the decode parks at its cycle-budget check
// and produces NO output. With no real PSX scheduler in this native path we must pump a
// large clock count ourselves after feeding a command/data so the block decode runs to
// completion. (This is the native equivalent of the scheduler ticking the MDEC event.)
static void mdec_pump(void) { MDEC_Run(0x40000000); }

void gpu_gp1(Core*, uint32_t w);
void gpu_native_init(void);

// pad access via c->game->pad — class Pad on Game (see game.h): pollSdl(), buttons field
#define PAD_START 0x0008u                                   // Start button bit (active-low)

static int fmv_resolve_path(const char* path, uint32_t* out_lba, uint32_t* out_size);


#define SECTOR_USER   2048u
#define SUBHDR_LEN    32u

// decode-scratch sizes (heap members on Fmv, allocated on first play)
#define FMV_PAYLOAD_BYTES (512u * 1024u)   // concatenated BS payload
#define FMV_CODES_MAX     (512u * 1024u)   // MDEC run-level codes
#define FMV_INBUF_WORDS   (128u * 1024u)   // MDEC input words
#define FMV_OUTBUF_WORDS  (512u * 1024u)   // MDEC output words
#define MDEC0         0x1F801820u   // data port
#define MDEC1         0x1F801824u   // control/status port

// ====================================================================================
// Bit reader — PSX BS stores the stream as little-endian 16-bit units, but bits within
// each 16-bit unit are consumed MSB-first.
// ====================================================================================
typedef struct {
  const uint8_t* data;
  uint32_t       size;     // bytes available
  uint32_t       bytepos;
  uint32_t       bitbuf;   // bits left-aligned, MSB next to consume
  int            bitcnt;   // valid bits in bitbuf
} BitReader;

static void br_init(BitReader* b, const uint8_t* data, uint32_t size) {
  b->data = data; b->size = size; b->bytepos = 0; b->bitbuf = 0; b->bitcnt = 0;
}
static void br_refill(BitReader* b) {
  while (b->bitcnt <= 16 && b->bytepos + 1 < b->size) {
    uint16_t w = (uint16_t)(b->data[b->bytepos] | (b->data[b->bytepos + 1] << 8));
    b->bytepos += 2;
    b->bitbuf |= (uint32_t)w << (16 - b->bitcnt);
    b->bitcnt += 16;
  }
}
static uint32_t br_peek(BitReader* b, int n) {
  br_refill(b);
  if (n == 0) return 0;
  return (b->bitbuf >> (32 - n)) & (uint32_t)((1ull << n) - 1);
}
static void br_skip(BitReader* b, int n) { b->bitbuf <<= n; b->bitcnt -= n; }
static uint32_t br_get(BitReader* b, int n) { uint32_t v = br_peek(b, n); br_skip(b, n); return v; }
static int br_eof(BitReader* b) { return (b->bitcnt <= 0) && (b->bytepos + 1 >= b->size); }

// ====================================================================================
// PSX STR / MPEG-1 AC run/level VLC table — SINGLE source of truth.
//
// This is the standard MPEG-1 Table B-14 AC coefficient code set, which PSX STR/BS v2
// uses verbatim. The 111 entries below were generated directly from jpsxdec's
// ZeroRunLengthAcLookup_STR.AC_VARIABLE_LENGTH_CODES_MPEG1 (m35/jpsxdec) — NOT hand-rolled
// from memory — so the codes are authoritative. Each entry: { len, code, run, level }.
// Codes are matched MSB-first; the set is uniquely decodable, so a longest-prefix scan by
// length is exact. A trailing sign bit follows every (run,level) code (1=+, 0=-). Two
// patterns are special-cased in bs_decode_ac():
//   End-Of-Block (EOB) = "10"            (len 2)
//   Escape            = "000001"         (len 6) -> 6-bit run, 10-bit signed level
// (Frame-end padding "0111111111" for v2 simply fails to match and ends frame decode.)
//
// Correctness is PROVEN end-to-end: scratch/fmvdev/fmvtest.c decodes real LOGO.STR and
// OP.STR frames; OP.STR frames yield ~10k-13k AC codes and render coherent video (verified).
// ====================================================================================
typedef struct { uint8_t len; uint16_t code; uint8_t run; uint8_t level; } VLC;

static const VLC s_vlc[] = {
  // {len, code, run, level}  -- the canonical MPEG-1 / PSX STR AC VLC table.
  { 2, 0x00003,  0,  1 },  // 11
  { 3, 0x00003,  1,  1 },  // 011
  { 4, 0x00004,  0,  2 },  // 0100
  { 4, 0x00005,  2,  1 },  // 0101
  { 5, 0x00005,  0,  3 },  // 00101
  { 5, 0x00006,  4,  1 },  // 00110
  { 5, 0x00007,  3,  1 },  // 00111
  { 6, 0x00004,  7,  1 },  // 000100
  { 6, 0x00005,  6,  1 },  // 000101
  { 6, 0x00006,  1,  2 },  // 000110
  { 6, 0x00007,  5,  1 },  // 000111
  { 7, 0x00004,  2,  2 },  // 0000100
  { 7, 0x00005,  9,  1 },  // 0000101
  { 7, 0x00006,  0,  4 },  // 0000110
  { 7, 0x00007,  8,  1 },  // 0000111
  { 8, 0x00020, 13,  1 },  // 00100000
  { 8, 0x00021,  0,  6 },  // 00100001
  { 8, 0x00022, 12,  1 },  // 00100010
  { 8, 0x00023, 11,  1 },  // 00100011
  { 8, 0x00024,  3,  2 },  // 00100100
  { 8, 0x00025,  1,  3 },  // 00100101
  { 8, 0x00026,  0,  5 },  // 00100110
  { 8, 0x00027, 10,  1 },  // 00100111
  {10, 0x00008, 16,  1 },  // 0000001000
  {10, 0x00009,  5,  2 },  // 0000001001
  {10, 0x0000a,  0,  7 },  // 0000001010
  {10, 0x0000b,  2,  3 },  // 0000001011
  {10, 0x0000c,  1,  4 },  // 0000001100
  {10, 0x0000d, 15,  1 },  // 0000001101
  {10, 0x0000e, 14,  1 },  // 0000001110
  {10, 0x0000f,  4,  2 },  // 0000001111
  {12, 0x00010,  0, 11 },  // 000000010000
  {12, 0x00011,  8,  2 },  // 000000010001
  {12, 0x00012,  4,  3 },  // 000000010010
  {12, 0x00013,  0, 10 },  // 000000010011
  {12, 0x00014,  2,  4 },  // 000000010100
  {12, 0x00015,  7,  2 },  // 000000010101
  {12, 0x00016, 21,  1 },  // 000000010110
  {12, 0x00017, 20,  1 },  // 000000010111
  {12, 0x00018,  0,  9 },  // 000000011000
  {12, 0x00019, 19,  1 },  // 000000011001
  {12, 0x0001a, 18,  1 },  // 000000011010
  {12, 0x0001b,  1,  5 },  // 000000011011
  {12, 0x0001c,  3,  3 },  // 000000011100
  {12, 0x0001d,  0,  8 },  // 000000011101
  {12, 0x0001e,  6,  2 },  // 000000011110
  {12, 0x0001f, 17,  1 },  // 000000011111
  {13, 0x00010, 10,  2 },  // 0000000010000
  {13, 0x00011,  9,  2 },  // 0000000010001
  {13, 0x00012,  5,  3 },  // 0000000010010
  {13, 0x00013,  3,  4 },  // 0000000010011
  {13, 0x00014,  2,  5 },  // 0000000010100
  {13, 0x00015,  1,  7 },  // 0000000010101
  {13, 0x00016,  1,  6 },  // 0000000010110
  {13, 0x00017,  0, 15 },  // 0000000010111
  {13, 0x00018,  0, 14 },  // 0000000011000
  {13, 0x00019,  0, 13 },  // 0000000011001
  {13, 0x0001a,  0, 12 },  // 0000000011010
  {13, 0x0001b, 26,  1 },  // 0000000011011
  {13, 0x0001c, 25,  1 },  // 0000000011100
  {13, 0x0001d, 24,  1 },  // 0000000011101
  {13, 0x0001e, 23,  1 },  // 0000000011110
  {13, 0x0001f, 22,  1 },  // 0000000011111
  {14, 0x00010,  0, 31 },  // 00000000010000
  {14, 0x00011,  0, 30 },  // 00000000010001
  {14, 0x00012,  0, 29 },  // 00000000010010
  {14, 0x00013,  0, 28 },  // 00000000010011
  {14, 0x00014,  0, 27 },  // 00000000010100
  {14, 0x00015,  0, 26 },  // 00000000010101
  {14, 0x00016,  0, 25 },  // 00000000010110
  {14, 0x00017,  0, 24 },  // 00000000010111
  {14, 0x00018,  0, 23 },  // 00000000011000
  {14, 0x00019,  0, 22 },  // 00000000011001
  {14, 0x0001a,  0, 21 },  // 00000000011010
  {14, 0x0001b,  0, 20 },  // 00000000011011
  {14, 0x0001c,  0, 19 },  // 00000000011100
  {14, 0x0001d,  0, 18 },  // 00000000011101
  {14, 0x0001e,  0, 17 },  // 00000000011110
  {14, 0x0001f,  0, 16 },  // 00000000011111
  {15, 0x00010,  0, 40 },  // 000000000010000
  {15, 0x00011,  0, 39 },  // 000000000010001
  {15, 0x00012,  0, 38 },  // 000000000010010
  {15, 0x00013,  0, 37 },  // 000000000010011
  {15, 0x00014,  0, 36 },  // 000000000010100
  {15, 0x00015,  0, 35 },  // 000000000010101
  {15, 0x00016,  0, 34 },  // 000000000010110
  {15, 0x00017,  0, 33 },  // 000000000010111
  {15, 0x00018,  0, 32 },  // 000000000011000
  {15, 0x00019,  1, 14 },  // 000000000011001
  {15, 0x0001a,  1, 13 },  // 000000000011010
  {15, 0x0001b,  1, 12 },  // 000000000011011
  {15, 0x0001c,  1, 11 },  // 000000000011100
  {15, 0x0001d,  1, 10 },  // 000000000011101
  {15, 0x0001e,  1,  9 },  // 000000000011110
  {15, 0x0001f,  1,  8 },  // 000000000011111
  {16, 0x00010,  1, 18 },  // 0000000000010000
  {16, 0x00011,  1, 17 },  // 0000000000010001
  {16, 0x00012,  1, 16 },  // 0000000000010010
  {16, 0x00013,  1, 15 },  // 0000000000010011
  {16, 0x00014,  6,  3 },  // 0000000000010100
  {16, 0x00015, 16,  2 },  // 0000000000010101
  {16, 0x00016, 15,  2 },  // 0000000000010110
  {16, 0x00017, 14,  2 },  // 0000000000010111
  {16, 0x00018, 13,  2 },  // 0000000000011000
  {16, 0x00019, 12,  2 },  // 0000000000011001
  {16, 0x0001a, 11,  2 },  // 0000000000011010
  {16, 0x0001b, 31,  1 },  // 0000000000011011
  {16, 0x0001c, 30,  1 },  // 0000000000011100
  {16, 0x0001d, 29,  1 },  // 0000000000011101
  {16, 0x0001e, 28,  1 },  // 0000000000011110
  {16, 0x0001f, 27,  1 },  // 0000000000011111
};

// Decode one AC code from the bitstream.
// Returns: 0 = EOB, 1 = (run,level) pair set, -1 = bad code.
static int bs_decode_ac(BitReader* b, int* run, int* level) {
  // EOB = "10"
  if (br_peek(b, 2) == 0x2) { br_skip(b, 2); return 0; }
  // ESCAPE = "000001"  (6 zero-ish bits: 000001)
  if (br_peek(b, 6) == 0x1) {
    br_skip(b, 6);
    int r = (int)br_get(b, 6);
    int l = (int)br_get(b, 10);
    if (l & 0x200) l -= 0x400;
    *run = r; *level = l;
    return 1;
  }
  for (int n = 1; n <= 16; n++) {
    uint32_t v = br_peek(b, n);
    for (unsigned i = 0; i < sizeof(s_vlc)/sizeof(s_vlc[0]); i++) {
      if (s_vlc[i].len == n && s_vlc[i].code == v) {
        br_skip(b, n);
        int s = (int)br_get(b, 1);   // sign bit: 0 = positive, 1 = negative (MPEG-1 convention)
        int lv = s_vlc[i].level;
        *run = s_vlc[i].run;
        *level = s ? -lv : lv;
        return 1;
      }
    }
  }
  return -1;
}

// Decode an entire BS frame into the MDEC run-level code stream.
// Returns number of 16-bit codes written, or negative on error.
int Fmv::bsDecodeFrame(const uint8_t* payload, uint32_t payload_size,
                       int width, int height, uint16_t* codes, int max_codes) {
  if (payload_size < 8) return -1;
  uint16_t bs_q = (uint16_t)(payload[4] | (payload[5] << 8));
  int qscale = bs_q & 0x3F;
  if (qscale == 0) qscale = 1;
  if (cfg_dbg("fmv")) {
    uint16_t magic = (uint16_t)(payload[2] | (payload[3] << 8));
    uint16_t ver   = (uint16_t)(payload[6] | (payload[7] << 8));
    if (!bs_hdr_logged) { bs_hdr_logged = 1;
      fprintf(stderr, "[fmv] BS hdr: nwords=%u magic=%04x qscale=%d version=%u\n",
              (unsigned)(payload[0] | (payload[1] << 8)), magic, qscale, ver); }
  }

  BitReader br;
  br_init(&br, payload + 8, payload_size - 8);

  int mbx = (width + 15) / 16;
  int mby = (height + 15) / 16;
  int nblocks = mbx * mby * 6;

  int out = 0;
  for (int blk = 0; blk < nblocks; blk++) {
    if (br_eof(&br)) break;
    // BS v2: each block begins with a fixed 10-bit DC coefficient (signed), then run/level
    // AC VLC pairs until EOB. The MDEC first-word format carries QScale in bits[15:10] and
    // the DC coefficient in bits[9:0]. (Verified against LOGO.STR: the leading bits parse as
    // 10-bit DC values followed by "10" EOB on the fade-in frames.)
    int dc = (int)br_get(&br, 10);
    if (dc & 0x200) dc -= 0x400;
    if (out >= max_codes) return out;
    codes[out++] = (uint16_t)(((qscale & 0x3F) << 10) | (dc & 0x3FF));

    if (dconly < 0) dconly = cfg_on("PSXPORT_FMV_DCONLY") ? 1 : 0;
    for (;;) {
      int run, level;
      int r = bs_decode_ac(&br, &run, &level);
      if (r == 0) {                       // EOB
        if (out >= max_codes) return out;
        codes[out++] = 0xFE00;
        break;
      }
      if (r < 0) return (out > 0) ? out : -2;
      if (out >= max_codes) return out;
      if (!dconly)                        // DCONLY: consume AC bits (keep sync) but drop the code
        codes[out++] = (uint16_t)(((run & 0x3F) << 10) | (level & 0x3FF));
    }
  }
  return out;
}

// ====================================================================================
// MDEC table upload — REQUIRED before decoding.
//
// After MDEC_Power the quantization matrix and the IDCT cosine matrix are all-zero, so
// every coefficient dequantizes/IDCTs to 0 and the output is a flat black frame. The PSX
// uploads these once via libpress DecDCTReset(0): the standard luma/chroma quant table
// (cmd 2 = SetIqTab) and the scaled IDCT cosine table (cmd 3 = SetIdctTab). We do the same.
//
//   cmd 2 (0x60000000 | bit0): load the colour quant table (16 words = 64 luma bytes +
//          64 chroma bytes when bit0 set). MDEC packs 4 bytes/word into QMatrix[0..1].
//   cmd 3 (0x60000001? -> see mdec.c: (Command>>29)==3): load 32 words = 64 int16 IDCT
//          coefficients; mdec.c stores them transposed and >>3.
//
// The default quant table is the documented PSX standard table (same as MPEG-1 intra);
// the IDCT table is *computed* from the standard cosine basis (math, not magic data).
// ====================================================================================

// PSX/MPEG-1 standard intra quantization matrix, in zig-zag-natural (row-major) order.
// This is the canonical DecDCTReset default; identical for luma and chroma here.
static const uint8_t s_quant_default[64] = {
   2, 16, 19, 22, 26, 27, 29, 34,
  16, 16, 22, 24, 27, 29, 34, 37,
  19, 22, 26, 27, 29, 34, 34, 38,
  22, 22, 26, 27, 29, 34, 37, 40,
  22, 26, 27, 29, 32, 35, 40, 48,
  26, 27, 29, 32, 35, 40, 48, 58,
  26, 27, 29, 34, 38, 46, 56, 69,
  27, 29, 35, 38, 46, 56, 69, 83,
};

// mednafen's MDEC ZigZag scan (mdec.c). The quant table is consumed in SCAN order
// (dequant uses QMatrix[CoeffIndex]; mdec stores the uploaded table linearly, mdec.c:844),
// so the table must be uploaded in zigzag/scan order: qz[scan] = quant_raster[ZigZag[scan]].
// s_quant_default is the raster-order MPEG intra matrix; uploading it raw applied the wrong
// per-frequency weight to every AC coefficient (DC at index 0 is unaffected: ZigZag[0]==0) —
// the cause of the "DC clean, AC banded" artifact. Reorder it here.
static const uint8_t MDEC_ZIGZAG[64] = {
  0x00,0x08,0x01,0x02,0x09,0x10,0x18,0x11,0x0a,0x03,0x04,0x0b,0x12,0x19,0x20,0x28,
  0x21,0x1a,0x13,0x0c,0x05,0x06,0x0d,0x14,0x1b,0x22,0x29,0x30,0x38,0x31,0x2a,0x23,
  0x1c,0x15,0x0e,0x07,0x0f,0x16,0x1d,0x24,0x2b,0x32,0x39,0x3a,0x33,0x2c,0x25,0x1e,
  0x17,0x1f,0x26,0x2d,0x34,0x3b,0x3c,0x35,0x2e,0x27,0x2f,0x36,0x3d,0x3e,0x37,0x3f,
};

static void mdec_upload_tables(void) {
  // ---- cmd 2: SetIqTab (quant matrix). bit0=1 -> load both luma+chroma (32 words). ----
  // Pack 4 bytes per 32-bit word; 64 luma + 64 chroma bytes = 32 words. Reorder raster ->
  // zigzag so QMatrix[CoeffIndex] (scan order) holds the right per-frequency weight.
  uint8_t qz[64];
  for (int p = 0; p < 64; p++) qz[p] = s_quant_default[MDEC_ZIGZAG[p]];
  uint32_t qcmd = (2u << 29) | 1u;     // (Command>>29)==2, bit0 set => 0x10+0x10 words
  mdec_write(MDEC0, qcmd);
  uint32_t qwords[32];
  for (int i = 0; i < 16; i++) {       // luma 64 bytes (zigzag order)
    qwords[i] = (uint32_t)qz[i*4+0]
              | ((uint32_t)qz[i*4+1] << 8)
              | ((uint32_t)qz[i*4+2] << 16)
              | ((uint32_t)qz[i*4+3] << 24);
  }
  for (int i = 0; i < 16; i++) qwords[16+i] = qwords[i];   // chroma = same default
  mdec_dma_in(qwords, 32);
  mdec_pump();

  // ---- cmd 3: SetIdctTab (IDCT cosine matrix). 32 words = 64 int16 coefficients. ----
  // mdec.c stores IDCTMatrix[((idx&7)<<3)|((idx>>3)&7)] = (int16)val >> 3, so we supply
  // val = round(cos_basis * 8) for each of the 64 positions, in natural index order.
  // Basis: c(u) * cos((2x+1)*u*PI/16), c(0)=1/sqrt(2), else 1, scaled by sqrt(2/8)... The
  // PSX scale that makes the two IDCT passes (each >>15 after *IDCTMatrix) reconstruct is
  // such that the stored 16-bit value ~= round( (u? cos(...) : 1/sqrt2) * sqrt(2) * 0x5A82/... ).
  // We use the exact mednafen-compatible scale: value = round( basis * 0x16A0A ) >> ? — to
  // avoid guesswork we compute the same fixed-point table the BIOS does (see derivation).
  uint32_t icmd = (3u << 29);
  mdec_write(MDEC0, icmd);
  int16_t idct[64];
  for (int u = 0; u < 8; u++) {
    for (int x = 0; x < 8; x++) {
      double cu = (u == 0) ? (1.0 / sqrt(2.0)) : 1.0;
      double v = cu * cos((2.0 * x + 1.0) * u * M_PI / 16.0);
      // mednafen mdec.c stores IDCTMatrix = (int16)uploaded >> 3 (mdec.c:869), so the value
      // we upload must be round(basis * 2^15) — this is the canonical PSX BIOS table whose
      // u=0 entries are 0x5A82 (= round(cos(pi/4) * 32768) = 23170). Earlier code uploaded
      // round(basis * 0x5A82) << 3, which for u=0 is 16384<<3 = 131072 and SATURATES to
      // 0x7FFF; nearly every entry clamped, giving the wrong IDCT gain (washed-out flat
      // blocks + banding). 2^15 is the correct, non-saturating scale.
      int iv = (int)lround(v * 32768.0);
      if (iv >  32767) iv =  32767;
      if (iv < -32768) iv = -32768;
      idct[u * 8 + x] = (int16_t)iv;
    }
  }
  uint32_t iwords[32];
  for (int i = 0; i < 32; i++)
    iwords[i] = (uint16_t)idct[i*2] | ((uint32_t)(uint16_t)idct[i*2+1] << 16);
  mdec_dma_in(iwords, 32);
  mdec_pump();
}

// ====================================================================================
// MDEC feed (16bpp) + RGB555 extraction
// ====================================================================================
int Fmv::mdecDecodeToRgb555(const uint16_t* codes, int ncodes,
                            int width, int height, uint16_t* pixels) {
  mdec_write(MDEC1, 0x80000000);              // reset
  mdec_write(MDEC1, (1u << 30) | (1u << 29)); // enable DMA in + out
  mdec_upload_tables();                        // load quant + IDCT (else output is black)

  int nwords = (ncodes + 1) / 2;
  // Decode-macroblock command: [31:29]=1, [28:27]=depth(16bpp=3), [15:0]=param word count.
  uint32_t cmd = 0x30000000u | (0x3u << 27) | (uint32_t)(nwords & 0xFFFF);
  mdec_write(MDEC0, cmd);

  if (!inbuf) inbuf = (uint32_t*)malloc(FMV_INBUF_WORDS * 4);
  if (nwords > (int)FMV_INBUF_WORDS) return -1;
  for (int i = 0; i < nwords; i++) {
    uint16_t lo = codes[i * 2];
    uint16_t hi = (i * 2 + 1 < ncodes) ? codes[i * 2 + 1] : 0xFE00;
    inbuf[i] = (uint32_t)lo | ((uint32_t)hi << 16);
  }

  // MDEC emits the frame as a sequence of 16x16 macroblocks in raster order (left->right,
  // top->bottom). The mdec_dma_out voffs scatter makes each 128-word (16bpp) group a
  // self-contained 16x16 RASTER block; it does NOT tile the blocks across the frame width
  // (on real PSX the game DMAs each macroblock to its own computed address). So we drain
  // the whole stream linearly, then TILE each 16x16 block into the width x height frame.
  int total_words = (width * height) / 2;     // 16bpp: 2 px/word
  if (!outbuf) outbuf = (uint32_t*)malloc(FMV_OUTBUF_WORDS * 4);
  if (total_words > (int)FMV_OUTBUF_WORDS) return -2;
  memset(outbuf, 0, (size_t)total_words * 4);

  // CRITICAL: the MDEC InFIFO is small (~0x20 words). Pushing the whole frame at once would
  // silently DROP every word past the FIFO depth (MDEC_DMAWrite discards when full). So we
  // INTERLEAVE: push a burst of input, pump the decode (drains InFIFO -> fills OutFIFO),
  // drain OutFIFO, repeat — exactly how real DMA0/DMA1 ping-pong around the decoder. Keep
  // going until all input is consumed AND no more output drains.
  int in_pos = 0, got = 0, stall = 0;
  for (;;) {
    // Feed a burst when the InFIFO reports room. MDEC_DMACanWrite is true only when >=0x20
    // words are free, so a 0x10-word chunk is always safe (never dropped). Pump after each
    // chunk so the decoder drains the InFIFO before we top it up again.
    if (in_pos < nwords && mdec_dma_can_write()) {
      int chunk = nwords - in_pos; if (chunk > 0x10) chunk = 0x10;
      mdec_dma_in(&inbuf[in_pos], chunk);
      in_pos += chunk;
    }
    mdec_pump();
    int n = (got < total_words) ? mdec_dma_out(outbuf + got, total_words - got) : 0;
    got += n;
    if (in_pos >= nwords && n == 0) {
      if (++stall >= 4) break;   // input drained + several empty output pumps => done
    } else {
      stall = 0;
    }
    if (got >= total_words && in_pos >= nwords) break;
  }

  // Tail flush: mdec_dma_out only drains in >=0x20-word bursts (DMACanRead gate), so a final
  // sub-0x20 remainder can sit in the OutFIFO (the bottom-right macroblock's last words). On
  // real PSX the game reads that tail via the MDEC data port; we do the same with mdec_read,
  // appending linearly (this only affects the final partial block's intra-block row order).
  // Drain the final sub-0x20 remainder WITH the voffs scatter (mdec_dma_out_rest), so the last
  // macroblock's tail lands at its real raster positions instead of linearly (which produced
  // garbage/black bits in the bottom-right macroblock).
  int got_before_tail = got;
  int tail = mdec_dma_out_rest(outbuf + got, total_words - got);
  got += tail;
  if (cfg_dbg("fmv"))
    fprintf(stderr, "[fmv]   drain: %d scattered + %d tail-scatter = %d/%d total\n",
            got_before_tail, tail, got, total_words);

  // Tile 16x16 macroblocks (each 128 words = 256 px, raster within the block) into the frame.
  memset(pixels, 0, (size_t)width * height * 2);
  const uint16_t* mb = (const uint16_t*)outbuf;  // 2 px per outbuf word
  int mbx = (width + 15) / 16;
  int mby = (height + 15) / 16;
  int produced = got;                            // words actually drained
  if (cfg_dbg("fmv"))
    fprintf(stderr, "[fmv]   drained %d/%d words (%d macroblocks)\n", got, total_words, got/128);
  int blocks_avail = produced / 128;             // 128 (32-bit) words per 16x16 MB
  // Each 128-word (256 px) group is a 16x16 RASTER macroblock: mednafen emits four 8x8 Y
  // sub-blocks and mdec_dma_out's voffs scatter (RAMOffsetWWS=4) lays them out as a 16x16
  // raster (verified via PSXPORT_MDEC_OFFS). The game emits macroblocks COLUMN-MAJOR
  // (top->bottom within a column, then the next column right): emit index k -> source
  // (row=k%mby, col=k/mby). Placing them row-major sheared the frame (verified: a horizontal
  // logo feature landed at emit stride mby=15). PSXPORT_FMV_ROWMAJOR forces the old order
  // for A/B testing.
  int rowmajor = cfg_on("PSXPORT_FMV_ROWMAJOR") ? 1 : 0;
  for (int blk = 0; blk < blocks_avail; blk++) {
    int bx, by;
    if (rowmajor) { by = blk / mbx; bx = blk % mbx; }
    else          { bx = blk / mby; by = blk % mby; }   // column-major emit order
    if (bx >= mbx || by >= mby) continue;
    const uint16_t* src = mb + blk * 256;
    for (int yy = 0; yy < 16; yy++) {
      int fy = by * 16 + yy;
      if (fy >= height) break;
      for (int xx = 0; xx < 16; xx++) {
        int fx = bx * 16 + xx;
        if (fx >= width) break;
        pixels[fy * width + fx] = src[yy * 16 + xx];
      }
    }
  }
  return width * height;   // pixel count
}

// Upload an RGB555 frame to VRAM(0,0) and present it.
static void present_rgb555(Core* core, const uint16_t* pixels, int width, int height) {
  gpu_gp0(core, 0xA0000000u);                              // CPU->VRAM
  gpu_gp0(core, 0);                                         // dest (y<<16)|x = (0,0)
  gpu_gp0(core, ((uint32_t)height << 16) | (uint32_t)width);
  int npix = width * height;
  for (int i = 0; i < npix; i += 2) {
    uint32_t lo = pixels[i];
    uint32_t hi = (i + 1 < npix) ? pixels[i + 1] : 0;
    gpu_gp0(core, lo | (hi << 16));
  }
  // Display this region: start (0,0), hres, vrange (handler takes y1-y0 as height).
  gpu_gp1(core, 0x05000000u);
  uint32_t hcode = (width == 256) ? 0 : (width == 320) ? 1 : (width == 512) ? 2 : 3;
  gpu_gp1(core, 0x08000000u | hcode);
  gpu_gp1(core, 0x07000000u | (((uint32_t)(16 + height) & 0x3FF) << 10) | 16u);
  gpu_present(core);
}

// ====================================================================================
// CD-XA ADPCM audio decode (STR movies interleave XA audio sectors with MDEC video).
// Algorithm transcribed from mednafen psx/cdc.c (PS_CDC_XA_ProcessSector / DecodeXAADPCM) =
// the project oracle: 18 sound groups/sector, 8 units/group (4-bit), 28 samples/unit, with
// a 2-tap IIR (filter weights) over a per-channel 2-sample history that persists across
// sectors. coding byte (subheader[3]): bit0 stereo, bit2 set => 18900Hz else 37800Hz,
// bit4 set => 8-bit (we handle both via unit_index_shift).
// ====================================================================================

static const int32_t XA_W[16][2] = {
  {0,0},{60,0},{115,-52},{98,-55},{122,-60} };   // mednafen DecodeXAADPCM weights

static void xa_decode_unit(const uint8_t* in, int16_t* out, int shift, int filter) {
  for (int i = 0; i < 28; i++) {
    int32_t s = (int16_t)(in[i] << 8); s >>= shift;
    s += (out[i-1] * XA_W[filter][0] + out[i-2] * XA_W[filter][1]) >> 6;
    if (s < -32768) s = -32768; else if (s > 32767) s = 32767;
    out[i] = (int16_t)s;
  }
}

// Decode one XA audio sector (raw 2352B). Writes interleaved S16 stereo to `out`
// (out[2*n]=L, out[2*n+1]=R); returns stereo frame count (<=2016). `hist[ch][0..1]` is the
// per-channel history that MUST persist across sectors. `*freq` set to the sample rate.
// Non-static so the sound oracle (tools/fmv_compare) can diff it against a reference.
int xa_decode_sector(const uint8_t* raw, int16_t* out, int16_t hist[2][2], int* freq) {
  int coding = raw[19];                          // subheader coding byte (raw = sync12+hdr4+subhdr8+...)
  int ishift = (coding & 0x10) ? 0 : 1;          // unit_index_shift: 1 for 4-bit, 0 for 8-bit
  int stereo = coding & 0x01;
  int units  = 4 << ishift;                      // 8 (4-bit) / 4 (8-bit)
  if (freq) *freq = (coding & 0x04) ? 18900 : 37800;
  // Per-channel sample buffer. MONO sectors put ALL units on one channel: 4-bit mono =
  // 18 groups * 8 units * 28 = 4032 samples/sector (vs 2016 per channel when stereo splits
  // them). Size for the mono max, else mono voice (e.g. Tomba2 dialog, 18900Hz mono) overflows.
  static int16_t ch[2][4032 + 8];
  int cp[2] = {0,0};
  for (int group = 0; group < 18; group++) {
    const uint8_t* sg = raw + 24 + group * 128;  // 16 param bytes + 112 sample bytes
    for (int unit = 0; unit < units; unit++) {
      int param  = sg[(unit & 3) | ((unit & 4) << 1)];
      int pcopy  = sg[4 | (unit & 3) | ((unit & 4) << 1)];
      uint8_t ib[28];
      for (int i = 0; i < 28; i++) {
        uint8_t t = sg[16 + i * 4 + (unit >> ishift)];
        if (ishift) { t <<= (unit & 1) ? 0 : 4; t &= 0xf0; }
        ib[i] = t;
      }
      int ocn = (unit & 1) && stereo;
      int16_t ob[2 + 28];
      ob[0] = hist[ocn][0]; ob[1] = hist[ocn][1];
      xa_decode_unit(ib, ob + 2, param & 0x0F, param >> 4);
      hist[ocn][0] = ob[28]; hist[ocn][1] = ob[29];
      if (param != pcopy) memset(ob, 0, sizeof ob);  // corruption guard (mednafen parity)
      if (stereo) { for (int s = 0; s < 28; s++) ch[ocn][cp[ocn]++] = ob[2 + s]; }
      else { for (int s = 0; s < 28; s++) { ch[0][cp[0]++] = ob[2+s]; ch[1][cp[1]++] = ob[2+s]; } }
    }
  }
  int n = cp[0];
  for (int i = 0; i < n; i++) { out[2*i] = ch[0][i]; out[2*i+1] = ch[1][i]; }
  return n;
}

// ---- FMV audio output (dedicated SDL device at the XA rate) + audio-master pacing --------
#ifdef PSXPORT_SDL
#include <SDL3/SDL.h>
// SDL3 push-model audio stream bound to the default playback device, opened at the movie's XA rate.
void Fmv::audioOpen(int freq) {
  SDL_AudioStream* st = (SDL_AudioStream*)stream;
  if (cfg_on("PSXPORT_NOAUDIO")) return;
  if (st && stream_freq == freq) { SDL_ClearAudioStream(st); return; }
  if (st) { SDL_DestroyAudioStream(st); stream = st = 0; }
  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) return;
  SDL_AudioSpec spec; SDL_memset(&spec, 0, sizeof spec);
  spec.freq = freq; spec.format = SDL_AUDIO_S16; spec.channels = 2;
  st = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, NULL, NULL);
  stream = st;
  if (st) { stream_freq = freq; SDL_ResumeAudioStreamDevice(st); }
}
void Fmv::audioQueue(const int16_t* pcm, int frames) {
  if (stream) SDL_PutAudioStreamData((SDL_AudioStream*)stream, pcm, frames * 4);   // S16 stereo
}
void Fmv::audioClose() { if (stream) { SDL_ClearAudioStream((SDL_AudioStream*)stream); } }

// Pace playback to the AUDIO/media clock: media_frames audio sample-pairs at `freq` Hz define
// the elapsed media time; sleep until wall-clock catches up. This is the real PSX rate (the
// fixed-15fps guess was too slow). Polls input and returns 1 if Start was pressed (skip).
// uncapped (PSXPORT_FMV_FPS=0) disables pacing for fast headless dumps.
int Fmv::pace(long media_frames, int freq, uint32_t t0, int uncapped) {
  game->pad.pollSdl();
  int pressed = ((game->pad.buttons & PAD_START) == 0) && !start_prev;
  start_prev = (game->pad.buttons & PAD_START) == 0;
  if (uncapped || freq <= 0) return pressed;
  uint32_t target = (uint32_t)((long long)media_frames * 1000 / freq);
  while ((int)(SDL_GetTicks() - t0) < (int)target) {
    SDL_Delay(2);
    game->pad.pollSdl();
    if (((game->pad.buttons & PAD_START) == 0) && !start_prev) pressed = 1;
    start_prev = (game->pad.buttons & PAD_START) == 0;
  }
  return pressed;
}
#else
void Fmv::audioOpen(int freq) { (void)freq; }
void Fmv::audioQueue(const int16_t* p, int n) { (void)p; (void)n; }
void Fmv::audioClose() {}
int Fmv::pace(long m, int f, uint32_t t, int u) { (void)m;(void)f;(void)t;(void)u; return 0; }
#endif

Fmv::~Fmv() {
  free(payload_buf); free(codes_buf); free(pixels_buf);
  free(inbuf); free(outbuf); free(xa_pcm);
}

// ====================================================================================
// STR demux + top-level play
// ====================================================================================
int Fmv::playLba(uint32_t lba, uint32_t size_bytes) {
  Core* core = &game->core;
  // PSXPORT_SBS: the side-by-side debugger compares the FIELD (gameplay + render); the intro movies are
  // identical pre-field content, and this player is a BLOCKING decode loop whose Start-skip reads the raw
  // host pad (pad_poll_sdl), which the harness's auto-skip (repl-injected Start) can't drive — so leaving
  // it in would freeze both panes in the FMV. Skip it entirely (like a headless run does at the call site),
  // so the concurrent-lockstep nav reaches free-roam. Both cores skip identically, so they stay in step.
  if (game->sbs) return 0;   // SBS: skip FMV (see comment above)
  gpu_native_init();
  mdec_init();

  uint32_t nsectors = (size_bytes + SECTOR_USER - 1) / SECTOR_USER;
  if (!payload_buf) {
    payload_buf = (uint8_t*)malloc(FMV_PAYLOAD_BYTES);
    codes_buf   = (uint16_t*)malloc(FMV_CODES_MAX * 2);
    pixels_buf  = (uint16_t*)malloc(1024 * 512 * 2);
    xa_pcm      = (int16_t*)malloc(4032 * 2 * 2);   // mono sectors yield up to 4032 frames (see xa_decode_sector)
  }
  uint8_t*  payload = payload_buf;
  uint16_t* codes   = codes_buf;
  uint16_t* pixels  = pixels_buf;

  int frames = 0;
  uint32_t sec = 0;
  int cur_frame = -1;
  uint32_t paylen = 0;
  int fwidth = 320, fheight = 240;
  int expected_chunks = 0, got_chunks = 0;

  // Optional dev cap: PSXPORT_FMV_MAXFRAMES bounds how many frames to play (0/unset = all).
  // Used by the standalone proof to decode just the first frame quickly; harmless in prod.
  int max_frames = 0;
  { const char* mf = cfg_str("PSXPORT_FMV_MAXFRAMES"); if (mf && *mf) max_frames = atoi(mf); }

  // Audio: STR interleaves XA-ADPCM sectors with the video sectors. Decode them, play through
  // a dedicated SDL device at the XA rate, and pace VIDEO to the audio/media clock (the real
  // PSX rate). uncapped = PSXPORT_FMV_FPS=0 (headless dumps: no pacing, no audio device).
  // AUTO-UNCAP HEADLESS: a headless run has no viewer, so real-time FMV pacing only wastes wall-clock
  // (it was making a field probe take ~77s instead of ~1.4s). Fast-forward the FMV unless the user
  // explicitly set an FMV_FPS. Same principle as the windowed-gated gpu_pace/stub pacing.
  int uncapped = 0; { const char* f = cfg_str("PSXPORT_FMV_FPS"); if (f && *f) uncapped = (atoi(f) == 0);
                      else if (cfg_on("PSXPORT_VK_HEADLESS")) uncapped = 1; }
  int xa_freq = 37800;
  int16_t xa_hist[2][2] = {{0,0},{0,0}};
  long media_frames = 0;                       // cumulative audio sample-pairs = media clock
  start_prev = 1;                              // assume Start may be held from a prior movie
  uint32_t t0 = 0;
#ifdef PSXPORT_SDL
  t0 = SDL_GetTicks();
#endif
  int skipped = 0;
  uint8_t raw[2352];
  while (sec < nsectors) {
    if (!disc_read_raw(lba + sec, raw, 2352)) break;
    sec++;
    int submode = raw[18];

    if (submode & 0x04) {                       // XA-ADPCM audio sector
      int n = xa_decode_sector(raw, xa_pcm, xa_hist, &xa_freq);
      if (sec == 1 || media_frames == 0) audioOpen(xa_freq);
      audioQueue(xa_pcm, n);
      media_frames += n;
      if (pace(media_frames, xa_freq, t0, uncapped)) { skipped = 1; break; }
      continue;
    }

    const uint8_t* sbuf = raw + 24;             // Form1 video user data
    uint16_t magic = (uint16_t)(sbuf[0] | (sbuf[1] << 8));
    if (magic != 0x0160) continue;              // not a video data sector (padding)

    int chunk_idx = sbuf[4]  | (sbuf[5]  << 8);
    int nchunks   = sbuf[6]  | (sbuf[7]  << 8);
    int framenum  = sbuf[8]  | (sbuf[9]  << 8) | (sbuf[10] << 16) | (sbuf[11] << 24);
    int w         = sbuf[16] | (sbuf[17] << 8);
    int h         = sbuf[18] | (sbuf[19] << 8);

    if (chunk_idx == 0) {
      cur_frame = framenum; paylen = 0;
      expected_chunks = nchunks; got_chunks = 0;
      fwidth = w ? w : 320; fheight = h ? h : 240;
    }
    if (cur_frame != framenum) continue;        // out of sync; wait for next chunk-0

    uint32_t plen = SECTOR_USER - SUBHDR_LEN;
    if (paylen + plen <= FMV_PAYLOAD_BYTES) {
      memcpy(payload + paylen, sbuf + SUBHDR_LEN, plen);
      paylen += plen;
    }
    got_chunks++;

    if (expected_chunks > 0 && got_chunks >= expected_chunks) {
      int ncodes = bsDecodeFrame(payload, paylen, fwidth, fheight, codes,
                                 (int)FMV_CODES_MAX);
      if (cfg_dbg("fmv"))
        fprintf(stderr, "[fmv] frame %d: %dx%d, %u payload bytes, %d codes\n",
                framenum, fwidth, fheight, paylen, ncodes);
      if (ncodes > 0) {
        int np = mdecDecodeToRgb555(codes, ncodes, fwidth, fheight, pixels);
        if (np > 0) {
          present_rgb555(core, pixels, fwidth, fheight); frames++;
          // Pace video to the audio/media clock (no audio sector here, so just gate on it).
          if (pace(media_frames, xa_freq, t0, uncapped)) { skipped = 1; break; }
        }
      }
      cur_frame = -1; expected_chunks = 0; got_chunks = 0; paylen = 0;
      if (max_frames && frames >= max_frames) break;
    }
  }
  if (skipped) fprintf(stderr, "[fmv] skipped by Start at frame %d\n", frames);
  if (cfg_dbg("fmv"))
    fprintf(stderr, "[fmv] done: %d video frames, %ld audio sample-pairs (%.2fs @ %dHz)\n",
            frames, media_frames, media_frames / (double)(xa_freq ? xa_freq : 37800), xa_freq);
  audioClose();
  // FMV teardown (issues #7/#11): EVERY exit (normal end AND Start-skip break) leaves the FMV's last
  // (possibly partial) frame in the display FB. Black it + present once so no FMV residue is revealed
  // under the front-end's still-loading 2D layer. Engine-owned deterministic hand-off, no sleep/retry.
  void gpu_clear_display(Core*);
  gpu_clear_display(core);
  return frames;
}

int Fmv::play(const char* path) {
  uint32_t lba = 0, size = 0;
  if (!fmv_resolve_path(path, &lba, &size)) {
    fprintf(stderr, "[fmv] could not resolve %s on disc\n", path);
    return -1;
  }
  fprintf(stderr, "[fmv] %s -> LBA %u, %u bytes\n", path, lba, size);
  return playLba(lba, size);
}

// ---- ISO9660 path resolution (walks directories via disc_read_sector) ----------------
static uint32_t le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static void iso_name(const uint8_t* rec, int nlen, char* out, int outsz) {
  int j = 0;
  for (int i = 0; i < nlen && j < outsz - 1; i++) {
    char c = (char)rec[i];
    if (c == ';') break;
    if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    out[j++] = c;
  }
  out[j] = 0;
}
static int iso_find_child(uint32_t dir_lba, uint32_t dir_size, const char* name,
                          int want_dir, uint32_t* clba, uint32_t* csize) {
  char upper[256]; int n = 0;
  for (const char* p = name; *p && n < 255; p++) {
    char c = *p; if (c >= 'a' && c <= 'z') c = c - 'a' + 'A';
    upper[n++] = c;
  }
  upper[n] = 0;

  uint32_t nsec = (dir_size + SECTOR_USER - 1) / SECTOR_USER;
  uint8_t sbuf[SECTOR_USER];
  for (uint32_t s = 0; s < nsec; s++) {
    if (!disc_read_sector(dir_lba + s, sbuf)) return 0;
    uint32_t pos = 0;
    while (pos < SECTOR_USER) {
      uint8_t len = sbuf[pos];
      if (len == 0) break;
      if (pos + len > SECTOR_USER) break;
      uint8_t flags = sbuf[pos + 25];
      uint8_t nlen  = sbuf[pos + 32];
      uint32_t e_lba = le32(&sbuf[pos + 2]);
      uint32_t e_size = le32(&sbuf[pos + 10]);
      if (!(nlen == 1 && (sbuf[pos + 33] == 0 || sbuf[pos + 33] == 1))) {
        char nm[256]; iso_name(&sbuf[pos + 33], nlen, nm, sizeof nm);
        int is_dir = (flags & 0x02) ? 1 : 0;
        if (is_dir == want_dir && strcmp(nm, upper) == 0) {
          *clba = e_lba; *csize = e_size; return 1;
        }
      }
      pos += len;
    }
  }
  return 0;
}
static int fmv_resolve_path(const char* path, uint32_t* out_lba, uint32_t* out_size) {
  uint8_t pvd[SECTOR_USER];
  if (!disc_read_sector(16, pvd)) return 0;
  if (memcmp(pvd + 1, "CD001", 5) != 0) return 0;
  uint32_t dir_lba  = le32(pvd + 156 + 2);
  uint32_t dir_size = le32(pvd + 156 + 10);

  char comp[256]; const char* p = path;
  while (*p) {
    int n = 0;
    while (*p && *p != '/' && *p != '\\' && n < 255) comp[n++] = *p++;
    comp[n] = 0;
    while (*p == '/' || *p == '\\') p++;
    int last = (*p == 0);
    uint32_t clba = 0, csize = 0;
    if (!iso_find_child(dir_lba, dir_size, comp, last ? 0 : 1, &clba, &csize)) return 0;
    if (last) { *out_lba = clba; *out_size = csize; return 1; }
    dir_lba = clba; dir_size = csize;
  }
  return 0;
}

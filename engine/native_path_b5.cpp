// Hand-native boot→cutscene path — batch b5: the VRAM/block allocator 0x800977C0 (last on-path
// non-leaf). Isolated in its own file because it is the riskiest transcription so far (branchy
// split/coalesce); A/B-gated standalone. Sub-call 0x80097A90 (lock/compact) via rec_dispatch.
#include "core.h"
#include <stdint.h>

static inline void call_fn(Core* c, uint32_t fn) { rec_dispatch(c, fn); }

// 0x800977C0 — allocate a `size`-byte run from the block table at G_base (0x800AC66C). Each block is
// 8 bytes: word0 = flags(bit30 used / bit31 free) | addr(low 28 bits), word1 = size. Globals:
//   active 0x800AC59C, a 0x800AC5A0, shift 0x800AC62C, mask 0x800AC634,
//   base 0x800AC66C, count 0x800AC664, nextfree 0x800AC668.
// Rounds `size` up to mask then aligns to 1<<shift; scans for a used block or a big-enough free block;
// splits it (used → new block at idx+1; free → remainder block at nextfree) and returns its addr, or
// -1 on no fit. 0x80097A90 is a lock/compaction sub-routine called around the table mutation.
static void ov_800977C0(Core* c) {
  const uint32_t GACT=0x800AC59Cu, GA=0x800AC5A0u, GSH=0x800AC62Cu, GMASK=0x800AC634u;
  const uint32_t GBASE=0x800AC66Cu, GCOUNT=0x800AC664u, GNEXT=0x800AC668u;

  uint32_t size = c->r[4];
  uint32_t active = c->mem_r32(GACT);
  int found = -1;                                   // r18

  uint32_t adj;                                     // r19
  if (active != 0) {
    uint32_t sh = c->mem_r32(GSH);
    adj = (0x10000u - c->mem_r32(GA)) << (sh & 31);
  } else adj = 0;

  // round up toward mask, then align down to 1<<shift (arithmetic shift)
  uint32_t mask = c->mem_r32(GMASK);
  if ((size & ~mask) != 0) size += mask;
  uint32_t sh = c->mem_r32(GSH);
  size = (uint32_t)(((int32_t)size >> (sh & 31)) << (sh & 31));

  uint32_t base = c->mem_r32(GBASE);
  if ((c->mem_r32(base) & 0x40000000u) != 0) {
    found = 0;                                      // *base already used → take index 0
  } else {
    call_fn(c, 0x80097A90u);
    int count = (int32_t)c->mem_r32(GCOUNT);
    if (0 < count) {
      int i = 0;
      uint32_t bp = c->mem_r32(GBASE);              // &block[0]
      for (;;) {
        uint32_t blk = c->mem_r32(bp);
        bool hit = false;
        if (blk & 0x40000000u) hit = true;          // used → take it
        else if (blk & 0x80000000u) {               // free → take if big enough
          if (!(c->mem_r32(bp + 4) < size)) hit = true;
        }
        if (hit) { found = i; break; }
        i++;
        if (i < count) { bp += 8; continue; }
        break;
      }
    }
  }

  if (found == -1) { c->r[2] = (uint32_t)-1; return; }

  uint32_t basep = c->mem_r32(GBASE);
  uint32_t blkp = (uint32_t)(found << 3) + basep;
  uint32_t blk = c->mem_r32(blkp);

  if (blk & 0x40000000u) {
    // USED block → split a new used block in at idx+1
    int count = (int32_t)c->mem_r32(GCOUNT);
    if (!((int32_t)found < count)) { c->r[2] = (uint32_t)-1; return; }
    if ((c->mem_r32(blkp + 4) - adj) < size) { c->r[2] = (uint32_t)-1; return; }
    uint32_t nidx = (uint32_t)found + 1;
    uint32_t np = (nidx << 3) + basep;
    uint32_t w0 = (c->mem_r32(blkp) & 0x0FFFFFFFu) + size;
    c->mem_w32(np + 0, w0 | 0x40000000u);
    c->mem_w32(np + 4, c->mem_r32(blkp + 4) - size);
    uint32_t addr = c->mem_r32(blkp);
    c->mem_w32(GNEXT, nidx);
    c->mem_w32(blkp + 4, size);
    c->mem_w32(blkp + 0, addr & 0x0FFFFFFFu);
    call_fn(c, 0x80097A90u);
    c->r[2] = c->mem_r32((uint32_t)(found << 3) + c->mem_r32(GBASE));
    return;
  }

  // FREE block → carve `size` off the front, push the remainder as a new free block at nextfree
  uint32_t blksize = c->mem_r32(blkp + 4);
  if (size < blksize) {
    int nf = (int32_t)c->mem_r32(GNEXT);
    int count = (int32_t)c->mem_r32(GCOUNT);
    if (nf < count) {
      uint32_t np = ((uint32_t)nf << 3) + basep;
      uint32_t oldw0 = c->mem_r32(np + 0);
      uint32_t oldw1 = c->mem_r32(np + 4);
      c->mem_w32(np + 0, (blk + size) | 0x80000000u);
      c->mem_w32(np + 4, blksize - size);
      c->mem_w32(GNEXT, (uint32_t)nf + 1);
      c->mem_w32(np + 8, oldw0);
      c->mem_w32(np + 12, oldw1);
    }
  }
  uint32_t basep2 = c->mem_r32(GBASE);
  uint32_t blkp2 = (uint32_t)(found << 3) + basep2;
  uint32_t addr = c->mem_r32(blkp2) & 0x0FFFFFFFu;
  c->mem_w32(blkp2 + 4, size);
  c->mem_w32(blkp2 + 0, addr);
  call_fn(c, 0x80097A90u);
  c->r[2] = c->mem_r32((uint32_t)(found << 3) + c->mem_r32(GBASE));
}

void games_native_path_b5_init(void) {
  rec_set_override(0x800977C0u, ov_800977C0);
}

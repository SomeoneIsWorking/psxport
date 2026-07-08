// game/world/area_slots.cpp — AreaSlots method bodies. See area_slots.h for the per-method RE
// contracts; the 8 leaf callees stay substrate.

#include "world/area_slots.h"
#include "core.h"
#include "game.h"
#include "engine_overrides.h"

// AreaSlots::updateTail — the last direct child of ov_field_frame at guest 0x80075A80.
// Slot-table state machine over the 24-entry × 12-byte area at 0x800BE238; see area_slots.h for
// the per-arm contract. All 8 callees stay substrate (FUN_800998E4 buf-fill, FUN_80092660 action
// leaf, FUN_80098F90 mask-drain, FUN_80075824 + FUN_80099490 common tail, FUN_8008E0C0 key2 probe,
// FUN_80074BF8 / FUN_80074E48 sub-obj tails). Guest allocates 88 bytes of stack — the buffer address
// is passed to FUN_800998E4 and the action leaf's 4 stacked args, so we mirror the sp adjust.
void AreaSlots::updateTail() {
  Core* c = core;
  uint32_t sp_save = c->r[29];
  uint32_t ra_save = c->r[31];
  c->r[29] = sp_save - 88u;                    // addiu sp, -88
  const uint32_t sp = c->r[29];
  const uint32_t S5 = 0x800BE1F8u;
  const uint32_t buf_addr = sp + 0x20u;

  // (1) Fill the 24-byte per-slot state buffer for this frame.
  c->r[4] = buf_addr;
  rec_dispatch(c, 0x800998E4u);

  // (2) Slot loop over 24 entries at 0x800BE238 (12 bytes each), starting at the counter at 0x800BED78.
  int32_t s2  = (int32_t)c->mem_r32(0x800BED78u);
  uint32_t s1 = 0x800BE238u + (uint32_t)s2 * 12u;
  for (; s2 < 24; s2++, s1 += 12u) {
    const uint32_t s0 = s1 + 1u;
    uint8_t kind = c->mem_r8(s1);
    if (kind == 0xFF) {
      // Action arm — hword and a3 pick by top bit of slot[3]. slot[2..7] fill a2/a3 + 4 stack args.
      uint8_t s3b = c->mem_r8(s0 + 2u);
      int16_t hword;
      uint32_t a3;
      if (s3b & 0x80u) {
        hword = c->mem_r16s(0x800A4F7Eu);
        a3    = (uint32_t)(s3b & 0x0Fu);
      } else {
        hword = c->mem_r16s(0x800BED84u);
        a3    = (uint32_t)s3b;
      }
      c->mem_w32(sp + 0x10u, (uint32_t)c->mem_r8(s0 + 3u));
      c->mem_w32(sp + 0x14u, (uint32_t)c->mem_r8(s0 + 4u));
      c->mem_w32(sp + 0x18u, (uint32_t)c->mem_r8(s0 + 5u));
      c->mem_w32(sp + 0x1Cu, (uint32_t)c->mem_r8(s0 + 6u));
      c->r[4] = (uint32_t)(int32_t)(int16_t)s2;
      c->r[5] = (uint32_t)(int32_t)hword;
      c->r[6] = (uint32_t)c->mem_r8(s0);           // slot[2]
      c->r[7] = a3;
      rec_dispatch(c, 0x80092660u);
      uint32_t mask = c->mem_r32(0x800BE358u);     // clear bit s2 in the arm-mask
      mask &= ~(1u << (uint32_t)s2);
      c->mem_w32(0x800BE358u, mask);
      c->mem_w8(s1, (uint8_t)(kind - 1u));         // kind -= 1 (0xFF -> 0xFE)
      continue;                                     // skip buf post-check (guest goto 0x80075C14)
    }
    if (kind != 0) {
      // Other-kind arm — always decrement kind; if slot[8]==4 and it hit 0, set the arm-mask bit and
      // zero slot[2]+slot[3] (guest 0x80075BE4/0x80075BEC — both writes fire via the jr delay slot).
      uint8_t s8b    = c->mem_r8(s0 + 7u);
      uint8_t newkind = (uint8_t)(kind - 1u);
      c->mem_w8(s1, newkind);
      if (s8b == 4 && newkind == 0) {
        uint32_t mask = c->mem_r32(0x800BE358u);
        mask |= (1u << (uint32_t)s2);
        c->mem_w32(0x800BE358u, mask);
        c->mem_w8(s1 + 3u, 0);                     // slot[3]
        c->mem_w8(s1 + 2u, 0);                     // slot[2]
      }
    }
    // Buf post-check: buf[s2] in {0,3} -> zero slot[1]. Reached for kind==0 and the other-kind arm.
    uint8_t bv = c->mem_r8(buf_addr + (uint32_t)s2);
    if (bv == 0 || bv == 3) c->mem_w8(s1 + 1u, 0);
  }

  // (3) Mask-drain: if any bit set, call FUN_80098F90(0) then clear the mask.
  if (c->mem_r32(0x800BE358u) != 0) {
    c->r[4] = 0;
    rec_dispatch(c, 0x80098F90u);
    c->mem_w32(0x800BE358u, 0);
  }

  // (4) Common tail leaves — both take a0 = 0x800BE1F8.
  // Was: c->engine.musicCoord.musicFadeIn() — WRONG. FUN_80075824 was misnamed as musicFadeIn;
  // the RE (2026-07-03, ghidra) shows it is the per-voice VOLUME MIXER tick, not a fade snap.
  // SBS gameplay mode surfaced the divergence at 0x800BE208/A the moment we replaced the recomp
  // dispatch with musicFadeIn; the proper port is voiceMixTick(0x800BE1F8).
  c->engine.musicCoord.voiceMixTick(S5);            // FUN_80075824 (native)
  c->r[4] = S5; rec_dispatch(c, 0x80099490u);

  // (5) Key2 branch: if the s16 at 0x800BED80 != -1, look up the entry hword and probe with FUN_8008E0C0.
  int16_t key2 = c->mem_r16s(0x800BED80u);
  if (key2 != -1) {
    c->mem_w32(S5, 0);
    uint32_t entry_addr = 0x800BE368u + (uint32_t)((int32_t)key2 * 8);
    int16_t entry_hw = c->mem_r16s(entry_addr);
    c->r[4] = (uint32_t)(int32_t)entry_hw;
    c->r[5] = 0;
    rec_dispatch(c, 0x8008E0C0u);
    if ((c->r[2] & 0xFFFFu) == 0) {
      // Guest checked (return << 16) != 0 == (return & 0xFFFF) != 0.
      uint8_t subid = c->mem_r8(0x800BE22Au);
      if (subid == 0) {
        rec_dispatch(c, 0x80074E48u);
      } else {
        c->r[4] = (uint32_t)subid;
        rec_dispatch(c, 0x80074BF8u);
        c->mem_w16(0x800BED80u, 0);
        c->mem_w8(0x800BE22Au, 0);
      }
    }
  }

  c->r[29] = sp_save;
  c->r[31] = ra_save;
}

// AreaSlots::ackIfMatch — FUN_80074AF0 body. Pure 21-instruction primitive over the same
// slot table (0x800BE238, 12-byte stride) + armed-mask (0x800BE358) that updateTail iterates,
// but scoped to a SINGLE ack event: the caller passes an arg encoding {entryIdx: arg & 0xFF,
// signature: arg & 0xFFFFFF00}; if the signature matches the u32 stored at slot[idx].w0's high
// 3 bytes, set the armed bit AND clear the slot's trigger-pending byte at +1. Signature mismatch
// is a silent no-op (the recomp's `bne v0, a0, 0x80074B3C` short-circuits directly to jr ra).
// RE'd verbatim from disas 0x80074AF0..0x80074B40.
void AreaSlots::ackIfMatch(uint32_t arg) {
  Core* c = core;
  uint32_t idx = arg & 0xFFu;
  uint32_t entry = 0x800BE238u + idx * 12u;
  uint32_t stored_hi3 = c->mem_r32(entry) & 0xFFFFFF00u;
  uint32_t arg_hi3    = arg & 0xFFFFFF00u;
  if (stored_hi3 != arg_hi3) return;
  uint32_t mask = c->mem_r32(0x800BE358u);
  c->mem_w32(0x800BE358u, mask | (1u << idx));       // set armed bit `idx`
  c->mem_w8 (entry + 1, 0);                            // clear trigger-pending byte
}

// AreaSlots::primeCountdown — FUN_80074A38 body. Pure 1-store leaf: table[idx].kind = 10.
void AreaSlots::primeCountdown(uint32_t idx) {
  Core* c = core;
  uint32_t entry = 0x800BE238u + (idx & 0xFFu) * 12u;
  c->mem_w8(entry, 10);
}

// AreaSlots::updateCell — FUN_8007496C body. sigArg carries {idx: low byte, signature: high 3
// bytes} exactly like ackIfMatch's arg; on a signature mismatch this is a silent no-op (false).
// On match: dx/dy are adjusted by a fixed global cell-scroll offset (the byte at 0x800FB165,
// scaled), clamped to [0,127], written into the slot's cell-coord bytes (entry+6=dx, entry+7=dy),
// and the notifier FUN_80092E3C(idx, dx<<7, dy<<7) fires (kept substrate — outside this band).
// RE'd verbatim from Ghidra (scratch/decomp/cluster1.c: FUN_8007496c).
bool AreaSlots::updateCell(uint32_t sigArg, int32_t dx, int32_t dy) {
  Core* c = core;
  uint32_t idx = sigArg & 0xFFu;
  uint32_t entry = 0x800BE238u + idx * 12u;
  if ((c->mem_r32(entry) & 0xFFFFFF00u) != (sigArg & 0xFFFFFF00u)) return false;

  int32_t bias = ((int32_t)c->mem_r8(0x800FB165u) - 9) * 16;   // (9 - DAT_800FB165) * -16
  dx += bias;
  dy += bias;
  if (dx < 0) dx = 0; else if (dx > 0x7F) dx = 0x7F;
  if (dy < 0) dy = 0; else if (dy > 0x7F) dy = 0x7F;

  c->mem_w8(entry + 6, (uint8_t)dx);
  c->mem_w8(entry + 7, (uint8_t)dy);
  c->r[4] = idx; c->r[5] = (uint32_t)(dx << 7); c->r[6] = (uint32_t)(dy << 7);
  rec_dispatch(c, 0x80092E3Cu);
  return true;
}

static void eov_areaSlotsPrime(Core* c)      { c->engine.areaSlots.primeCountdown(c->r[4]); }
static void eov_areaSlotsUpdateCell(Core* c) { c->r[2] = c->engine.areaSlots.updateCell(c->r[4], (int32_t)c->r[5], (int32_t)c->r[6]) ? 1 : 0; }

void AreaSlots::registerOverrides() {
  EngineOverrides& ov = core->game->engine_overrides;
  ov.register_(0x80074A38u, "AreaSlots::primeCountdown", eov_areaSlotsPrime);
  ov.register_(0x8007496Cu, "AreaSlots::updateCell",     eov_areaSlotsUpdateCell);
}

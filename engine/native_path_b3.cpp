// Hand-native boot→cutscene path — batch b3: more non-leaf init/setup helpers along the
// ov_game_main init-prefix call tree (all static callees already native overrides).
// Sub-calls via rec_dispatch(c, addr). RE'd from the gen_func bodies; post-`jr ra` over-run dropped.
// A/B-verified (non-leaf: stack-region frame-slot diffs are benign; globals/pool/scratchpad must match).
#include "core.h"
#include <stdint.h>

static inline void call_fn(Core* c, uint32_t fn) { rec_dispatch(c, fn); }

// 0x800753AC — a1 = *(0x800BE108) + a1, then tail-call 0x8001DC40(a0, a1).
static void ov_800753AC(Core* c) {
  c->r[5] = c->mem_r32(0x800BE108u) + c->r[5];
  call_fn(c, 0x8001DC40u);
}

// 0x800834A0 — call 0x80085900(-1); store ret+240 at 0x800A5ADC and 0 at 0x800A5AE0.
static void ov_800834A0(Core* c) {
  c->r[4] = (uint32_t)-1; call_fn(c, 0x80085900u);
  c->mem_w32(0x800A5ADCu, c->r[2] + 240u);
  c->mem_w32(0x800A5AE0u, 0);
}

// 0x80045080 — index the stride-8 table at 0x800BE118 by (a1 & 255); load its two words into
// a1,a2 and tail-call 0x8001DC40(a0, tab[0], tab[1]).
static void ov_80045080(Core* c) {
  uint32_t tab = 0x800BE118u + (c->r[5] & 255u) * 8u;
  c->r[5] = c->mem_r32(tab);
  c->r[6] = c->mem_r32(tab + 4);
  call_fn(c, 0x8001DC40u);
}

// 0x80051F80 — fetch the descriptor ptr at scratch+312, write a0 to +2 and 1 to +0, then
// call 0x80080880(0xFF000000).
static void ov_80051F80(Core* c) {
  uint32_t a0 = c->r[4];
  uint32_t p = c->mem_r32(0x1F800138u);   // scratchpad + 312
  c->mem_w16(p + 2, (uint16_t)a0);
  c->mem_w16(p + 0, 1);
  c->r[4] = 0xFF000000u;
  call_fn(c, 0x80080880u);
}

// 0x8009C9D0 — finalize a GPU command packet: call 0x8009CAEC(a0,a1); then poke five GPU/DMA
// register cells (ptrs held in the 0x800AD0xx table) with composed words. v16 = (a1>>5)<<16.
static void ov_8009C9D0(Core* c) {
  uint32_t a0 = c->r[4], a1 = c->r[5];
  call_fn(c, 0x8009CAECu);                 // (a0, a1) — args already in r4/r5
  uint32_t v16 = (a1 >> 5) << 16;
  uint32_t p;
  p = c->mem_r32(0x800AD09Cu); c->mem_w32(p, c->mem_r32(p) | 136u);
  p = c->mem_r32(0x800AD064u); c->mem_w32(p, a0 + 4u);
  p = c->mem_r32(0x800AD068u); c->mem_w32(p, v16 | 32u);
  p = c->mem_r32(0x800AD094u); c->mem_w32(p, c->mem_r32(a0));
  p = c->mem_r32(0x800AD06Cu); c->mem_w32(p, 0x01000000u | 513u);
}

// 0x80089BAC — retry wrapper (up to 4 attempts) around 0x8008AC34. idx = a0 & 255.
// Returns 1 on success (3rd call returns 0), 0 if all attempts fail.
static void ov_80089BAC(Core* c) {
  uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6];
  uint32_t idx = a0 & 255u;
  uint32_t slot = 0x800ABF34u + idx * 4u;
  uint32_t saved = c->mem_r32(0x800ABFBCu);
  int attempt = 3;
  for (;;) {
    c->mem_w32(0x800ABFBCu, 0);
    if (idx != 1u && (c->mem_r8(0x800ABFC8u) & 16u) != 0u) {
      c->r[4]=1; c->r[5]=0; c->r[6]=0; c->r[7]=0; call_fn(c, 0x8008AC34u);
    }
    bool do_main = true;                     // the L_C78 block
    if (a1 != 0u && c->mem_r32(slot) != 0u) {
      c->r[4]=2; c->r[5]=a1; c->r[6]=a2; c->r[7]=0; call_fn(c, 0x8008AC34u);
      if (c->r[2] != 0u) do_main = false;    // -> retry (L_C9C)
    }
    if (do_main) {
      c->mem_w32(0x800ABFBCu, saved);
      c->r[4]=idx; c->r[5]=a1; c->r[6]=a2; c->r[7]=0; call_fn(c, 0x8008AC34u);
      if (c->r[2] == 0u) { c->r[2] = 1; return; }   // success
    }
    if (--attempt < 0) break;                // r16 reached -1
  }
  c->mem_w32(0x800ABFBCu, saved);
  c->r[2] = 0;
}

// 0x80089E1C — like 80089BAC but on success additionally calls 0x8008A6EC(0,a2) and returns
// (its ret == 2). Returns 0 if all attempts fail. idx = a0 & 255.
static void ov_80089E1C(Core* c) {
  uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6];
  uint32_t idx = a0 & 255u;
  uint32_t slot = 0x800ABF34u + idx * 4u;
  uint32_t saved = c->mem_r32(0x800ABFBCu);
  int attempt = 3;
  for (;;) {
    c->mem_w32(0x800ABFBCu, 0);
    if (idx != 1u && (c->mem_r8(0x800ABFC8u) & 16u) != 0u) {
      c->r[4]=1; c->r[5]=0; c->r[6]=0; c->r[7]=0; call_fn(c, 0x8008AC34u);
    }
    bool do_main = true;
    if (a1 != 0u && c->mem_r32(slot) != 0u) {
      c->r[4]=2; c->r[5]=a1; c->r[6]=a2; c->r[7]=0; call_fn(c, 0x8008AC34u);
      if (c->r[2] != 0u) do_main = false;    // -> retry
    }
    if (do_main) {
      c->mem_w32(0x800ABFBCu, saved);
      c->r[4]=idx; c->r[5]=a1; c->r[6]=a2; c->r[7]=0; call_fn(c, 0x8008AC34u);
      if (c->r[2] == 0u) {                    // success
        c->r[4]=0; c->r[5]=a2; call_fn(c, 0x8008A6ECu);
        c->r[2] = (c->r[2] == 2u) ? 1u : 0u;
        return;
      }
    }
    if (--attempt < 0) break;
  }
  c->mem_w32(0x800ABFBCu, saved);
  c->r[2] = 0;
}

// 0x800796DC — zero the 104-byte control block at 0x800BF808 (via 0x8009A420), seed two bytes,
// clear ~30 scratchpad fields, call 0x800782F0(b[168],b[169]) and 0x800508A8(), then select a
// uniform arg (0 if state byte == 0 or in {7,8}, else 255) for 0x8005082C(arg,arg,arg).
static void ov_800796DC(Core* c) {
  uint32_t b = 0x800BF808u;
  c->r[4]=b; c->r[5]=0; c->r[6]=104; call_fn(c, 0x8009A420u);   // memset(b, 0, 104)
  uint32_t a4 = c->mem_r8(0x800BF8B0u);
  uint32_t a5 = c->mem_r8(0x800BF8B1u);
  c->mem_w8(b + 41, 255);
  c->mem_w8(b + 40, 255);
  const uint32_t S = 0x1F800000u;            // scratchpad
  c->mem_w8 (S + 638, 0); c->mem_w32(S + 584, 0); c->mem_w8 (S + 582, 0);
  c->mem_w32(S + 588, 0); c->mem_w8 (S + 592, 0); c->mem_w8 (S + 601, 0);
  c->mem_w8 (S + 602, 0); c->mem_w8 (S + 603, 0); c->mem_w8 (S + 634, 0);
  c->mem_w8 (S + 560, 0);
  c->mem_w8 (b + 7, 1);
  c->mem_w8 (S + 310, 0); c->mem_w8 (S + 311, 0);
  c->mem_w32(S + 388, 0); c->mem_w32(S + 532, 0); c->mem_w32(S + 520, 0); c->mem_w32(S + 640, 0);
  c->mem_w8 (S + 561, 0); c->mem_w8 (S + 593, 0); c->mem_w8 (S + 562, 0);
  c->mem_w8 (S + 595, 0); c->mem_w8 (S + 563, 0); c->mem_w8 (S + 571, 0);
  c->r[4]=a4; c->r[5]=a5; call_fn(c, 0x800782F0u);
  call_fn(c, 0x800508A8u);
  uint32_t v = c->mem_r8(S + 566);
  uint32_t arg = (v == 0u || (uint32_t)(v - 7u) < 2u) ? 0u : 255u;
  c->r[4]=arg; c->r[5]=arg; c->r[6]=arg; call_fn(c, 0x8005082Cu);
  c->mem_w8(0x800BF9D4u, 0);
}

// 0x8007E9C8 — build a GPU primitive into a 12-word pool slot (pool head at 0x800BF544, indexed
// table ptr at 0x800ED8C8). Composes a header via scratchpad, links into the OT bucket a2, then
// calls 0x80083DE0(slot, 0, 0, len, /*arg5*/0) — STACK ARG at sp+16, so the gen 40-byte frame is
// replicated (c->r[29] -= 40 .. += 40) so the callee reads arg5 correctly without smashing the caller.
static void ov_8007E9C8(Core* c) {
  uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6];
  const uint32_t S = 0x1F800000u;            // scratchpad scratch struct
  c->mem_w32(S + 4, a0);
  c->mem_w8 (S + 7, 98);
  uint32_t p = c->mem_r32(0x800BF544u);
  c->mem_w16(S + 12, 320);
  c->mem_w16(S + 8, 0); c->mem_w16(S + 10, 0); c->mem_w16(S + 14, 240);
  uint32_t tabp = c->mem_r32(0x800ED8C8u) + a2 * 4u;
  c->mem_w32(p, c->mem_r32(tabp) | 0x03000000u);
  c->mem_w32(tabp, p);
  p += 4;
  c->mem_w32(p, c->mem_r32(S + 4));  p += 4;
  c->mem_w32(p, c->mem_r32(S + 8));  p += 4;
  c->mem_w32(p, c->mem_r32(S + 12)); p += 4;
  c->mem_w32(0x800BF544u, p);
  uint32_t len = ((a1 & 255u) != 0u) ? 32u : 64u;
  c->r[29] -= 40;                            // replicate gen frame for stack arg5
  c->mem_w32(c->r[29] + 16, 0);
  c->r[4] = p; c->r[5] = 0; c->r[6] = 0; c->r[7] = len;
  call_fn(c, 0x80083DE0u);
  c->r[29] += 40;
  tabp = c->mem_r32(0x800ED8C8u) + a2 * 4u;
  c->mem_w32(p, c->mem_r32(tabp) | 0x02000000u);
  c->mem_w32(tabp, p);
  uint32_t q = c->mem_r32(0x800BF544u);
  c->mem_w32(0x800BF544u, q + 12u);
}

// 0x800798F8 — seed five contiguous object arrays (each entry zeroed via 0x8009A420, +36 linked
// to the next entry, +40 = type tag, +12 = 0; last entry's +36 nulled), record each array's base
// ptr + count to globals, then wire up two scratchpad list-head pairs.
static void seed_array(Core* c, uint32_t base, uint32_t stride, int count, uint32_t tag) {
  for (int k = 0; k < count; k++) {
    uint32_t e = base + (uint32_t)k * stride;
    c->r[4]=e; c->r[5]=0; c->r[6]=stride; call_fn(c, 0x8009A420u);   // memset(e, 0, stride)
    c->mem_w32(e + 36, base + (uint32_t)(k + 1) * stride);          // next ptr
    c->mem_w8 (e + 40, (uint8_t)tag);
    c->mem_w8 (e + 12, 0);
  }
  c->mem_w32(base + (uint32_t)(count - 1) * stride + 36, 0);        // last entry next = 0
}
static void ov_800798F8(Core* c) {
  c->mem_w32(0x800FB168u, 0);
  c->mem_w32(0x800F23A8u, 0);
  c->mem_w32(0x800F2624u, 0);
  c->mem_w32(0x800F239Cu, 0);

  seed_array(c, 0x800ED8D8u, 136, 52, 0);
  c->mem_w32(0x800E8098u, 0x800ED8D8u); c->mem_w8(0x800E7E7Cu, 52);
  seed_array(c, 0x800EF478u, 196, 58, 1);
  c->mem_w32(0x800E80A0u, 0x800EF478u); c->mem_w8(0x800E7E7Du, 58);
  seed_array(c, 0x800FE198u, 208, 42, 2);
  c->mem_w32(0x800F2398u, 0x800FE198u); c->mem_w8(0x800ED8CCu, 42);
  seed_array(c, 0x800FB858u, 264, 40, 3);
  c->mem_w32(0x800ED8D4u, 0x800FB858u); c->mem_w8(0x800ED8C5u, 40);
  seed_array(c, 0x800FB218u, 320,  5, 4);
  c->mem_w32(0x800ED8D0u, 0x800FB218u); c->mem_w8(0x800ED8C4u, 5);

  const uint32_t S = 0x1F800000u;
  c->mem_w32(S + 316, 0x800F2410u); c->mem_w32(S + 320, 0x800F2410u); c->mem_w16(S + 326, 0);
  c->mem_w32(S + 328, 0x800F26C8u); c->mem_w32(S + 332, 0x800F26C8u);
  c->mem_w16(S + 338, 0); c->mem_w16(S + 336, 0);
  c->mem_w32(S + 340, 0x800F2738u); c->mem_w32(S + 344, 0x800F2738u);
  c->mem_w16(S + 350, 0); c->mem_w16(S + 348, 0);
}

void games_native_path_b3_init(void) {
}

// engine/objbeh_8004ce14.cpp — PC-native per-object BEHAVIOR handler FUN_8004CE14.
//
// A RESIDENT MAIN.EXE per-object behavior routine (prologue 0x8004CE14; `jr ra` at 0x8004D0CC).
// Same SHAPE as the sibling owned behaviors (objbeh_80071a3c / objbeh_739ac / …): a state machine on the
// node's state byte node[4]. RE'd 1:1 from disas 0x8004CE14:
//
//   STATE 2 : nothing.   STATE 3 : FUN_8007A624(node).   default(>=4): nothing.
//   STATE 0 : if (area-flag byte 0x800BF873 != 0) { node[4]=3; return; }
//             else: node[4]=1, node[0]=1, node[0x74]=0, FUN_8004B354(node,0), node[0x6c] = table
//             0x800A3F00[node[3]].  Then FALL THROUGH into the shared scan block (same as state 1).
//   STATE 1 : the shared scan block directly.
//
//   Shared scan block: decide run-vs-early-return from the area byte 0x800BF870 + node[3] + the camera
//   scroll byte 0x1F800207, then walk the 16-byte record list at node[0x6c] until a 0xFF terminator.
//   Per record, a short-circuit visibility test (record flag bit7 selects FUN_8004D7EC vs FUN_8004D868;
//   the persistent mask node[0x74] gates) decides whether to ACT: act = FUN_80111CCC(record[12]) when
//   area==1 && 0x800BF871>=15, else FUN_80077ACC(node, record[4], record[6], record[8]); a nonzero result
//   ORs bit s3 into node[0x70]. At the terminator: node[0x6a]=count, node[11]=31, FUN_80077EFC(node),
//   node[1]=1.
//
// Ownership model (identical to the siblings): CONTROL FLOW + the node/global memory WRITES owned native;
// every sub-behavior CALL stays reachable by address via rec_dispatch (leaf, no recursion). NO GTE, NO
// render packets. Gated byte-exact (full RAM+scratchpad A/B vs rec_super_call) like every owned behavior.
// v0 is NOT reproduced: the per-object dispatcher (call_handler) ignores the return; the gate compares
// only RAM+scratchpad — matching the sibling objbeh gates.

#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

constexpr uint32_t BEH_FN = 0x8004CE14u;

inline uint32_t lh(Core* c, uint32_t a) { return (uint32_t)(int32_t)(int16_t)c->mem_r16(a); }

void beh_8004ce14(Core* c) {
  uint32_t obj = c->r[4];
  uint32_t s5  = obj + 0x60;
  uint8_t state = c->mem_r8(obj + 4);

  if (state == 2) return;                                       // no-op
  if (state == 3) { c->r[4] = obj; rec_dispatch(c, 0x8007A624u); return; }
  if (state > 3) return;                                        // default

  if (state == 0) {
    if (c->mem_r8(0x800BF873u) != 0) { c->mem_w8(obj + 4, 3); return; }
    uint32_t d3 = c->mem_r8(obj + 3);
    c->mem_w8(obj + 4, 1);
    c->mem_w8(obj + 0, 1);
    uint32_t tv = c->mem_r32(0x800A3F00u + d3 * 4u);
    c->mem_w32(s5 + 20, 0);                                     // node[0x74] = 0
    c->r[4] = obj; c->r[5] = 0; rec_dispatch(c, 0x8004B354u);
    c->mem_w32(s5 + 12, tv);                                    // node[0x6c] = table value
  }

  // shared scan block (state 0 fall-through + state 1): early-return gate
  uint32_t area = c->mem_r8(0x800BF870u);
  uint32_t d3   = c->mem_r8(obj + 3);
  if (area == 0) {
    if (d3 == 1) {
      if (!(c->mem_r8(0x1F800207u) < 28)) return;
    } else if (d3 == 2) {
      if (c->mem_r8(0x1F800207u) < 28) return;
    }
  } else if (area == 6) {
    if (c->mem_r8(0x800BF871u) == 19) return;
  }

  // record-list scan
  uint32_t s4 = c->mem_r32(s5 + 12);                            // node[0x6c]
  c->mem_w32(s5 + 16, 0);                                       // node[0x70] = 0
  int s3 = 0;
  for (;;) {
    if (c->mem_r8(s4) == 0xFFu) {                               // terminator
      c->mem_w16(s5 + 10, (uint16_t)s3);                        // node[0x6a] = count
      c->mem_w8(obj + 11, 31);
      c->r[4] = obj; rec_dispatch(c, 0x80077EFCu);
      c->mem_w8(obj + 1, 1);
      return;
    }
    uint32_t s2 = c->mem_r8(s4 + 2) & 0x80u;
    uint32_t s1 = 1u << (s3 & 31);
    int verdict = -2;                                           // -2 = undecided, 0 = skip, 1 = act
    if (s2 == 0) {
      c->r[4] = lh(c, s4 + 10); c->r[5] = 0; rec_dispatch(c, 0x8004D7ECu);
      if (c->r[2] != 0) verdict = 0;
    }
    if (verdict == -2 && (c->mem_r32(obj + 0x74) & s1) != 0) verdict = 0;
    if (verdict == -2 && s2 != 0) {
      c->r[4] = lh(c, s4 + 10); c->r[5] = 0; rec_dispatch(c, 0x8004D868u);
      if (c->r[2] != 0) verdict = 0;
    }
    if (verdict == -2) verdict = ((c->mem_r32(obj + 0x74) & s1) != 0) ? 0 : 1;

    if (verdict == 1) {
      uint32_t ret;
      if (area == 1 && c->mem_r8(0x800BF871u) >= 15) {
        c->r[4] = c->mem_r8(s4 + 12); rec_dispatch(c, 0x80111CCCu);
        ret = c->r[2];
      } else {
        c->r[4] = obj; c->r[5] = lh(c, s4 + 4); c->r[6] = lh(c, s4 + 6); c->r[7] = lh(c, s4 + 8);
        rec_dispatch(c, 0x80077ACCu);
        ret = c->r[2];
      }
      if (ret != 0) c->mem_w32(s5 + 16, c->mem_r32(s5 + 16) | s1);
    }
    s3 += 1;
    s4 += 16;
  }
}

void ov_beh_8004ce14(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("obj8004ce14verify") ? 1 : 0;
  if (!s_v) { beh_8004ce14(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t obj = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  beh_8004ce14(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, BEH_FN);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[obj8004ce14verify] MISMATCH obj=%08x st=%u ram@%x spad@%x\n",
                           obj, c->mem_r8(obj + 4), ro, so);
  } else if (++ng % 50 == 0) fprintf(stderr, "[obj8004ce14verify] %ld matches\n", ng);
}

}  // namespace

void ov_beh_8004ce14_run(Core* c) { ov_beh_8004ce14(c); }

// class SceneTransition — see scene_transition.h. Implementations only.
//
// `areaMaskTrigger` is RE'd 1:1 from disas 0x800782F0 (docs L55158-L55186 in
// scratch/decomp/ram_f1000_all.c). Guest-memory-direct: the two resident tables
// (PTR_DAT_800A54A8 and DAT_800A55B0) live in MAIN.EXE .rodata and are read via mem_r32/mem_r8.
//
// Verify gate: `debug scene_transitionverify` runs the native + a `rec_super_call` snapshot
// against each other on every call and prints a first-mismatch address once the RAM diverges.
#include "scene_transition.h"
#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

namespace {

// Native RE (0x800782F0 body).
inline void areaMaskTrigger_impl(Core* c, uint8_t area, uint8_t sub) {
  if (area < 9) {
    // v1 = *(PTR_DAT_800A54A8 + area*4); v1 = *(u16*)(v1 + sub*8 + 6)
    uint32_t tbl_entry = c->mem_r32(0x800A54A8u + (uint32_t)area * 4u);
    uint16_t h        = c->mem_r16(tbl_entry + (uint32_t)sub * 8u + 6u);
    // shift  = ((DAT_800A55B0[area]) + ((h & 0x0600) >> 9)) & 0x1F
    uint8_t  base     = c->mem_r8(0x800A55B0u + area);
    uint32_t shift    = ((uint32_t)base + (((uint32_t)h & 0x0600u) >> 9)) & 0x1Fu;
    uint32_t reg      = c->mem_r32(0x800BFE50u);
    c->mem_w32(0x800BFE50u, reg | (1u << shift));
  }

  uint8_t cur = c->mem_r8(0x800BF870u);
  if (cur == 5) { c->mem_w8(0x800BF9DBu, (uint8_t)(c->mem_r8(0x800BF9DBu) | 0x02)); return; }
  if (cur == 6) { c->mem_w8(0x800BF9DBu, (uint8_t)(c->mem_r8(0x800BF9DBu) | 0x04)); return; }
  if (cur == 7) { c->mem_w8(0x800BF9DBu, (uint8_t)(c->mem_r8(0x800BF9DBu) | 0x08)); return; }
  if (cur == 8) { c->mem_w8(0x800BF9DBu, (uint8_t)(c->mem_r8(0x800BF9DBu) | 0x10));         }
}

}  // namespace

void SceneTransition::areaMaskTrigger(Core* c, uint8_t area, uint8_t sub) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("scene_transitionverify") ? 1 : 0;
  if (!s_v) { areaMaskTrigger_impl(c, area, sub); return; }

  // A/B against the substrate: snapshot RAM+scratchpad+regs, run native, snapshot ramN, restore,
  // rec_super_call, diff. Same shape as beh_scene_ui_trigger's verify wrapper.
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);

  areaMaskTrigger_impl(c, area, sub);

  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  c->r[4] = area; c->r[5] = sub;
  rec_super_call(c, 0x800782F0u);

  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1;
  for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1;
  for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[scene_transitionverify] MISMATCH area=%u sub=%u ram@%x spad@%x\n",
                           area, sub, ro, so);
  } else if (++ng % 50 == 0) fprintf(stderr, "[scene_transitionverify] %ld matches\n", ng);
}

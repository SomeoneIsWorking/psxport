// engine/sop.cpp — PC-native ownership of the SOP field-gameplay-mode overlay (the per-area scene
// loop reached once the GAME stage enters gameplay). The SOP field-mode machine 0x80109450 (sm[0x50]
// LOAD->FADE->GAMEPLAY) and its per-frame handler are the gameplay-start engine systems; this file
// owns them top-down. Map: scratch/gameplay_start_flow_re.md + scratch/sop_mode_re.md.
//
// FIRST OWNED PIECE — the area-DATA load (LAB_80109164, SOP.BIN 0x80109164).
// In the PSX flow, SOP state-0 spawns this as a COOPERATIVE task in slot 1 (FUN_80044bd4 ->
// FUN_80051f14(1, LAB_80109164)) and then BLOCKS on the byte *0x1f80019b until the task sets it.
// LAB_80109164 itself does only SYNCHRONOUS work — 4 CD reads via FUN_8001dc40 (= ov_cd_dc40, the
// native synchronous sector read), an unpack (FUN_80044e84), a collision-grid load (FUN_80045258,
// itself sync), and an ecf58 reloc-patch loop — then sets *0x1f80019b = 1 and calls FUN_80051fb4
// (task-complete/yield). We reimplement the BODY natively (the leaves stay dispatched as they are
// already sync) and DROP the task-complete yield: the native scheduler marks the slot done. This
// removes the load's cross-frame cooperative dependency (the prerequisite for owning the SOP machine
// as a native per-frame dispatcher) WITHOUT changing the observable result — *0x1f80019b ends 1 and
// ecf58[..] is patched exactly as the recomp body leaves it. RE: scratch/sop_mode_re.md + the disasm
// of 0x80109164 (faithful below, addresses annotated).

#include "core.h"
#include "cfg.h"
#include <stdio.h>

// Owned synchronous area-DATA load (replaces the body of LAB_80109164 0x80109164). Runs in the
// slot-1 task register context; uses c->r[] for the dispatched leaves' args; writes guest RAM.
void native_sop_area_load(Core* c) {
  uint32_t sm = c->mem_r32(0x1f800138u);
  c->mem_w8(sm + 0x6e, 3);                              // sm[0x6e] = 3 (area sub-index; 0x80109198)

  // LOAD 1 — FUN_8001dc40(0x800ef478, *0x800be0f0 + sm[0x6e], 2048)  (0x801091ac)
  uint8_t  a6e = c->mem_r8(sm + 0x6e);                  // = 3 (re-read, as the disasm does)
  c->r[4] = 0x800ef478u;
  c->r[5] = c->mem_r32(0x800be0f0u) + a6e;
  c->r[6] = 2048;
  rec_dispatch(c, 0x8001dc40u);

  // LOAD 2 — FUN_8001dc40(0x8018a000, *0x800be0f8 + (*0x800ef478>>11), *0x800ef47c - *0x800ef478)
  uint32_t l2 = c->mem_r32(0x800ef478u);               // (0x801091bc)
  c->r[4] = 0x8018a000u;
  c->r[5] = c->mem_r32(0x800be0f8u) + (l2 >> 11);
  c->r[6] = c->mem_r32(0x800ef47cu) - l2;
  rec_dispatch(c, 0x8001dc40u);

  // UNPACK — FUN_80044e84(0x8018a000, 0x1f8000)  (0x801091e4)
  c->r[4] = 0x8018a000u;
  c->r[5] = 0x001f8000u;
  rec_dispatch(c, 0x80044e84u);

  // LOAD 4 — FUN_8001dc40(0x8018a000, *0x800be100 + (*0x800ef480>>11), *0x800ef484 - *0x800ef480);
  //          *0x800a3ec8 = *0x800ef480>>11  (0x80109210/0x80109214)
  uint32_t l4 = c->mem_r32(0x800ef480u);
  c->mem_w32(0x800a3ec8u, l4 >> 11);
  c->r[4] = 0x8018a000u;
  c->r[5] = c->mem_r32(0x800be100u) + (l4 >> 11);
  c->r[6] = c->mem_r32(0x800ef484u) - l4;
  rec_dispatch(c, 0x8001dc40u);

  // COLLISION GRID — FUN_80045258((area&0xf)<<1, 0x2f)  (0x80109228)
  uint16_t area = c->mem_r16(0x800bf89eu);
  c->r[4] = (uint32_t)((area & 0xf) << 1);
  c->r[5] = 0x2f;
  rec_dispatch(c, 0x80045258u);

  // RELOC PATCH — for i in 0..*0x800ef488: ecf58[w>>24] = 0x8018a000 + (w & 0xffffff), w=*0x800ef48c[i]
  int32_t count = (int32_t)c->mem_r32(0x800ef488u);    // (0x80109234; blez skip if <=0)
  for (int32_t i = 0; i < count; i++) {
    uint32_t w   = c->mem_r32(0x800ef48cu + (uint32_t)i * 4);
    uint32_t idx = w >> 24;
    uint32_t off = w & 0x00ffffffu;
    c->mem_w32(0x800ecf58u + idx * 4, 0x8018a000u + off);
  }

  // LOAD DONE — *0x1f80019b = 1  (0x80109290). NB: the recomp's FUN_80051fb4 task-complete/yield is
  // intentionally DROPPED — the native scheduler marks the slot done after this returns.
  c->mem_w8(0x1f80019bu, 1);
  if (cfg_dbg("stage"))
    fprintf(stderr, "[sop] native area-load done: 1f80019b=1, ecf58 patched %d entries (area&0xf=%u)\n",
            count, (unsigned)(area & 0xf));
}

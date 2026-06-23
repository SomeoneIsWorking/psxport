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

// dispatch a still-recomp leaf with up to 3 args set (helper for the SOP machine).
static void d0(Core* c, uint32_t fn) { rec_dispatch(c, fn); }
static void d1(Core* c, uint32_t fn, uint32_t a0) { c->r[4]=a0; rec_dispatch(c, fn); }
static void d2(Core* c, uint32_t fn, uint32_t a0, uint32_t a1) { c->r[4]=a0; c->r[5]=a1; rec_dispatch(c, fn); }
static void d3(Core* c, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2) {
  c->r[4]=a0; c->r[5]=a1; c->r[6]=a2; rec_dispatch(c, fn);
}

// SOP FIELD-MODE MACHINE — native ownership of FUN_80109450 (decomp scratch/decomp/sop/80109450.c).
// Owns the sm[0x50] switch + every field write; dispatches the heavy per-state callees (those not yet
// owned native). CRITICAL: state 0 does NOT call FUN_80044bd4 (which clears 1f80019b, spawns the slot-1
// load task, and yields-waits — fatal to re-enter per-frame). It calls native_sop_area_load INLINE.
// Called from the native bridge ov_game_submode0 (per frame) once the GAME loop is native per-frame.
void ov_sop_field_mode(Core* c) {
  uint32_t sm = c->mem_r32(0x1f800138u);
  uint16_t st = c->mem_r16(sm + 0x50);
  switch (st) {
    case 0: {  // LOAD
      d3(c, 0x8007e9c8u, 0xffffffu, 0, 0);    // clear screen white
      native_sop_area_load(c);                 // INLINE sync load (replaces FUN_80044bd4) -> 1f80019b=1
      d0(c, 0x8007b18cu);
      d0(c, 0x800796dcu);
      d0(c, 0x80078610u);
      d1(c, 0x8010a8d4u, 0x800f2418u);         // SOP bg-ptr setup
      // 3 scene objects: spawn + stamp fields from the SOP overlay tables @0x8010c98c (stride 12).
      for (int i = 0; i < 3; i++) {
        d3(c, 0x8007a980u, 3, 3, 1);           // spawn -> node in v0
        uint32_t node = c->r[2];
        uint32_t t = 0x8010c98cu + (uint32_t)i * 12;
        c->mem_w16(node + 0x2e, c->mem_r16(t + 0));
        c->mem_w16(node + 0x32, c->mem_r16(t + 2));
        c->mem_w16(node + 0x36, c->mem_r16(t + 4));
        c->mem_w32(node + 0x1c, c->mem_r32(t + 8));   // per-scene handler (content)
      }
      d2(c, 0x8006cbd0u, 0x800e8008u, 0x8010c95cu);   // BG xform setup
      d2(c, 0x8006e3b0u, 0x800e8008u, 0x800e8040u);   // BG init
      sm = c->mem_r32(0x1f800138u);                   // (callees don't move sm, but reload defensively)
      c->mem_w16(sm + 0x50, 1);
      d0(c, 0x80075240u);
      c->mem_w16(sm + 0x60, 0x1e);
      c->mem_w16(sm + 0x52, 0);
      c->mem_w16(sm + 0x54, 0);
      c->mem_w8 (sm + 0x6c, 0x1f);
      c->mem_w8 (0x1f800137u, 1);
      break;
    }
    case 1: {  // FADE-IN
      uint32_t u = (uint32_t)c->mem_r8(sm + 0x6c) & 0x1f;
      d3(c, 0x8007e9c8u, (u << 19) | (u << 11) | (u << 3), 0, 0);
      uint8_t v = (uint8_t)(c->mem_r8(sm + 0x6c) - 1);
      c->mem_w8(sm + 0x6c, v);
      if (v == 0) { c->mem_w8(sm + 0x6c, 0x1f); c->mem_w16(sm + 0x50, (uint16_t)(c->mem_r16(sm + 0x50) + 1)); }
      d0(c, 0x801092b4u);                             // per-frame field update
      break;
    }
    case 2: {  // GAMEPLAY
      d0(c, 0x801092b4u);
      if (c->mem_r8(0x800bf839u) != 0 || (c->mem_r32(0x800e7e68u) & 8) != 0)
        c->mem_w16(sm + 0x50, (uint16_t)(c->mem_r16(sm + 0x50) + 1));
      break;
    }
    case 3: {  // FADE-OUT
      uint32_t u = ((uint32_t)c->mem_r8(sm + 0x6c) * (uint32_t)-8) & 0xff;
      d3(c, 0x8007e9c8u, (u << 16) | (u << 8) | u, 0, 0);
      uint8_t v = (uint8_t)(c->mem_r8(sm + 0x6c) - 1);
      c->mem_w8(sm + 0x6c, v);
      if (v == 0) c->mem_w16(sm + 0x50, (uint16_t)(c->mem_r16(sm + 0x50) + 1));
      d0(c, 0x801092b4u);
      break;
    }
    case 4: {  // RESET -> next area
      d0(c, 0x8001cf2cu);                             // kill load task slot 2 (settle CD)
      c->mem_w8(0x1f800137u, 0);
      int16_t s4c = (int16_t)c->mem_r16(sm + 0x4c);
      c->mem_w16(sm + 0x4e, 0);
      c->mem_w16(sm + 0x50, 0);
      c->mem_w16(sm + 0x52, 0);
      c->mem_w16(sm + 0x54, 0);
      c->mem_w16(sm + 0x4c, (uint16_t)(s4c + 1));
      c->mem_w8(0x800bf9b4u, 0);
      break;
    }
    default: return;
  }
}

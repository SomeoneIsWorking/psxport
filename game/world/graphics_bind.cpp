// game/world/graphics_bind.cpp — PC-native OBJECT RENDER-BIND subsystem (relocated verbatim from
// engine/entity_spawn.cpp). See graphics_bind.h.
#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "graphics_bind.h"
#include "verify_gate.h"

// Forward native scene-data record the decoupled native renderer will consume (geometry + float
// transform + texture). Populated in a later pass; the object subsystem will fill one of these per
// visible entity instead of building PSX render-command words. (No GTE/OT/GP0.)
struct SceneObject {
    uint32_t geomblk;        // model-space prim-list (GT3/GT4 records) guest ptr
    float    world[16];      // float model->world transform (column-major 4x4)
    uint32_t texture_id;     // native texture-cache key (resolved from tpage/clut at bind time)
};

// ====================================================================================================
// OBJECT RENDER-RECORD subsystem — per-object display-record allocation + init (shared by collectables
// and other entities; 15 call sites across the collectable handlers alone).
// ====================================================================================================
//
// FUN_8007AAE8 — the render-record BUMP ALLOCATOR. The record pool is a contiguous array of pre-built
// record pointers; a cursor at 0x800E7E74 points at the next free slot and a signed count at 0x800ED098
// tracks remaining slots. RE'd from disas 0x8007AAE8:
//   if ((s16)cnt <= 0) return 0;                      // pool empty
//   record = *cursor; cursor += 4; cnt--; return record;
static uint32_t record_alloc(Core* c) {
  int16_t cnt = (int16_t)c->mem_r16(0x800ED098u);
  if (cnt <= 0) return 0;
  uint32_t cursor = c->mem_r32(0x800E7E74u);
  c->mem_w16(0x800ED098u, (uint16_t)(cnt - 1));
  uint32_t record = c->mem_r32(cursor);
  c->mem_w32(0x800E7E74u, cursor + 4);
  return record;
}

// FUN_80051B70 — per-object render-record INIT. Allocates a record (FUN_8007AAE8), zero/init its fields
// (scale 0x1000), stamps the object's render fields, and stores a data pointer computed from two .data
// tables indexed by (a1, a2). If the record pool is exhausted it sets obj[+4]=3 (despawn-pending) and
// returns 1. RE'd from disas 0x80051B70:
//   if ((s16)*0x800ED098 <= 0) { obj[+4] = 3; return 1; }
//   obj[+8]=1; obj[+9]=1; obj[+0xd]=0; obj[+0xb8]=obj[+0xba]=obj[+0xbc]=0x1000;
//   rec = alloc(); obj[+0xc0] = rec;
//   rec[+6]=-1; rec[+0]=rec[+2]=rec[+4]=rec[+8]=rec[+0xa]=rec[+0xc]=0; rec[+0x38]=rec[+0x3a]=rec[+0x3c]=0x1000;
//   base = *(0x800ECF58 + a1*4);  rec[+0x40] = base + *(base + a2*4 + 4);
//   return 0;
static uint32_t obj_record_init(Core* c) {
  uint32_t obj = c->r[4], a1 = c->r[5], a2 = c->r[6];
  if ((int16_t)c->mem_r16(0x800ED098u) <= 0) { c->mem_w8(obj + 4, 3); return 1; }
  c->mem_w8(obj + 8, 1);
  c->mem_w8(obj + 9, 1);
  c->mem_w8(obj + 0xd, 0);
  c->mem_w16(obj + 0xbc, 0x1000);
  c->mem_w16(obj + 0xba, 0x1000);
  c->mem_w16(obj + 0xb8, 0x1000);
  rec_dispatch(c, 0x8007AAE8u);                  // allocate the record (native via ov_record_alloc)
  uint32_t rec = c->r[2];
  c->mem_w32(obj + 0xc0, rec);
  c->mem_w16(rec + 6, 0xffff);                   // -1
  c->mem_w16(rec + 0, 0);  c->mem_w16(rec + 2, 0);  c->mem_w16(rec + 4, 0);
  c->mem_w16(rec + 8, 0);  c->mem_w16(rec + 0xa, 0); c->mem_w16(rec + 0xc, 0);
  c->mem_w16(rec + 0x38, 0x1000); c->mem_w16(rec + 0x3a, 0x1000); c->mem_w16(rec + 0x3c, 0x1000);
  uint32_t base = c->mem_r32(0x800ECF58u + a1 * 4u);     // table[a1]
  uint32_t off  = c->mem_r32(base + a2 * 4u + 4u);        // *(table[a1] + a2*4 + 4)
  c->mem_w32(rec + 0x40, base + off);
  return 0;
}
void ov_record_alloc_g(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("recallocverify") ? 1 : 0;
  record_gate(c, record_alloc, 0x8007AAE8u, "recallocverify", s_v);
}
void ov_obj_record_init(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("recinitverify") ? 1 : 0;
  record_gate(c, obj_record_init, 0x80051B70u, "recinitverify", s_v);
}

// FUN_800517F8 — per-object RENDER-STATE UPDATE: build the object's transform, then snapshot its int16
// world position into the 32-bit render-position fields. RE'd from disas 0x800517F8:
//   FUN_80085480(obj+0x54, obj+0x98);                          // transform/matrix build (kept content)
//   obj[+0xac] = (s32)(s16)obj[+0x2e]; obj[+0xb0] = (s32)(s16)obj[+0x32]; obj[+0xb4] = (s32)(s16)obj[+0x36];
//   FUN_80051300(obj);                                          // downstream render setup (kept content)
// The two callees stay PSX via rec_dispatch; we own the control flow + the position snapshot.
static uint32_t obj_render_update(Core* c) {
  uint32_t obj = c->r[4];
  c->r[4] = obj + 0x54; c->r[5] = obj + 0x98;
  rec_dispatch(c, 0x80085480u);
  c->mem_w32(obj + 0xac, (uint32_t)(int32_t)(int16_t)c->mem_r16(obj + 0x2e));
  c->mem_w32(obj + 0xb0, (uint32_t)(int32_t)(int16_t)c->mem_r16(obj + 0x32));
  c->mem_w32(obj + 0xb4, (uint32_t)(int32_t)(int16_t)c->mem_r16(obj + 0x36));
  c->r[4] = obj;
  rec_dispatch(c, 0x80051300u);
  return c->r[2];
}
void ov_obj_render_update(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("rendupdverify") ? 1 : 0;
  record_gate(c, obj_render_update, 0x800517F8u, "rendupdverify", s_v);
}

// FUN_80077B38 — set an object's GEOMETRY-BLOCK pointer from a table. RE'd from disas 0x80077B38 (leaf):
//   ent = *(a1 + a2*4);  obj[+0x38] = ent;  obj[+0x0e] = (u16)ent[+2] & 0x3fff;  return that value.
static uint32_t obj_set_geom(Core* c) {
  uint32_t obj = c->r[4], tbl = c->r[5], idx = c->r[6];
  uint32_t ent = c->mem_r32(tbl + idx * 4u);
  uint32_t cnt = (uint32_t)(c->mem_r16(ent + 2) & 0x3fffu);
  c->mem_w32(obj + 0x38, ent);
  c->mem_w16(obj + 0x0e, (uint16_t)cnt);
  return cnt;   // incidental v0 the recomp leaves (callers treat this void)
}
void ov_obj_set_geom(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("setgeomverify") ? 1 : 0;
  record_gate(c, obj_set_geom, 0x80077B38u, "setgeomverify", s_v);
}

// FUN_8006CBD0 — copy a 6-halfword TRANSFORM BLOCK from a1 into the scratchpad camera/transform block
// (0x1F8000D2/D6/DA) + the object's rotation fields (obj+0x3a/0x3e/0x42). RE'd from disas 0x8006CBD0 (leaf):
//   *0x1F8000D2 = a1[0]; *0x1F8000D6 = a1[1]; *0x1F8000DA = a1[2];
//   obj[+0x3a] = a1[3]; obj[+0x3e] = a1[4]; obj[+0x42] = a1[5];
static uint32_t obj_set_xformblk(Core* c) {
  uint32_t obj = c->r[4], src = c->r[5];
  c->mem_w16(0x1F8000D2u, c->mem_r16(src + 0));
  c->mem_w16(0x1F8000D6u, c->mem_r16(src + 2));
  c->mem_w16(0x1F8000DAu, c->mem_r16(src + 4));
  c->mem_w16(obj + 0x3a, c->mem_r16(src + 6));
  c->mem_w16(obj + 0x3e, c->mem_r16(src + 8));
  uint32_t last = c->mem_r16(src + 0xa);
  c->mem_w16(obj + 0x42, (uint16_t)last);
  return last;   // incidental v0
}
void ov_obj_set_xformblk(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("setxblkverify") ? 1 : 0;
  record_gate(c, obj_set_xformblk, 0x8006CBD0u, "setxblkverify", s_v);
}

// FUN_8004BD64 — per-object POSITION-COMPOSE + render-state refresh. RE'd from disas 0x8004BD64
// (args: a0=obj, a1=mode&0xff, a2=srcA, a3=srcB, [sp+0x10]=t0 offset vector):
//   mode 0: obj[+0x2e/32/36] = roundedAvg(srcA[+0x2c/30/34], srcB...)   // 32-bit reads, (s+sign)>>1
//   mode 1: obj[+0x2e/32/36] = srcA[+0x2c/30/34] + t0[+0/2/4]            // 16-bit reads, add
//   mode 2: obj[+0x2e/32/36] = srcB[+0x2c/30/34] + t0[+0/2/4]
//   (other mode: no position write)
//   if ((obj[+0x28] & 0x7f) != 0) FUN_800517F8(obj);                     // refresh render-state (owned)
// v0 the recomp incidentally leaves = the last full (un-truncated) computed value (or 0x800517F8's return
// if the tail ran); we mirror it so the A/B v0 compare holds.
static uint32_t obj_pos_compose(Core* c) {
  uint32_t obj = c->r[4], mode = c->r[5] & 0xffu, srcA = c->r[6], srcB = c->r[7];
  uint32_t t0 = c->mem_r32(c->r[29] + 0x10);
  uint32_t last = c->r[2];   // default (other mode): v0 unchanged
  if (mode == 0) {
    for (int i = 0; i < 3; i++) {
      uint32_t so = 0x2c + (uint32_t)i * 4, oo = 0x2e + (uint32_t)i * 4;
      int32_t s = (int32_t)c->mem_r32(srcA + so) + (int32_t)c->mem_r32(srcB + so);
      s = (s + (int32_t)((uint32_t)s >> 31)) >> 1;
      c->mem_w16(obj + oo, (uint16_t)s);
      last = (uint32_t)s;
    }
  } else if (mode == 1 || mode == 2) {
    uint32_t src = (mode == 1) ? srcA : srcB;
    for (int i = 0; i < 3; i++) {
      uint32_t so = 0x2c + (uint32_t)i * 4, oo = 0x2e + (uint32_t)i * 4;
      uint32_t sum = (uint32_t)c->mem_r16(src + so) + (uint32_t)c->mem_r16(t0 + (uint32_t)i * 2);
      c->mem_w16(obj + oo, (uint16_t)sum);
      last = sum;
    }
  }
  if ((c->mem_r8(obj + 0x28) & 0x7fu) != 0) {
    c->r[4] = obj;
    ov_obj_render_update(c);
    return c->r[2];
  }
  return last;
}
void ov_obj_pos_compose(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("poscomposeverify") ? 1 : 0;
  record_gate(c, obj_pos_compose, 0x8004BD64u, "poscomposeverify", s_v);
}

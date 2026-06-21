// engine/entity_spawn.cpp — PC-native ENTITY SPAWN / PLACEMENT subsystem.
//
// FUN_80079C3C — the engine's core SPAWN PRIMITIVE: instantiate a new game object into the active object
// pool. It pops a node from the shared free-list, links it into one of three live doubly-linked object
// lists at a requested position, and stamps the node's identity fields. This is the function the per-type
// spawn dispatchers (FUN_8007A980 / FUN_8007AA38, via tables 0x80016E4C / 0x80016E64) tail-call to actually
// place an NPC/item/effect node — so it runs on every spawn on the field. Pure pool/list memory: NO GTE,
// NO render packets. Reimplemented PC-native; the per-type dispatch tables + their handlers stay PSX
// (content-side type routing). The recomp body is the reference/super-call for the `spawnverify` gate.
//
// ABI:  node* spawn(a0 = ref, a1 = type_byte, a2 = mode, a3 = list_idx)  -> v0 = node (or 0 if pool full)
//
// POOL (built by the pool init FUN_800798F8):
//   free-list head  = u32 @ 0x800E8098   (singly-linked via node[+0x24] = node[36])
//   free count      = u8  @ 0x800E7E7C   (the pool has three free-lists; this 208-byte-node list is one)
//   THREE active lists, selected by a3, each a doubly-linked list (prev=node[+0x20]=node[32],
//   next=node[+0x24]=node[36]) with a HEAD ptr (first / prev-end) and a TAIL ptr (last / next-end):
//     a3==0 -> head 0x800FB168, tail 0x800F23A8
//     a3==1 -> head 0x800F2624, tail 0x800F239C
//     a3==2 -> head 0x800F2738, tail 0x800F23A0
//
// BODY (RE'd verbatim from disas 0x80079C3C):
//   cnt = u8[0x800E7E7C]; if (cnt < 3) return 0;          // pool-low guard (keeps >=2 nodes spare)
//   node = u32[0x800E8098];                                // pop free head
//   u8[0x800E7E7C] = cnt - 1;
//   u32[0x800E8098] = u32[node+36];                        // free head = popped node's next
//   // link by mode a2 (positions are relative to ref = a0):
//   //   a2==0  insert BEFORE ref (falls back to HEAD insert if ref->prev == 0)
//   //   a2==1  insert at list HEAD
//   //   a2==2  insert AFTER  ref (falls back to TAIL insert if ref->next == 0)
//   //   a2==3  insert at list TAIL
//   //   other  no link (node left unlinked, just stamped)
//   // stamp identity (all paths):
//   u8[node+10] = a3;  u8[node+0] = 2;  u8[node+12] = a1;  return node;
//
// node[+0]  = active byte (2 = live)   node[+0x0A]=node[10] = list id   node[+0x0C]=node[12] = entity type
// node[+0x20]=node[32] = prev link     node[+0x24]=node[36] = next link
//
// `spawnverify` gate = full main-RAM (0x200000) + SCRATCHPAD (0x400) A/B vs rec_super_call(0x80079C3C),
// over N live spawns. Native run -> snapshot+rollback -> super-call -> diff. The recomp body's own 40-byte
// stack frame is dead below entry sp on return; exclude [sp-0x800, sp) (sp ~0x1FExxx, RAM end 0x200000 —
// far above all pool/game data; a real divergence alters persistent state). v0 (the node ptr) is compared.

#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "entity_spawn.h"
void rec_super_call(Core*, uint32_t);

// Pool addresses.
static const uint32_t FREE_HEAD = 0x800E8098u;
static const uint32_t FREE_CNT  = 0x800E7E7Cu;

// (head, tail) per list index a3 (0..2). Indices outside 0..2 land on list 0 (the MIPS default branch).
static const uint32_t LIST_HEAD[3] = { 0x800FB168u, 0x800F2624u, 0x800F2738u };
static const uint32_t LIST_TAIL[3] = { 0x800F23A8u, 0x800F239Cu, 0x800F23A0u };

// Link `node` into active list `list` at position `mode` relative to `ref`, then stamp identity. This is
// the body shared by ALL FIVE pool spawn primitives — pool-208 FUN_80079C3C, pool-2 FUN_80079DDC, and the
// three pool variants FUN_80079F90 / FUN_8007A12C / FUN_8007A2C8 link + stamp identically (verified from all
// disasms); only the free-list they pop from differs. When the target active list is EMPTY, the recomp
// initializes BOTH end pointers (a head-insert also writes *tail, a tail-insert also writes *head); we match
// that (caught by spawnvarverify: empty-list inserts that the seaside-only spawnverify never exercised).
static void spawn_link_stamp(Core* c, uint32_t node, uint32_t ref, uint32_t type, uint32_t mode, uint32_t list) {
  // Resolve the selected active list's head/tail vars (MIPS: a3==1 -> list1, a3==2 -> list2, else list0).
  uint32_t head, tail;
  if (list == 1)      { head = LIST_HEAD[1]; tail = LIST_TAIL[1]; }
  else if (list == 2) { head = LIST_HEAD[2]; tail = LIST_TAIL[2]; }
  else                { head = LIST_HEAD[0]; tail = LIST_TAIL[0]; }

  // Insert by mode. Both "before" and "after" fall through to a head/tail insert when the neighbor link
  // is null (matches the recomp's branch-to-0x80079d28 / 0x80079d90 fallbacks).
  bool do_head = false, do_tail = false;
  if (mode == 0) {                                  // insert BEFORE ref
    uint32_t prev = c->mem_r32(ref + 32);
    if (prev == 0) do_head = true;
    else {
      c->mem_w32(node + 32, prev);                  // node->prev = ref->prev
      c->mem_w32(node + 36, ref);                   // node->next = ref
      c->mem_w32(prev + 36, node);                  // ref->prev->next = node
      c->mem_w32(ref  + 32, node);                  // ref->prev = node
    }
  } else if (mode == 1) {                           // insert at HEAD
    do_head = true;
  } else if (mode == 2) {                           // insert AFTER ref
    uint32_t next = c->mem_r32(ref + 36);
    if (next == 0) do_tail = true;
    else {
      c->mem_w32(node + 32, ref);                   // node->prev = ref
      c->mem_w32(node + 36, next);                  // node->next = ref->next
      c->mem_w32(next + 32, node);                  // ref->next->prev = node
      c->mem_w32(ref  + 36, node);                  // ref->next = node
    }
  } else if (mode == 3) {                           // insert at TAIL
    do_tail = true;
  }
  // else: no link.

  if (do_head) {
    c->mem_w32(node + 32, 0);                        // node->prev = 0
    uint32_t old = c->mem_r32(head);
    c->mem_w32(node + 36, old);                      // node->next = *head
    if (old != 0) c->mem_w32(old + 32, node);        // (*head)->prev = node
    else          c->mem_w32(tail, node);            // list was EMPTY -> also init the tail ptr (recomp does this)
    c->mem_w32(head, node);                          // *head = node
  } else if (do_tail) {
    c->mem_w32(node + 36, 0);                        // node->next = 0
    uint32_t old = c->mem_r32(tail);
    c->mem_w32(node + 32, old);                      // node->prev = *tail
    if (old != 0) c->mem_w32(old + 36, node);        // (*tail)->next = node
    else          c->mem_w32(head, node);            // list was EMPTY -> also init the head ptr (recomp does this)
    c->mem_w32(tail, node);                          // *tail = node
  }

  // Stamp identity (all paths).
  c->mem_w8(node + 10, (uint8_t)list);
  c->mem_w8(node + 0,  2);
  c->mem_w8(node + 12, (uint8_t)type);
}

static uint32_t entity_spawn(Core* c) {
  const uint32_t ref  = c->r[4];          // a0
  const uint32_t type = c->r[5] & 0xffu;  // a1 (stored as u8 -> node[12])
  const uint32_t mode = c->r[6];          // a2 (insertion mode)
  const uint32_t list = c->r[7];          // a3 (list id -> node[10])

  uint32_t cnt = c->mem_r8(FREE_CNT);
  if (cnt < 3) return 0;                   // pool-low guard

  uint32_t node = c->mem_r32(FREE_HEAD);
  c->mem_w8(FREE_CNT, (uint8_t)(cnt - 1));
  c->mem_w32(FREE_HEAD, c->mem_r32(node + 36));   // free head = node->next

  spawn_link_stamp(c, node, ref, type, mode, list);
  return node;
}

// FUN_80079DDC — the POOL-2 spawn primitive (spawn variant class 1). Same link/stamp as FUN_80079C3C but
// pops a DIFFERENT free-list (head 0x800E80A0, count 0x800E7E7D), has NO pool-low guard, and when its pool
// is EMPTY it DELEGATES to variant 2 (FUN_80079F90) with the original (ref, type&0xff, mode, list). RE'd
// from disas 0x80079DDC. The fallback target stays content (rec_dispatch).
static const uint32_t POOL2_HEAD = 0x800E80A0u, POOL2_CNT = 0x800E7E7Du;
static uint32_t spawn_pool2(Core* c) {
  const uint32_t ref = c->r[4], type = c->r[5] & 0xffu, mode = c->r[6], list = c->r[7];
  uint32_t node = c->mem_r32(POOL2_HEAD);
  if (node == 0) {                                  // pool empty -> delegate to FUN_80079F90(ref,type,mode,list)
    c->r[4] = ref; c->r[5] = type;                  // a2/a3 already hold mode/list, untouched
    rec_dispatch(c, 0x80079F90u);
    return c->r[2];
  }
  uint32_t cnt = c->mem_r8(POOL2_CNT);
  c->mem_w8(POOL2_CNT, (uint8_t)(cnt - 1));
  c->mem_w32(POOL2_HEAD, c->mem_r32(node + 36));    // free head = node->next
  spawn_link_stamp(c, node, ref, type, mode, list);
  return node;
}

void ov_entity_spawn(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("spawnverify") ? 1 : 0;
  if (!s_v) { c->r[2] = entity_spawn(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t a0 = c->r[4], a2 = c->r[6], a3 = c->r[7];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  c->r[2] = entity_spawn(c);
  uint32_t v0_n = c->r[2];
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, 0x80079C3Cu);
  uint32_t v0_o = c->r[2];
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0 || v0_n != v0_o) {
    if (nb++ < 40) fprintf(stderr, "[spawnverify] MISMATCH ref=%08x mode=%u list=%u v0 n=%x o=%x ram@%x spad@%x sp=%x\n",
                           a0, a2, a3, v0_n, v0_o, ro, so, sp);
  } else if (++ng % 20 == 0) fprintf(stderr, "[spawnverify] %ld matches\n", ng);
}

// FUN_8007A980 — the per-type SPAWN DISPATCHER: the entry point game logic calls to spawn an object. It
// routes by CLASS (a0 & 0xff, 0..4) through table 0x80016E4C to one of 5 thin per-type handlers, each of
// which calls its spawn VARIANT as `variant(a0=0 ref, a1=type&0xff, a2=3 tail-insert, a3=list)` and returns
// the new node. RE'd from disas 0x8007A980 + the 5 handlers (0x8007a9b8.. each: a0=0; a1&=0xff; a2=3; jal
// variant; j epilogue). Owning this puts the engine in charge of object SPAWNING; the per-type variants
// (which do the alloc + per-class init) stay reachable by address via rec_dispatch (content). Out-of-range
// class returns 0 (the recomp's sltiu(cls,5)=0 lands in v0 and falls to the epilogue).
//
//   v0 = spawn(a0=class, a1=type, a2=list):
//     cls = a0 & 0xff;  if (cls >= 5) return 0;
//     variant = VAR[cls];                      // 0x80079c3c / 79ddc / 79f90 / 7a12c / 7a2c8
//     return variant(0, a1 & 0xff, 3, a2);     // ref=0, type, mode=3 (tail), list=a2
static const uint32_t SPAWN_VAR[5] = { 0x80079C3Cu, 0x80079DDCu, 0x80079F90u, 0x8007A12Cu, 0x8007A2C8u };
static void spawn_dispatch(Core* c) {
  uint32_t cls = c->r[4] & 0xffu;
  if (cls >= 5) { c->r[2] = 0; return; }
  uint32_t type = c->r[5] & 0xffu, list = c->r[6];
  c->r[4] = 0; c->r[5] = type; c->r[6] = 3; c->r[7] = list;   // handler sets ref=0, type&0xff, mode=3, a3=list
  rec_dispatch(c, SPAWN_VAR[cls]);                            // run the per-type spawn variant → v0 in r2
}
void rec_dispatch(Core*, uint32_t);
void ov_spawn_dispatch(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("spawndispverify") ? 1 : 0;
  if (!s_v) { spawn_dispatch(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t a0 = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  spawn_dispatch(c);
  uint32_t v0_n = c->r[2];
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, 0x8007A980u);
  uint32_t v0_o = c->r[2];
  // Same family rationale as spawnverify: the variant runs in BOTH passes; its + the dispatcher's stack
  // frames are dead below entry sp on return, and the native path doesn't decrement sp so they sit 24B apart.
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0 || v0_n != v0_o) {
    if (nb++ < 40) fprintf(stderr, "[spawndispverify] MISMATCH cls=%u v0 n=%x o=%x ram@%x spad@%x sp=%x\n",
                           a0 & 0xff, v0_n, v0_o, ro, so, sp);
  } else if (++ng % 20 == 0) fprintf(stderr, "[spawndispverify] %ld matches\n", ng);
}

// FUN_80079F90 / FUN_8007A12C / FUN_8007A2C8 — the remaining three pool spawn primitives (dispatcher
// classes 2/3/4). RE'd from disas (0x80079F90 / 0x8007A12C / 0x8007A2C8): each is byte-identical to the
// pool-2 primitive's pop+link+stamp EXCEPT (a) a different free-list head + count byte, and (b) when its
// pool is EMPTY it returns 0 (jr ra; v0=zero at the 0x8007a124/0x8007a2c0/0x8007a45c tails) — NO delegation
// (unlike pool-2, which delegates to 0x80079F90). The link/stamp (list head/tail by a3, insert by a2, stamp
// node+10/+0/+12) is the shared spawn_link_stamp. No pool-low guard (only 0x80079C3C has the cnt<3 guard).
struct PoolDesc { uint32_t free_head, cnt; };
static const PoolDesc POOL_VAR2 = { 0x800F2398u, 0x800ED8CCu };   // FUN_80079F90 (class 2)
static const PoolDesc POOL_VAR3 = { 0x800ED8D4u, 0x800ED8C5u };   // FUN_8007A12C (class 3)
static const PoolDesc POOL_VAR4 = { 0x800ED8D0u, 0x800ED8C4u };   // FUN_8007A2C8 (class 4)

static uint32_t pool_spawn(Core* c, const PoolDesc& p) {
  const uint32_t ref = c->r[4], type = c->r[5] & 0xffu, mode = c->r[6], list = c->r[7];
  uint32_t node = c->mem_r32(p.free_head);
  if (node == 0) return 0;                          // pool empty -> v0 = 0 (no delegation)
  uint32_t cnt = c->mem_r8(p.cnt);
  c->mem_w8(p.cnt, (uint8_t)(cnt - 1));
  c->mem_w32(p.free_head, c->mem_r32(node + 36));   // free head = node->next
  spawn_link_stamp(c, node, ref, type, mode, list);
  return node;
}

// Shared A/B gate for a pool spawn variant: run native, snapshot+rollback, super-call the recomp body,
// diff full main-RAM (excluding the dead stack window [sp-0x800, sp)) + scratchpad + v0. Same rationale as
// spawnverify/pool2verify: the only callee-touched persistent state is the pool/list nodes the diff covers.
static void spawn_variant_gate(Core* c, const PoolDesc& p, uint32_t super_addr, const char* gate, int on) {
  if (!on) { c->r[2] = pool_spawn(c, p); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t a3 = c->r[7];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  c->r[2] = pool_spawn(c, p);
  uint32_t v0_n = c->r[2];
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, super_addr);
  uint32_t v0_o = c->r[2];
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0 || v0_n != v0_o) {
    if (nb++ < 40) fprintf(stderr, "[%s] MISMATCH list=%u v0 n=%x o=%x ram@%x spad@%x sp=%x\n",
                           gate, a3, v0_n, v0_o, ro, so, sp);
  } else if (++ng % 20 == 0) fprintf(stderr, "[%s] %ld matches\n", gate, ng);
}

void ov_spawn_var2(Core* c) {   // FUN_80079F90 (class 2)
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("spawnvarverify") ? 1 : 0;
  spawn_variant_gate(c, POOL_VAR2, 0x80079F90u, "spawnvarverify", s_v);
}
void ov_spawn_var3(Core* c) {   // FUN_8007A12C (class 3)
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("spawnvarverify") ? 1 : 0;
  spawn_variant_gate(c, POOL_VAR3, 0x8007A12Cu, "spawnvarverify", s_v);
}
void ov_spawn_var4(Core* c) {   // FUN_8007A2C8 (class 4)
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("spawnvarverify") ? 1 : 0;
  spawn_variant_gate(c, POOL_VAR4, 0x8007A2C8u, "spawnvarverify", s_v);
}

void ov_spawn_pool2(Core* c) {   // FUN_80079DDC
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("pool2verify") ? 1 : 0;
  if (!s_v) { c->r[2] = spawn_pool2(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t a3 = c->r[7];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  c->r[2] = spawn_pool2(c);
  uint32_t v0_n = c->r[2];
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, 0x80079DDCu);
  uint32_t v0_o = c->r[2];
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0 || v0_n != v0_o) {
    if (nb++ < 40) fprintf(stderr, "[pool2verify] MISMATCH list=%u v0 n=%x o=%x ram@%x spad@%x sp=%x\n",
                           a3, v0_n, v0_o, ro, so, sp);
  } else if (++ng % 20 == 0) fprintf(stderr, "[pool2verify] %ld matches\n", ng);
}

// FUN_8007AA38 — the SPAWN-RELATIVE-TO-OBJECT dispatcher (a second spawn entry, table 0x80016E64). Unlike
// FUN_8007A980 (which forces ref=0, mode=3 tail-insert), this one spawns a new node RELATIVE to an existing
// object `obj`, passing the caller's mode (a2) and list (a3) straight through. RE'd from disas 0x8007AA38 +
// the 5 thin handlers 0x8007aa90..0x8007aad0 (each: a1 &= 0xff; jal SPAWN_VAR[class]; return its v0):
//
//   v0 = spawn_rel(a0=obj, a1=packed, a2=mode, a3=list):
//     if ((u8)obj[+0x0a] != a3) return 0;          // guard: obj must already be in the expected list
//     cls = (a1 >> 8) & 0x7f;  type = a1 & 0xff;
//     if (cls >= 5) return 0;
//     return SPAWN_VAR[cls](obj, type, a2, a3);     // ref=obj, type, mode=a2, list=a3
//
// The per-class spawn VARIANTS are all owned natively above; this just guards + routes (content-side type).
static void replace_dispatch(Core* c) {
  uint32_t obj = c->r[4], a1 = c->r[5], a2 = c->r[6], a3 = c->r[7];
  if (c->mem_r8(obj + 0x0au) != a3) { c->r[2] = 0; return; }   // obj's list id (+0x0a) must equal a3
  uint32_t cls = (a1 >> 8) & 0x7fu;
  if (cls >= 5) { c->r[2] = 0; return; }
  c->r[5] = a1 & 0xffu;                  // a1 = type (a0=obj, a2=mode, a3=list pass through)
  rec_dispatch(c, SPAWN_VAR[cls]);       // run the per-type spawn variant → v0 in r2
}
void ov_replace_dispatch(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("replacedispverify") ? 1 : 0;
  if (!s_v) { replace_dispatch(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t a1 = c->r[5];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  replace_dispatch(c);
  uint32_t v0_n = c->r[2];
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, 0x8007AA38u);
  uint32_t v0_o = c->r[2];
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0 || v0_n != v0_o) {
    if (nb++ < 40) fprintf(stderr, "[replacedispverify] MISMATCH a1=%x cls=%u v0 n=%x o=%x ram@%x spad@%x sp=%x\n",
                           a1, (a1 >> 8) & 0x7fu, v0_n, v0_o, ro, so, sp);
  } else if (++ng % 20 == 0) fprintf(stderr, "[replacedispverify] %ld matches\n", ng);
}

// ------------------------------------------------------------------------------------------------
// Public registration — ONE line from game_tomba2.cpp init.
// ------------------------------------------------------------------------------------------------
void entity_spawn_register(void) {
  rec_set_override(0x80079C3Cu, ov_entity_spawn);   // FUN_80079C3C entity spawn / placement primitive (pool 208)
  rec_set_override(0x80079DDCu, ov_spawn_pool2);    // FUN_80079DDC spawn variant — pool 2 (delegates to 0x80079F90)
  rec_set_override(0x80079F90u, ov_spawn_var2);     // FUN_80079F90 spawn variant — pool var2 (class 2, empty->0)
  rec_set_override(0x8007A12Cu, ov_spawn_var3);     // FUN_8007A12C spawn variant — pool var3 (class 3, empty->0)
  rec_set_override(0x8007A2C8u, ov_spawn_var4);     // FUN_8007A2C8 spawn variant — pool var4 (class 4, empty->0)
  rec_set_override(0x8007A980u, ov_spawn_dispatch); // FUN_8007A980 per-type spawn dispatcher (entry point)
  rec_set_override(0x8007AA38u, ov_replace_dispatch); // FUN_8007AA38 spawn-relative-to-object dispatcher
}

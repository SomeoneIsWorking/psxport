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

// --- spawntrace: log the CALLER (ra) of each spawn-entry, to find the field-load PLACEMENT driver
// top-down. Enable with REPL `debug spawntrace`. Prints ra + args on every spawn-entry invocation.
static void spawn_trace(Core* c, const char* who, uint32_t entry) {
  static int s_t = -1; if (s_t < 0) s_t = cfg_dbg("spawntrace") ? 1 : 0;
  if (!s_t) return;
  fprintf(stderr, "[spawntrace] %s@%08x ra=%08x a0=%08x a1=%08x a2=%08x a3=%08x\n",
          who, entry, c->r[31], c->r[4], c->r[5], c->r[6], c->r[7]);
}

void ov_entity_spawn(Core* c) {
  spawn_trace(c, "spawn79c3c", 0x80079C3Cu);
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
  spawn_trace(c, "disp7a980", 0x8007A980u);
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
// FUN_8003116C — SPAWN-AND-INIT helper: spawn a type-6 object on list 1 (via the owned dispatcher
// FUN_8007A980), seed its position from a1, stash a2, and run the object init FUN_80028E10. RE'd from disas
// 0x8003116C:
//   if ((u8)*0x800E7E7C < 7) return 0;                 // pool-0 free-count guard
//   node = FUN_8007A980(class=0, type=6, list=1);  if (!node) return 0;
//   if (a1) { node[+0x2c]=a1[+2]; node[+0x2e]=a1[+6]; node[+0x30]=a1[+0xa]; }
//   node[+0x32] = (u16)a2;  FUN_80028E10(node, a0);  return node;
// The owned spawn dispatcher does the alloc; the per-object init FUN_80028E10 stays content (rec_dispatch).
static uint32_t spawn_and_init(Core* c) {
  uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6];
  if (c->mem_r8(0x800E7E7Cu) < 7) return 0;
  c->r[4] = 0; c->r[5] = 6; c->r[6] = 1;
  rec_dispatch(c, 0x8007A980u);
  uint32_t node = c->r[2];
  if (node == 0) return 0;
  if (a1 != 0) {
    c->mem_w16(node + 0x2c, c->mem_r16(a1 + 2));
    c->mem_w16(node + 0x2e, c->mem_r16(a1 + 6));
    c->mem_w16(node + 0x30, c->mem_r16(a1 + 0xa));
  }
  c->mem_w16(node + 0x32, (uint16_t)a2);
  c->r[4] = node; c->r[5] = a0;
  rec_dispatch(c, 0x80028E10u);
  return node;
}

void ov_replace_dispatch(Core* c) {
  spawn_trace(c, "repl7aa38", 0x8007AA38u);
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

// FUN_8007A624 — the DESPAWN primitive (the inverse of spawn): unlink a node from its active list, clear
// its high state bit, return it to its pool's free-list, and deactivate it. RE'd from disas 0x8007A624 +
// the 5 free handlers 0x8007a718..0x8007a7a8 (each pushes the node to a pool free-list + bumps the count,
// then falls into the shared epilogue at 0x8007a7d0 that clears node[+0]/[+4]). Steps:
//   (1) UNLINK from the active doubly-linked list (standard removal w/ head/tail fixup). The head/tail vars
//       are picked list-0 vs ANY-nonzero (list!=0 uses list-1's head/tail) — verbatim recomp quirk; it only
//       matters when the freed node is AT a list end (interior removal uses the prev/next pointers only).
//   (2) node[+0x28] &= 0x7f  (clear the high bit)
//   (3) cls = node[+0x28] & 0xff; if (cls < 5): push node onto pool[cls] free-list (node->next = *head;
//       *head = node; cnt++); class 4 also calls the cleanup FUN_8007ADDC(node) (kept content via dispatch).
//   (4) EPILOGUE (all classes incl. cls>=5): node[+0] = 0; node[+4] = 0  (deactivate).
// The 5 pool descriptors are exactly the spawn-side free-lists (pool0/2 + the three variants).
static const PoolDesc DESPAWN_POOL[5] = {
  { 0x800E8098u, 0x800E7E7Cu },   // class 0 (FUN_80079C3C pool-208)
  { 0x800E80A0u, 0x800E7E7Du },   // class 1 (FUN_80079DDC pool-2)
  { 0x800F2398u, 0x800ED8CCu },   // class 2 (FUN_80079F90)
  { 0x800ED8D4u, 0x800ED8C5u },   // class 3 (FUN_8007A12C)
  { 0x800ED8D0u, 0x800ED8C4u },   // class 4 (FUN_8007A2C8) + cleanup 0x8007ADDC
};
static void despawn(Core* c) {
  uint32_t node = c->r[4];
  // (1) unlink. head/tail: list 0 -> list-0 vars, ANY nonzero list -> list-1 vars (verbatim).
  uint32_t list = c->mem_r8(node + 0x0au);
  uint32_t head = (list == 0) ? LIST_HEAD[0] : LIST_HEAD[1];
  uint32_t tail = (list == 0) ? LIST_TAIL[0] : LIST_TAIL[1];
  uint32_t prev = c->mem_r32(node + 32);
  uint32_t next = c->mem_r32(node + 36);
  if (prev != 0) c->mem_w32(prev + 36, next);            // prev->next = next
  else { c->mem_w32(head, next); if (next != 0) c->mem_w32(next + 32, 0); }   // *head = next; new head->prev = 0
  if (next != 0) c->mem_w32(next + 32, prev);            // next->prev = prev
  else { c->mem_w32(tail, prev); if (prev != 0) c->mem_w32(prev + 36, 0); }   // *tail = prev; new tail->next = 0
  // (2) clear high bit, (3) free-push by class
  uint32_t v = c->mem_r8(node + 0x28) & 0x7fu;
  c->mem_w8(node + 0x28, (uint8_t)v);
  uint32_t cls = v & 0xffu;
  if (cls < 5) {
    const PoolDesc& p = DESPAWN_POOL[cls];
    c->mem_w32(node + 36, c->mem_r32(p.free_head));      // node->next = *free_head
    c->mem_w32(p.free_head, node);                        // *free_head = node
    c->mem_w8(p.cnt, (uint8_t)(c->mem_r8(p.cnt) + 1));    // cnt++
    if (cls == 4) { c->r[4] = node; rec_dispatch(c, 0x8007ADDCu); }   // pool-4 extra cleanup (content)
  }
  // (4) epilogue (0x8007a7d0): deactivate — zero the node header words 0/4/8/c/10/14/18/38 (covers active
  // byte +0, state +4, list-id +0x0a and type +0x0c) + the bytes +0x29/+0x2a/+0x2b/+0x5e. The free-list link
  // (+0x24) is deliberately NOT cleared (it must survive in the free-list). Verbatim recomp.
  const uint32_t zw[] = { 0, 4, 8, 0xc, 0x10, 0x14, 0x18, 0x38 };
  for (uint32_t o : zw) c->mem_w32(node + o, 0);
  c->mem_w8(node + 0x2a, 0);
  c->mem_w8(node + 0x2b, 0);
  c->mem_w8(node + 0x29, 0);
  c->mem_w8(node + 0x5e, 0);
}
void ov_despawn(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("despawnverify") ? 1 : 0;
  if (!s_v) { despawn(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t a0 = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  despawn(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, 0x8007A624u);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[despawnverify] MISMATCH node=%08x list=%u ram@%x spad@%x sp=%x\n",
                           a0, c->mem_r8(a0 + 0x0au), ro, so, sp);
  } else if (++ng % 20 == 0) fprintf(stderr, "[despawnverify] %ld matches\n", ng);
}

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
// Shared A/B gate template for these two record-subsystem fns (native run, snapshot+rollback, super-call,
// diff full main-RAM excl. dead stack + scratchpad + v0).
static void record_gate(Core* c, uint32_t (*fn)(Core*), uint32_t super_addr, const char* gate, int on) {
  if (!on) { c->r[2] = fn(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t a0 = c->r[4];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  c->r[2] = fn(c);
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
    if (nb++ < 40) fprintf(stderr, "[%s] MISMATCH a0=%08x v0 n=%x o=%x ram@%x spad@%x sp=%x\n", gate, a0, v0_n, v0_o, ro, so, sp);
  } else if (++ng % 20 == 0) fprintf(stderr, "[%s] %ld matches\n", gate, ng);
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
void ov_spawn_and_init(Core* c) {   // FUN_8003116C (defined above, gated here after record_gate)
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("spawninitverify") ? 1 : 0;
  record_gate(c, spawn_and_init, 0x8003116Cu, "spawninitverify", s_v);
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
    rec_dispatch(c, 0x800517F8u);
    return c->r[2];
  }
  return last;
}
void ov_obj_pos_compose(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("poscomposeverify") ? 1 : 0;
  record_gate(c, obj_pos_compose, 0x8004BD64u, "poscomposeverify", s_v);
}

// ================================================================================================
// FUN_80072A78 — the field OBJECT-PLACEMENT DRIVER (top-down: the "object spawn handler").
//
// This is the function the GAME-stage area machine calls (from GAME.BIN @0x80106bf4 / 0x801072a8 /
// 0x801077f0 / 0x80108e14) when a field/area becomes active. It selects the area's PLACEMENT TABLE
// from (area id 0x800BF870, sub-state 0x800BF871), then walks the table's fixed 0x14-byte records,
// spawning one object per record via the owned per-type dispatch FUN_8007A980 and stamping the new
// node's identity / position / facing / behavior-handler from the record. It is THE entry point that
// populates a field with its NPCs/items/scenery. Resident MAIN.EXE leaf (no yield) → ownable by a
// plain override; the only call it makes is the spawn dispatch (owned). It WRITES guest object state
// the still-recomp content reads → content-INTERFACE: gated byte-exact (full RAM+scratchpad A/B).
//
// PLACEMENT RECORD (0x14 bytes, table terminated by a record whose byte[0]==0xff):
//   +0x00 u8   type   (a0 to spawn = type & 0x7f; full byte also stamped to node+0x28)
//   +0x01 u8   class  (a1 to spawn; if class==3 -> a2/list=1, else list=0)
//   +0x02 u16  X      -> node+0x2e        +0x04 u16  Y -> node+0x32     +0x06 u16  Z -> node+0x36
//   +0x08 u8          -> node+0x02        +0x09 u8     -> node+0x03
//   +0x0a s16  facingA(deg) -> node+0x56 (deg->PSX 0..0xfff)   +0x0c s16 facingB(deg) -> node+0x58
//   +0x0e s16  cond   (1 = gated by per-area collected-bitmask 0x800BFE56 bit[area];
//                      2 = gated by the global enable byte 0x800BF873)
//   +0x10 u32  handler fn ptr -> node+0x1c (the per-object update/render routine; content)
//
// TABLE SELECT (special-cased per area; default = PTR table 0x800A4C28[area]):
//   area5,sub1..3 -> 0x8013C1A4 ; area1,sub>=15 -> 0x80134918 ;
//   area6: sub<6 0x801437AC / sub<9 0x80143ACC / else 0x80143AE0 ;
//   area8: sub<9 0x8014304C / sub<16 0x801432B8 / sub<21 0x80143470 / else 0x80143614 ;
//   area0x15: sub 0..4 -> 0x80115004/18/F4/180/1F8 / else 0x80115310 ;
//   default: if (u16@0x800BF870 == 0x704) none; else 0x800A4C28[area] (0 -> none).
static uint32_t place_select_table(Core* c) {
  uint8_t area = c->mem_r8(0x800BF870u);
  uint8_t sub  = c->mem_r8(0x800BF871u);
  if (area == 5  && (uint8_t)(sub - 1) < 3) return 0x8013C1A4u;
  if (area == 1  && sub >= 15)              return 0x80134918u;
  if (area == 6) { if (sub < 6) return 0x801437ACu; if (sub < 9) return 0x80143ACCu; return 0x80143AE0u; }
  if (area == 8) { if (sub < 9) return 0x8014304Cu; if (sub < 16) return 0x801432B8u; if (sub < 21) return 0x80143470u; return 0x80143614u; }
  if (area == 0x15) {
    switch (sub) {
      case 0: return 0x80115004u; case 1: return 0x80115018u; case 2: return 0x801150F4u;
      case 3: return 0x80115180u; case 4: return 0x801151F8u; default: return 0x80115310u;
    }
  }
  if (c->mem_r16(0x800BF870u) == 0x704u) return 0;   // area4/sub7 -> no placement
  return c->mem_r32(0x800A4C28u + (uint32_t)area * 4);
}

// deg->PSX angle (0..0xfff): exact replica of the signed div-by-360 reciprocal at 0x80072d50.
static uint16_t place_deg2ang(int16_t deg) {
  int32_t  v1   = (int32_t)((uint32_t)(int32_t)deg << 12);
  int64_t  prod = (int64_t)v1 * (int64_t)(int32_t)0xB60B60B7u;   // MIPS signed mult
  int32_t  hi   = (int32_t)((uint64_t)prod >> 32);               // mfhi
  int32_t  v0   = (int32_t)((uint32_t)hi + (uint32_t)v1) >> 8;   // addu; sra 8
  uint32_t r    = (uint32_t)v0 - (uint32_t)(v1 >> 31);           // subu (sign correction)
  return (uint16_t)(r & 0xfffu);
}

static void place_objects(Core* c) {
  uint32_t rec = place_select_table(c);
  if (rec == 0 || c->mem_r8(rec) == 0xff) return;
  uint8_t area = c->mem_r8(0x800BF870u);
  do {
    int16_t cond = (int16_t)c->mem_r16(rec + 0x0e);
    bool skip = false;
    if (cond == 1) {
      uint32_t bits = c->mem_r16(0x800BFE56u);          // u16, zero-extended
      if ((bits >> (area & 0x1f)) & 1u) skip = true;     // already-collected gate
    } else if (cond == 2) {
      if (c->mem_r8(0x800BF873u) != 0) skip = true;      // global-enable gate
    }
    if (!skip) {
      uint8_t cls = c->mem_r8(rec + 1);
      c->r[4] = c->mem_r8(rec) & 0x7fu;                  // a0 = type & 0x7f
      c->r[5] = (cls == 3) ? 3u : cls;                   // a1 = class
      c->r[6] = (cls == 3) ? 1u : 0u;                    // a2 = list (3 -> 1)
      spawn_dispatch(c);
      uint32_t node = c->r[2];
      if (node != 0) {
        c->mem_w8 (node + 0x28, c->mem_r8 (rec));
        c->mem_w32(node + 0x1c, c->mem_r32(rec + 0x10));
        c->mem_w8 (node + 0x02, c->mem_r8 (rec + 8));
        c->mem_w8 (node + 0x03, c->mem_r8 (rec + 9));
        c->mem_w16(node + 0x2e, c->mem_r16(rec + 2));
        c->mem_w16(node + 0x32, c->mem_r16(rec + 4));
        c->mem_w16(node + 0x54, 0);
        c->mem_w16(node + 0x36, c->mem_r16(rec + 6));
        c->mem_w16(node + 0x56, place_deg2ang((int16_t)c->mem_r16(rec + 0x0a)));
        c->mem_w16(node + 0x58, place_deg2ang((int16_t)c->mem_r16(rec + 0x0c)));
      }
    }
    rec += 0x14;
  } while (c->mem_r8(rec) != 0xff);
}

void ov_place_objects(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("placeverify") ? 1 : 0;
  if (!s_v) { place_objects(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t area = c->mem_r8(0x800BF870u), sub = c->mem_r8(0x800BF871u);
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  place_objects(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, 0x80072A78u);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[placeverify] MISMATCH area=%u sub=%u ram@%x spad@%x sp=%x\n", area, sub, ro, so, sp);
  } else fprintf(stderr, "[placeverify] match #%ld (area=%u sub=%u)\n", ++ng, area, sub);
}

// FUN_80072DDC — single-object SPAWN-WITH-PARENT helper (the 2nd dominant field-spawn caller found by
// spawntrace). Spawns one object via the owned dispatch FUN_8007A980, then links it to a PARENT object and
// stamps a flag. Resident leaf, no yield. RE'd 1:1 from disas 0x80072ddc.
//   node* spawn_parent(a0=parent, a1=type, a2=class, a3=flag):
//     node = FUN_8007A980(type & 0x7f, (class==3)?3:class&0xff, (class==3)?1:0);
//     if (node) { node[0x28]=type(full byte); node[0x10]=parent; node[2]=flag; }
//     return node;
static uint32_t spawn_with_parent(Core* c) {
  uint32_t parent = c->r[4], type = c->r[5], cls = c->r[6] & 0xffu, flag = c->r[7];
  c->r[4] = type & 0x7fu;
  c->r[5] = (cls == 3) ? 3u : cls;
  c->r[6] = (cls == 3) ? 1u : 0u;
  spawn_dispatch(c);
  uint32_t node = c->r[2];
  if (node != 0) {
    c->mem_w8 (node + 0x28, type & 0xffu);   // full type byte (s0 = original a1, unmasked)
    c->mem_w32(node + 0x10, parent);
    c->mem_w8 (node + 0x02, flag & 0xffu);
  }
  return node;
}
void ov_spawn_with_parent(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("spawnparentverify") ? 1 : 0;
  if (!s_v) { c->r[2] = spawn_with_parent(c); return; }
  static uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);
  c->r[2] = spawn_with_parent(c);
  uint32_t v0_n = c->r[2];
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, 0x80072DDCu);
  uint32_t v0_o = c->r[2];
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0 || v0_n != v0_o) {
    if (nb++ < 40) fprintf(stderr, "[spawnparentverify] MISMATCH v0 n=%x o=%x ram@%x spad@%x\n", v0_n, v0_o, ro, so);
  } else if (++ng % 20 == 0) fprintf(stderr, "[spawnparentverify] %ld matches\n", ng);
}

// ------------------------------------------------------------------------------------------------
// Public registration — ONE line from game_tomba2.cpp init.
// ------------------------------------------------------------------------------------------------
void entity_spawn_register(void) {
}

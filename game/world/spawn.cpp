// game/world/spawn.cpp — PC-native ENTITY SPAWN subsystem (pool-pop + list-link + identity-stamp).
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
#include "spawn.h"
#include "world_pool.h"
#include "verify_gate.h"
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
// is EMPTY it DELEGATES to variant 2 (FUN_80079F90) — POOL_VAR2, owned natively via pool_spawn (byte-perfect
// per spawn_variant_native, proven by the spawn_dispatch A/B wire in af27fd8).
static uint32_t pool_spawn(Core* c, const PoolDesc& p);
static const uint32_t POOL2_HEAD = 0x800E80A0u, POOL2_CNT = 0x800E7E7Du;
static uint32_t spawn_pool2(Core* c) {
  const uint32_t ref = c->r[4], type = c->r[5] & 0xffu, mode = c->r[6], list = c->r[7];
  uint32_t node = c->mem_r32(POOL2_HEAD);
  if (node == 0) {                                  // pool empty -> delegate to POOL_VAR2 (FUN_80079F90) natively
    c->r[4] = ref; c->r[5] = type;                  // a2/a3 already hold mode/list, untouched
    static const PoolDesc P = { 0x800F2398u, 0x800ED8CCu };   // POOL_VAR2 (FUN_80079F90) — see defn below
    return pool_spawn(c, P);
  }
  uint32_t cnt = c->mem_r8(POOL2_CNT);
  c->mem_w8(POOL2_CNT, (uint8_t)(cnt - 1));
  c->mem_w32(POOL2_HEAD, c->mem_r32(node + 36));    // free head = node->next
  spawn_link_stamp(c, node, ref, type, mode, list);
  return node;
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
// Run the per-class spawn VARIANT NATIVELY (the 5 bodies are all owned in this TU) reading ref/type/mode/
// list from r4..r7 and returning the node ptr. Replaces the former rec_dispatch(SPAWN_VAR[cls]) into the
// PSX body — keeps the placement→spawn path fully native (PC calls PC). Defined after pool_spawn/POOL_VAR.
static uint32_t spawn_variant_native(Core* c, uint32_t cls);
uint32_t Spawn::dispatch(uint32_t cls_in, uint32_t type_in, uint32_t list) {
  Core* c = this->core;
  uint32_t cls = cls_in & 0xffu;
  if (cls >= 5) { c->r[2] = 0; return 0; }
  uint32_t type = type_in & 0xffu;
  c->r[4] = 0; c->r[5] = type; c->r[6] = 3; c->r[7] = list;   // handler sets ref=0, type&0xff, mode=3, a3=list
  uint32_t node = spawn_variant_native(c, cls);               // run the per-type spawn variant (native)
  c->r[2] = node;
  return node;
}
void rec_dispatch(Core*, uint32_t);

// FUN_80079F90 / FUN_8007A12C / FUN_8007A2C8 — the remaining three pool spawn primitives (dispatcher
// classes 2/3/4). RE'd from disas (0x80079F90 / 0x8007A12C / 0x8007A2C8): each is byte-identical to the
// pool-2 primitive's pop+link+stamp EXCEPT (a) a different free-list head + count byte, and (b) when its
// pool is EMPTY it returns 0 (jr ra; v0=zero at the 0x8007a124/0x8007a2c0/0x8007a45c tails) — NO delegation
// (unlike pool-2, which delegates to 0x80079F90). The link/stamp (list head/tail by a3, insert by a2, stamp
// node+10/+0/+12) is the shared spawn_link_stamp. No pool-low guard (only 0x80079C3C has the cnt<3 guard).
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

// Native per-class spawn-variant dispatch (forward-declared above spawn_dispatch). All 5 variant bodies are
// owned in this TU; they read ref/type/mode/list from r4..r7. cls is pre-validated (<5) by the callers.
static uint32_t spawn_variant_native(Core* c, uint32_t cls) {
  switch (cls) {
    case 0:  return entity_spawn(c);            // FUN_80079C3C (pool-208, with pool-low guard)
    case 1:  return spawn_pool2(c);             // FUN_80079DDC (pool-2, delegates to var2 when empty)
    case 2:  return pool_spawn(c, POOL_VAR2);   // FUN_80079F90
    case 3:  return pool_spawn(c, POOL_VAR3);   // FUN_8007A12C
    default: return pool_spawn(c, POOL_VAR4);   // FUN_8007A2C8 (cls==4)
  }
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
  uint32_t node = c->engine.spawn.dispatch(/*cls=*/0, /*type=*/6, /*list=*/1);   // FUN_8007A980 — native
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
void Spawn::despawn(uint32_t node) {
  Core* c = this->core;
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("despawnverify") ? 1 : 0;
  static uint8_t* ram0 = nullptr;
  static uint8_t* ramN = nullptr;
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32];
  if (s_v) {
    if (!ram0) { ram0 = (uint8_t*)malloc(0x200000); ramN = (uint8_t*)malloc(0x200000); }
    memcpy(regs0, c->r, sizeof regs0);
    memcpy(ram0, c->ram, 0x200000);
    memcpy(spad0, c->scratch, 0x400);
  }
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
    if (cls == 4) {
      // FUN_8007ADDC — pool-4 child-record cleanup, inlined (disas 0x8007ADDC..0x8007AE2C).
      // Class-4 nodes own N child GraphicsBind records at node[+0xC0..+0xC0+4*(N-1)]; on despawn
      // those records are pushed back onto the shared record freelist (ptr @0x800E7E74 grows down,
      // count @0x800ED098). The recomp loops i from N-1 to 0, so the LAST child is freed first —
      // matches the freelist's LIFO reuse. Tail zeroes node[+9] (the child count).
      uint8_t n = c->mem_r8(node + 9);
      while (n != 0) {
        uint16_t freeCnt = c->mem_r16(0x800ED098u);
        uint32_t freePtr = c->mem_r32(0x800E7E74u);
        uint32_t child   = c->mem_r32(node + 0xC0u + ((uint32_t)n - 1u) * 4u);
        c->mem_w16(0x800ED098u, (uint16_t)(freeCnt + 1));
        c->mem_w32(0x800E7E74u, freePtr - 4);
        c->mem_w32(freePtr - 4, child);
        n--;
      }
      c->mem_w8(node + 9, 0);
    }
  }
  // (4) epilogue (0x8007a7d0): deactivate — zero node header words 0/4/8/c/10/14/18/38 (active byte +0,
  // state +4, list-id +0x0a, type +0x0c) + bytes +0x29/+0x2a/+0x2b/+0x5e. The free-list link (+0x24) is
  // deliberately NOT cleared (it must survive in the free-list).
  const uint32_t zw[] = { 0, 4, 8, 0xc, 0x10, 0x14, 0x18, 0x38 };
  for (uint32_t o : zw) c->mem_w32(node + o, 0);
  c->mem_w8(node + 0x2a, 0);
  c->mem_w8(node + 0x2b, 0);
  c->mem_w8(node + 0x29, 0);
  c->mem_w8(node + 0x5e, 0);

  if (!s_v) return;
  // `despawnverify` A/B: snapshot the native result, roll back, super-call the recomp, diff.
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  c->r[4] = node;
  rec_super_call(c, 0x8007A624u);
  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[despawnverify] MISMATCH node=%08x list=%u ram@%x spad@%x sp=%x\n",
                           node, c->mem_r8(node + 0x0au), ro, so, sp);
  } else if (++ng % 20 == 0) fprintf(stderr, "[despawnverify] %ld matches\n", ng);
}

uint32_t Spawn::spawnAndInit(uint32_t a0, uint32_t posSrc, uint32_t a2) {   // FUN_8003116C
  Core* c = this->core;
  c->r[4] = a0; c->r[5] = posSrc; c->r[6] = a2;
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("spawninitverify") ? 1 : 0;
  c->engine.verifyGate.run(spawn_and_init, 0x8003116Cu, "spawninitverify", s_v);
  return c->r[2];
}

// FUN_8007E110 — SCENE-ENTITY SPAWN primitive. RE'd from disas 0x8007E110..0x8007E1B4.
// Shape: allocate a class-3 slot via FUN_8007A5A8, install per-frame handler 0x8007DDE0, initialise
// scene-data pointers from *(u32)0x800ECF60. FUN_8007A5A8 is the SPECIALISED ALLOCATOR — always list 1,
// always tail-insert, no ref/mode arg, no pool-low guard, class byte forced to 3. Inlined here (its
// only caller was FUN_8007E110, so no separate primitive is worth exposing).
//
// FUN_8007A5A8 body (disas 0x8007A5A8..0x8007A620):
//   node = *(u32)FREE_HEAD; if (node == 0) return 0;         # freelist empty
//   nextFree = *(u32)(node+36);
//   tail = *(u32)LIST_TAIL[1];
//   *(u32)(node+36) = 0;                                     # clear next
//   *(u8)FREE_CNT = *(u8)FREE_CNT - 1;
//   *(u32)FREE_HEAD = nextFree;
//   *(u32)(node+32) = tail;                                  # node.prev = tail (0 if list empty)
//   if (tail == 0) *(u32)LIST_HEAD[1] = node;
//   else            *(u32)(tail+36) = node;                  # tail.next = node
//   *(u32)LIST_TAIL[1] = node;
//   *(u8)(node+10) = 1;                                      # list id
//   *(u8)(node+0)  = 2;                                      # alive
//   *(u8)(node+12) = 3;                                      # class = 3 (constant arg)
//
// FUN_8007E110 body (post-alloc, disas 0x8007E144..0x8007E1A0):
//   *(u8)(node+0x47) = 2;
//   *(u32)(node+0x1C) = 0x8007DDE0;                          # behavior handler ptr (per-frame tick)
//   *(u8)(node+3) = subtype;
//   *(u8)(node+0x28) |= 0x80;                                # alive/active flag
//   base = *(u32)0x800ECF60;                                 # scene-entity data-table pointer
//   *(u32)(node+0x48) = base;
//   *(u32)(node+0x4C) = base + 0x10;
//   hCount = *(u16)base;
//   *(u16)(node+0x5C) = 0xFFFF;                              # sentinel (-1)
//   *(u16)(node+0x5E) = sceneId;
//   *(u32)(node+0x50) = base + 0x10 + hCount*4;              # record-end
//
// Return: node ptr on success, 0 on freelist exhaustion (caller stashes in Actor::sceneHandle).
// A/B'd via `sceneentityverify` (full main-RAM + scratchpad diff vs rec_super_call(0x8007E110u)).
static uint32_t scene_entity_native(Core* c) {
  const uint32_t sceneId = c->r[4] & 0xFFFFu;
  const uint32_t subtype = c->r[5] & 0xFFu;
  // ---- FUN_8007A5A8: alloc class-3 tail-insert into list 1 ------------------------------------
  uint32_t node = c->mem_r32(FREE_HEAD);
  if (node == 0) return 0;
  uint32_t nextFree = c->mem_r32(node + 36);
  uint32_t tail     = c->mem_r32(LIST_TAIL[1]);
  c->mem_w32(node + 36, 0);
  c->mem_w8(FREE_CNT, (uint8_t)(c->mem_r8(FREE_CNT) - 1));
  c->mem_w32(FREE_HEAD, nextFree);
  c->mem_w32(node + 32, tail);
  if (tail == 0) c->mem_w32(LIST_HEAD[1], node);
  else           c->mem_w32(tail + 36, node);
  c->mem_w32(LIST_TAIL[1], node);
  c->mem_w8(node + 10, 1);
  c->mem_w8(node + 0,  2);
  c->mem_w8(node + 12, 3);
  // ---- FUN_8007E110: scene-entity init on the fresh node --------------------------------------
  c->mem_w8 (node + 0x47, 2);
  c->mem_w32(node + 0x1C, 0x8007DDE0u);
  c->mem_w8 (node + 3,    (uint8_t)subtype);
  c->mem_w8 (node + 0x28, (uint8_t)(c->mem_r8(node + 0x28) | 0x80));
  uint32_t base   = c->mem_r32(0x800ECF60u);
  uint16_t hCount = c->mem_r16(base);
  c->mem_w32(node + 0x48, base);
  c->mem_w32(node + 0x4C, base + 0x10);
  c->mem_w16(node + 0x5C, (uint16_t)0xFFFFu);
  c->mem_w16(node + 0x5E, (uint16_t)sceneId);
  c->mem_w32(node + 0x50, base + 0x10 + (uint32_t)hCount * 4);
  return node;
}
uint32_t Spawn::sceneEntity(uint16_t sceneId, uint8_t subtype) {
  Core* c = this->core;
  c->r[4] = sceneId;
  c->r[5] = subtype;
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("sceneentityverify") ? 1 : 0;
  c->engine.verifyGate.run(scene_entity_native, 0x8007E110u, "sceneentityverify", s_v);
  return c->r[2];
}

// FUN_8004B3F4 — SCORE-GEM DROP wrapper. Every callsite passes one of the eight fixed AP-gem
// denominations (100/200/500/1000/5000/10000/20000/100000), so this is the drop primitive for
// the score gems Tomba collects. RE'd from disas 0x8004B3F4..0x8004B424 (13 insns) and cross-
// checked against the Ghidra decompile:
//     DAT_800bf874 = DAT_800bf874 + param_2;      // running AP total
//     FUN_80071b44(param_1, param_2, 0);          // spawn gem entity + play pickup SFX
//     return 1;
// The MIPS body puts the store `sw v0, 4(v1)` in the delay slot of the `jal FUN_80071B44` so
// the score total at 0x800BF874 is updated BEFORE the callee begins. Since we don't reorder
// with the substrate call (the callee still runs under interpretation and observes the same
// sequential timing), a straight `total += value; call substrate;` matches semantically.
// FUN_80071B44's own conditional double-bump is gated on its param_3, which is always 0 here —
// so the wrapper's single unconditional bump is the entire delta observed.
void Spawn::dropScoreGem(uint32_t sourceNode, int32_t value) {
  Core* c = this->core;
  c->mem_w32(0x800BF874u, c->mem_r32(0x800BF874u) + (uint32_t)value);
  c->r[4] = sourceNode; c->r[5] = (uint32_t)value; c->r[6] = 0;
  rec_dispatch(c, 0x80071B44u);
  c->r[2] = 1;   // recomp returns v0 = 1 (unread by every current callsite, but faithful)
}

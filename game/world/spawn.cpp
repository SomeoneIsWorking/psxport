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
#include "game_ctx.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "spawn.h"
#include "game.h"
#include "override_registry.h"   // overrides::install — the one native-override registry       // c->game->verify — the shared A/B verify scaffold
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
void Spawn::spawnLinkStamp(Core* c, uint32_t node, uint32_t ref, uint32_t type, uint32_t mode, uint32_t list) {
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

uint32_t Spawn::entitySpawnBody(Core* c) {
  const uint32_t ref  = c->r[4];          // a0
  const uint32_t type = c->r[5] & 0xffu;  // a1 (stored as u8 -> node[12])
  const uint32_t mode = c->r[6];          // a2 (insertion mode)
  const uint32_t list = c->r[7];          // a3 (list id -> node[10])

  uint32_t cnt = c->mem_r8(FREE_CNT);
  if (cnt < 3) return 0;                   // pool-low guard

  uint32_t node = c->mem_r32(FREE_HEAD);
  c->mem_w8(FREE_CNT, (uint8_t)(cnt - 1));
  c->mem_w32(FREE_HEAD, c->mem_r32(node + 36));   // free head = node->next

  spawnLinkStamp(c, node, ref, type, mode, list);
  return node;
}

// FUN_80079DDC — the POOL-2 spawn primitive (spawn variant class 1). Same link/stamp as FUN_80079C3C but
// pops a DIFFERENT free-list (head 0x800E80A0, count 0x800E7E7D), has NO pool-low guard, and when its pool
// is EMPTY it DELEGATES to variant 2 (FUN_80079F90) — POOL_VAR2, owned natively via poolSpawn (byte-perfect
// per spawnVariantNative, proven by the spawn_dispatch A/B wire in af27fd8).
static const uint32_t POOL2_HEAD = 0x800E80A0u, POOL2_CNT = 0x800E7E7Du;
uint32_t Spawn::spawnPool2Body(Core* c) {
  const uint32_t ref = c->r[4], type = c->r[5] & 0xffu, mode = c->r[6], list = c->r[7];
  uint32_t node = c->mem_r32(POOL2_HEAD);
  if (node == 0) {                                  // pool empty -> delegate to POOL_VAR2 (FUN_80079F90) natively
    c->r[4] = ref; c->r[5] = type;                  // a2/a3 already hold mode/list, untouched
    static const PoolDesc P = { 0x800F2398u, 0x800ED8CCu };   // POOL_VAR2 (FUN_80079F90) — see defn below
    return poolSpawn(c, P);
  }
  uint32_t cnt = c->mem_r8(POOL2_CNT);
  c->mem_w8(POOL2_CNT, (uint8_t)(cnt - 1));
  c->mem_w32(POOL2_HEAD, c->mem_r32(node + 36));    // free head = node->next
  spawnLinkStamp(c, node, ref, type, mode, list);
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
// PSX body — keeps the placement→spawn path fully native (PC calls PC). Defined after poolSpawn/POOL_VAR.
uint32_t Spawn::dispatch(uint32_t cls_in, uint32_t type_in, uint32_t list) {
  Core* c = this->core;
  uint32_t cls = cls_in & 0xffu;
  if (cls >= 5) { c->r[2] = 0; return 0; }
  uint32_t type = type_in & 0xffu;
  c->r[4] = 0; c->r[5] = type; c->r[6] = 3; c->r[7] = list;   // handler sets ref=0, type&0xff, mode=3, a3=list
  uint32_t node = spawnVariantNative(c, cls);               // run the per-type spawn variant (native)
  c->r[2] = node;
  return node;
}
void rec_dispatch(Core*, uint32_t);

// FUN_80079F90 / FUN_8007A12C / FUN_8007A2C8 — the remaining three pool spawn primitives (dispatcher
// classes 2/3/4). RE'd from disas (0x80079F90 / 0x8007A12C / 0x8007A2C8): each is byte-identical to the
// pool-2 primitive's pop+link+stamp EXCEPT (a) a different free-list head + count byte, and (b) when its
// pool is EMPTY it returns 0 (jr ra; v0=zero at the 0x8007a124/0x8007a2c0/0x8007a45c tails) — NO delegation
// (unlike pool-2, which delegates to 0x80079F90). The link/stamp (list head/tail by a3, insert by a2, stamp
// node+10/+0/+12) is the shared spawnLinkStamp. No pool-low guard (only 0x80079C3C has the cnt<3 guard).
static const PoolDesc POOL_VAR2 = { 0x800F2398u, 0x800ED8CCu };   // FUN_80079F90 (class 2)
static const PoolDesc POOL_VAR3 = { 0x800ED8D4u, 0x800ED8C5u };   // FUN_8007A12C (class 3)
static const PoolDesc POOL_VAR4 = { 0x800ED8D0u, 0x800ED8C4u };   // FUN_8007A2C8 (class 4)

uint32_t Spawn::poolSpawn(Core* c, const PoolDesc& p) {
  const uint32_t ref = c->r[4], type = c->r[5] & 0xffu, mode = c->r[6], list = c->r[7];
  uint32_t node = c->mem_r32(p.free_head);
  if (node == 0) return 0;                          // pool empty -> v0 = 0 (no delegation)
  uint32_t cnt = c->mem_r8(p.cnt);
  c->mem_w8(p.cnt, (uint8_t)(cnt - 1));
  c->mem_w32(p.free_head, c->mem_r32(node + 36));   // free head = node->next
  spawnLinkStamp(c, node, ref, type, mode, list);
  return node;
}

// Native per-class spawn-variant dispatch (forward-declared above spawn_dispatch). All 5 variant bodies are
// owned in this TU; they read ref/type/mode/list from r4..r7. cls is pre-validated (<5) by the callers.
uint32_t Spawn::spawnVariantNative(Core* c, uint32_t cls) {
  switch (cls) {
    case 0:  return entitySpawnBody(c);            // FUN_80079C3C (pool-208, with pool-low guard)
    case 1:  return spawnPool2Body(c);             // FUN_80079DDC (pool-2, delegates to var2 when empty)
    case 2:  return poolSpawn(c, POOL_VAR2);   // FUN_80079F90
    case 3:  return poolSpawn(c, POOL_VAR3);   // FUN_8007A12C
    default: return poolSpawn(c, POOL_VAR4);   // FUN_8007A2C8 (cls==4)
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
uint32_t Spawn::spawnAndInitBody(Core* c) {
  uint32_t a0 = c->r[4], a1 = c->r[5], a2 = c->r[6];
  if (c->mem_r8(0x800E7E7Cu) < 7) return 0;
  uint32_t node = eng(c).spawn.dispatch(/*cls=*/0, /*type=*/6, /*list=*/1);   // FUN_8007A980 — native
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
  int s_v = c->game->verify.on("despawnverify");
  uint8_t* ram0 = nullptr;
  uint8_t* ramN = nullptr;
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32];
  if (s_v) {
    ram0 = c->game->verify.ram0(); ramN = c->game->verify.ramN();
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
  VerifyHarness::Check& chk = c->game->verify.check("despawnverify");
  long &ng = chk.nMatch, &nb = chk.nMismatch;
  if (ro >= 0 || so >= 0) {
    if (nb++ < 40) fprintf(stderr, "[despawnverify] MISMATCH node=%08x list=%u ram@%x spad@%x sp=%x\n",
                           node, c->mem_r8(node + 0x0au), ro, so, sp);
  } else if (++ng % 20 == 0) fprintf(stderr, "[despawnverify] %ld matches\n", ng);
}

uint32_t Spawn::spawnAndInit(uint32_t a0, uint32_t posSrc, uint32_t a2) {   // FUN_8003116C
  Core* c = this->core;
  c->r[4] = a0; c->r[5] = posSrc; c->r[6] = a2;
  c->game->verify.run(&Spawn::spawnAndInitBody, 0x8003116Cu, "spawninitverify",
                      c->game->verify.on("spawninitverify"));
  return c->r[2];
}

// FUN_801360F4 / FUN_80139838 / FUN_8013AC34 / FUN_8013A730 — TYPED-CHILD SPAWN wrappers (A00 overlay,
// 0x80135000-0x8013AFFF band). Called from per-object overlay behaviors — beh_box_seed_phase_gate
// (FUN_8012A0B8) STATE 0 passes both `801360F4(node,node[3])` + `80139838(node,0)`/`(node,1)` when
// node[3]<2, else `8013AC34(node,node[3])`; beh_single_child_cull (FUN_80132400) STATE 0 passes
// `8013A730(node)` (no sub-index arg) — to allocate a companion object and wire it up. RE'd from disas:
// each is IDENTICAL in shape — dispatch via the already-owned Spawn::dispatch(cls, type=4, list=0),
// then on success stamp 3 (or 4) fields on the fresh child node:
//   [+0x1C] = a fixed per-object AI HANDLER address (one of the already-native beh_* leaves below)
//   [+0x10] = the CALLING node (owner back-pointer)
//   [+2]    = a fixed content-type byte
//   [+3]    = the caller's sub-index byte (three of the four leaves only — FUN_8013A730 has no 2nd
//             guest arg and never touches [+3])
// Pool-empty (dispatch()==0) propagates unchanged (recomp: `beq v0,zero,skip-the-stores; ... ; jr ra`).
//   FUN_801360F4(node,sub): dispatch(cls=2,4,0); child[0x1C]=0x80135D64 (Spawn::despawn's sibling
//                           beh_quad_record_table_seed); child[2]=7;  child[3]=sub.
//   FUN_80139838(node,sub): dispatch(cls=1,4,0); child[0x1C]=0x801395C0 (beh_sibling_angle_track);
//                           child[2]=13; child[3]=sub.
//   FUN_8013AC34(node,sub): dispatch(cls=2,4,0); child[0x1C]=0x8013A900 (beh_child_trig_motion);
//                           child[2]=17; child[3]=sub.
//   FUN_8013A730(node):     dispatch(cls=3,4,0); child[0x1C]=0x8013A330 (beh_lift_platform);
//                           child[2]=16.  (single-arg variant — no [+3] write.)
uint32_t Spawn::spawnTypedChild(uint32_t owner, uint32_t cls, uint32_t handlerAddr, uint8_t typeByte,
                                 bool hasSub, uint32_t sub) {
  Core* c = this->core;
  uint32_t child = dispatch(cls, /*type=*/4, /*list=*/0);
  if (child == 0) return 0;
  c->mem_w32(child + 0x1c, handlerAddr);
  c->mem_w32(child + 0x10, owner);
  c->mem_w8 (child + 2, typeByte);
  if (hasSub) c->mem_w8(child + 3, (uint8_t)sub);
  return child;
}
uint32_t Spawn::spawnQuadRecordChild(uint32_t owner, uint32_t sub) {   // FUN_801360F4
  return spawnTypedChild(owner, /*cls=*/2, 0x80135D64u, /*type=*/7,  /*hasSub=*/true, sub);
}
uint32_t Spawn::spawnSiblingAngleChild(uint32_t owner, uint32_t sub) {   // FUN_80139838
  return spawnTypedChild(owner, /*cls=*/1, 0x801395C0u, /*type=*/13, /*hasSub=*/true, sub);
}
uint32_t Spawn::spawnChildTrigChild(uint32_t owner, uint32_t sub) {   // FUN_8013AC34
  return spawnTypedChild(owner, /*cls=*/2, 0x8013A900u, /*type=*/17, /*hasSub=*/true, sub);
}
uint32_t Spawn::spawnLiftPlatformChild(uint32_t owner) {   // FUN_8013A730
  return spawnTypedChild(owner, /*cls=*/3, 0x8013A330u, /*type=*/16, /*hasSub=*/false, 0);
}

// FUN_80031558 — Spawn::spawnEffectChild. One of the near-identical MAIN.EXE "spawn a child effect
// object" leaves (siblings 0x800310F4/8003116C/800312D4/800313A0/80031470, engine_re.md band 2):
// allocate an effect node via the per-type dispatcher FUN_8007A980 with (cls=0, type=6, list=1), then
// on success stamp the fresh node — per-frame handler 0x80029B40 at [+0x1C], list/state byte 32 at
// [+0x0B], owner back-pointer (arg0) at [+0x10], effect data-table ptr 0x80029F6C at [+0x18], caller
// sub-index (arg1, low byte) at [+3], and OR 0x80 into the flag byte at [+0x28]. Pool-empty
// (dispatch()==0) returns 0. The dispatcher FUN_8007A980 and the stamped handler/data addresses stay
// substrate (content routing). READY-FRAME leaf: the gen body descends sp by 32 and spills the callee-
// saved regs ra/s1/s0 (r31/r17/r16) at sp+24/+20/+16 with their LIVE incoming values, restoring them
// before return — the native port mirrors that guest stack frame exactly (docs/faithful-execution.md).
// ORACLE: gen_func_80031558
extern void func_8007A980(Core*);   // generated/shard_disp.c — per-type spawn dispatcher (FUN_8007A980)
uint32_t Spawn::spawnEffectChild(uint32_t owner, uint32_t sub) {
  Core* c = this->core;
  c->r[4] = owner;
  c->r[5] = sub;
  c->r[29] = c->r[29] + (uint32_t)-32;               // addiu sp,-0x20 — descend the guest frame
  c->mem_w32((c->r[29] + (uint32_t)20), c->r[17]);   // sw s1,0x14(sp) — LIVE incoming s1
  c->r[17] = c->r[4] + c->r[0];                       // s1 = owner
  c->mem_w32((c->r[29] + (uint32_t)16), c->r[16]);   // sw s0,0x10(sp) — LIVE incoming s0
  c->r[16] = c->r[5] + c->r[0];                       // s0 = sub
  c->r[4] = c->r[0] + c->r[0];                        // dispatch arg cls = 0
  c->r[5] = c->r[0] + (uint32_t)6;                    // dispatch arg type = 6
  c->mem_w32((c->r[29] + (uint32_t)24), c->r[31]);   // sw ra,0x18(sp)
  c->r[31] = 0x80031580u;
  c->r[6] = c->r[0] + (uint32_t)1; func_8007A980(c);  // dispatch arg list = 1 — FUN_8007A980
  { int poolEmpty = (c->r[2] == c->r[0]); c->r[3] = (uint32_t)32771u << 16; if (poolEmpty) goto L_poolEmpty; }
  c->r[3] = c->r[3] + (uint32_t)-25792;               // r3 = 0x80029B40 (per-frame effect handler)
  c->mem_w32((c->r[2] + (uint32_t)28), c->r[3]);      // child[+0x1C] = handler
  c->r[3] = c->r[0] + (uint32_t)32;
  c->mem_w8((c->r[2] + (uint32_t)11), (uint8_t)c->r[3]);  // child[+0x0B] = 32
  c->r[3] = (uint32_t)32771u << 16;
  c->r[4] = (uint32_t)c->mem_r8((c->r[2] + (uint32_t)40));  // r4 = child[+0x28] flag byte
  c->r[3] = c->r[3] + (uint32_t)-24724;               // r3 = 0x80029F6C (effect data table)
  c->mem_w32((c->r[2] + (uint32_t)16), c->r[17]);     // child[+0x10] = owner
  c->mem_w32((c->r[2] + (uint32_t)24), c->r[3]);      // child[+0x18] = data table ptr
  c->mem_w8((c->r[2] + (uint32_t)3), (uint8_t)c->r[16]);  // child[+3] = sub (low byte)
  c->r[4] = c->r[4] | 128u;
  c->mem_w8((c->r[2] + (uint32_t)40), (uint8_t)c->r[4]);  // child[+0x28] |= 0x80
  goto L_epilogue;
L_poolEmpty:;
  c->r[2] = c->r[0] + c->r[0];                        // return 0 (pool empty)
L_epilogue:;
  c->r[31] = c->mem_r32((c->r[29] + (uint32_t)24));   // lw ra,0x18(sp)
  c->r[17] = c->mem_r32((c->r[29] + (uint32_t)20));   // lw s1,0x14(sp)
  c->r[16] = c->mem_r32((c->r[29] + (uint32_t)16));   // lw s0,0x10(sp)
  c->r[29] = c->r[29] + (uint32_t)32;                 // addiu sp,0x20 — ascend the guest frame
  return c->r[2];
}

// Guest-ABI trampolines (registered via overrides::install): substrate/native rec_dispatch callers
// reach the 4 native bodies above exactly like the recomp bodies — args in r4/r5, return in r2.
//
// REGISTER-FAITHFULNESS (2026-07-11, the f389 diverge root cause): the native C++ spawnTypedChild
// above takes a SHORTCUT — it calls the native Spawn::dispatch which remaps args and calls native
// spawn bodies, bypassing the substrate's gen_func_8007A980 table dispatch. That leaves different
// r31/r3 values and different callee stack-frame residuals than the substrate. MIRROR_VERIFY
// compares the full RAM+regs, so the trampoline MUST reproduce the substrate's EXACT dispatch path:
// set up the same registers (r4=cls, r5=4, r6=0, r16=owner, r31=jal-site), call
// rec_dispatch(0x8007A980), then do the child-field writes. Each variant's jal-site ra and post-
// dispatch writes come from the substrate gen_ body (generated/ov_a00_shard_0.c).
static constexpr uint32_t SPAWN_DISPATCH = 0x8007A980u;   // gen_func_8007A980 — the table dispatch
static constexpr uint32_t HANDLER_LIFT    = 0x8013A330u;  // child+0x1C handler (beh_lift_platform)
static constexpr uint32_t HANDLER_QUADREC = 0x80135D64u;
static constexpr uint32_t HANDLER_SIBANG  = 0x801395C0u;
static constexpr uint32_t HANDLER_CHILDTRIG = 0x8013A900u;
// 0x8013A730 — spawnLiftPlatformChild: frame=24, spills r16@16, r31@20; dispatch(cls=3); single-arg.
static void eov_spawnLiftPlatformChild(Core* c) {
  const uint32_t owner = c->r[4];
  c->r[29] -= 24;
  c->mem_w32(c->r[29] + 16, c->r[16]);
  c->r[16] = owner;
  c->r[4] = 3; c->r[5] = 4; c->r[6] = 0;
  c->mem_w32(c->r[29] + 20, c->r[31]);
  c->r[31] = 0x8013A750u;
  rec_dispatch(c, SPAWN_DISPATCH);
  c->r[3] = (uint32_t)32788u << 16;   // r3 = 0x80140000 (set before null-check, per substrate)
  if (c->r[2] != 0) {
    c->mem_w32(c->r[2] + 0x1Cu, HANDLER_LIFT);
    c->r[3] = 16;
    c->mem_w32(c->r[2] + 0x10u, c->r[16]);
    c->mem_w8 (c->r[2] + 2u, (uint8_t)c->r[3]);
  } else {
    c->r[2] = 0;   // explicit return 0 (matches substrate L_8013A770)
  }
  c->r[31] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] += 24;
}
// 0x801360F4 — spawnQuadRecordChild: frame=32, spills r16@16, r17@20, r31@24; dispatch(cls=2).
// Substrate swaps: r17=owner(r4), r16=sub(r5). child+16=r17(owner), child+3=r16(sub).
static void eov_spawnQuadRecordChild(Core* c) {
  const uint32_t owner = c->r[4], sub = c->r[5];
  c->r[29] -= 32;
  c->mem_w32(c->r[29] + 20, c->r[17]);
  c->r[17] = owner;
  c->mem_w32(c->r[29] + 16, c->r[16]);
  c->r[16] = sub;
  c->r[4] = 2; c->r[5] = 4; c->r[6] = 0;
  c->mem_w32(c->r[29] + 24, c->r[31]);
  c->r[31] = 0x8013611Cu;
  rec_dispatch(c, SPAWN_DISPATCH);
  c->r[3] = (uint32_t)32787u << 16;   // 32787 (not 32788) per substrate
  if (c->r[2] != 0) {
    c->mem_w32(c->r[2] + 0x1Cu, HANDLER_QUADREC);
    c->r[3] = 7;
    c->mem_w32(c->r[2] + 0x10u, c->r[17]);
    c->mem_w8 (c->r[2] + 2u, (uint8_t)c->r[3]);
    c->mem_w8 (c->r[2] + 3u, (uint8_t)c->r[16]);
  } else {
    c->r[2] = 0;
  }
  c->r[31] = c->mem_r32(c->r[29] + 24);
  c->r[17] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] += 32;
}
// 0x80139838 — spawnSiblingAngleChild: frame=32, spills r16@16, r17@20, r31@24; dispatch(cls=1).
static void eov_spawnSiblingAngleChild(Core* c) {
  const uint32_t owner = c->r[4], sub = c->r[5];
  c->r[29] -= 32;
  c->mem_w32(c->r[29] + 20, c->r[17]);
  c->r[17] = owner;
  c->mem_w32(c->r[29] + 16, c->r[16]);
  c->r[16] = sub;
  c->r[4] = 1; c->r[5] = 4; c->r[6] = 0;
  c->mem_w32(c->r[29] + 24, c->r[31]);
  c->r[31] = 0x80139860u;
  rec_dispatch(c, SPAWN_DISPATCH);
  c->r[3] = (uint32_t)32788u << 16;
  if (c->r[2] != 0) {
    c->mem_w32(c->r[2] + 0x1Cu, HANDLER_SIBANG);
    c->r[3] = 13;
    c->mem_w32(c->r[2] + 0x10u, c->r[17]);
    c->mem_w8 (c->r[2] + 2u, (uint8_t)c->r[3]);
    c->mem_w8 (c->r[2] + 3u, (uint8_t)c->r[16]);
  } else {
    c->r[2] = 0;
  }
  c->r[31] = c->mem_r32(c->r[29] + 24);
  c->r[17] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] += 32;
}
// 0x8013AC34 — spawnChildTrigChild: frame=32, spills r16@16, r17@20, r31@24; dispatch(cls=2).
static void eov_spawnChildTrigChild(Core* c) {
  const uint32_t owner = c->r[4], sub = c->r[5];
  c->r[29] -= 32;
  c->mem_w32(c->r[29] + 20, c->r[17]);
  c->r[17] = owner;
  c->mem_w32(c->r[29] + 16, c->r[16]);
  c->r[16] = sub;
  c->r[4] = 2; c->r[5] = 4; c->r[6] = 0;
  c->mem_w32(c->r[29] + 24, c->r[31]);
  c->r[31] = 0x8013AC5Cu;
  rec_dispatch(c, SPAWN_DISPATCH);
  c->r[3] = (uint32_t)32788u << 16;
  if (c->r[2] != 0) {
    c->mem_w32(c->r[2] + 0x1Cu, HANDLER_CHILDTRIG);
    c->r[3] = 17;
    c->mem_w32(c->r[2] + 0x10u, c->r[17]);
    c->mem_w8 (c->r[2] + 2u, (uint8_t)c->r[3]);
    c->mem_w8 (c->r[2] + 3u, (uint8_t)c->r[16]);
  } else {
    c->r[2] = 0;
  }
  c->r[31] = c->mem_r32(c->r[29] + 24);
  c->r[17] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] += 32;
}
extern void ov_a00_gen_801360F4(Core*);
extern void ov_a00_gen_80139838(Core*);
extern void ov_a00_gen_8013AC34(Core*);
extern void ov_a00_gen_8013A730(Core*);

// FUN_80031558 — guest-ABI adapter (args in c->r[4]/c->r[5], return in c->r[2]).
extern void gen_func_80031558(Core*);
extern void shard_set_override(uint32_t, void (*)(Core*));
static void eov_spawnEffectChild(Core* c) { c->r[2] = eng(c).spawn.spawnEffectChild(c->r[4], c->r[5]); }

void Spawn::registerTypedChildOverrides() {
  using overrides::install;
  install(0x801360F4u, "Spawn::spawnQuadRecordChild",   eov_spawnQuadRecordChild,   ov_a00_gen_801360F4);
  install(0x80139838u, "Spawn::spawnSiblingAngleChild", eov_spawnSiblingAngleChild, ov_a00_gen_80139838);
  install(0x8013AC34u, "Spawn::spawnChildTrigChild",    eov_spawnChildTrigChild,    ov_a00_gen_8013AC34);
  install(0x8013A730u, "Spawn::spawnLiftPlatformChild", eov_spawnLiftPlatformChild, ov_a00_gen_8013A730);
  install(0x80031558u, "Spawn::spawnEffectChild",       eov_spawnEffectChild,       gen_func_80031558, shard_set_override);
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
uint32_t Spawn::sceneEntityBody(Core* c) {
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
  c->game->verify.run(&Spawn::sceneEntityBody, 0x8007E110u, "sceneentityverify",
                      c->game->verify.on("sceneentityverify"));
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

// FUN_8007E038 — VARIANT-OVERLAY SPAWN primitive. RE'd from disas 0x8007E038..0x8007E10C.
// Same allocator shape as sceneEntityBody (FUN_8007A5A8: class-3 tail-insert into list 1 —
// see the comment above sceneEntityBody for the primitive's full derivation), gated by:
//   guard = (DAT_800BF81E == 2) || (variant != 0) || (DAT_800BF822 == 0);   // else return 0
// then, post-alloc, installs the "variant overlay" per-frame handler instead of the scene-entity
// one and stamps node[3] with `variant` instead of a plain subtype byte:
//   node[0x47] = 1;                                          # (sceneEntity uses 2 here)
//   node[0x1C] = 0x8007DC38;                                 # beh_variant_overlay_lifecycle
//   node[3]    = (uint8_t)variant;
//   node[0x28] |= 0x80;
//   base = *(u32)0x800ECF60;                                 # SAME table sceneEntity reads
//   node[0x48] = base; node[0x4C] = base+0x10;
//   node[0x5C] = 0xFFFF; node[0x5E] = recordIndex;
//   node[0x50] = base + 0x10 + (*(u16)base)*4;
// Return: node ptr on success, 0 on guard-miss or freelist exhaustion.
// A/B'd via `spawnoverlayverify` (full main-RAM + scratchpad diff vs rec_super_call(0x8007E038u)).
uint32_t Spawn::spawnOverlayVariantBody(Core* c) {
  const uint16_t recordIndex = (uint16_t)(c->r[4] & 0xFFFFu);
  const int16_t  variant     = (int16_t)(c->r[5] & 0xFFFFu);   // guard needs the full 16-bit value
  if (!(c->mem_r8(0x800BF81Eu) == 2 || variant != 0 || c->mem_r8(0x800BF822u) == 0)) return 0;
  // ---- FUN_8007A5A8: alloc class-3 tail-insert into list 1 (identical to sceneEntityBody) ------
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
  // ---- FUN_8007E038: variant-overlay init on the fresh node -------------------------------------
  c->mem_w8 (node + 0x47, 1);
  c->mem_w8 (node + 3,    (uint8_t)variant);
  c->mem_w32(node + 0x1C, 0x8007DC38u);
  c->mem_w8 (node + 0x28, (uint8_t)(c->mem_r8(node + 0x28) | 0x80));
  uint32_t base   = c->mem_r32(0x800ECF60u);
  uint16_t hCount = c->mem_r16(base);
  c->mem_w32(node + 0x48, base);
  c->mem_w32(node + 0x4C, base + 0x10);
  c->mem_w16(node + 0x5C, (uint16_t)0xFFFFu);
  c->mem_w16(node + 0x5E, recordIndex);
  c->mem_w32(node + 0x50, base + 0x10 + (uint32_t)hCount * 4);
  return node;
}
uint32_t Spawn::spawnOverlayVariant(uint16_t recordIndex, int16_t variant) {
  Core* c = this->core;
  c->r[4] = recordIndex;
  c->r[5] = (uint32_t)(int32_t)variant;
  c->game->verify.run(&Spawn::spawnOverlayVariantBody, 0x8007E038u, "spawnoverlayverify",
                      c->game->verify.on("spawnoverlayverify"));
  return c->r[2];
}

// FUN_800735F4 — per-object controller that owns exactly ONE linked "variant overlay" child
// (spawned via spawnOverlayVariant) at obj[0x14], driven by a state byte obj[7] and a countdown
// obj[0x40]. RE'd from disas 0x800735F4..0x8007374C:
//   state 0: if (0x800BF816==0 && obj[0x29]!=0): node = spawnOverlayVariant(recordId, 2);
//            obj[0x14]=node; if (node!=0) { obj[0x40]=0x46; obj[7]++; }
//   state 1: if (0x800BF816!=0 && 0x800BF80F==0) — pause/freeze gate — kill the child (obj[0x14]
//            state->2 if still <2, clear the ptr) and obj[7]=0. Else: obj[0x40]-- ; once it rolls
//            from 0 to -1, kill the child the same way and obj[7]++.
//   state 2: if obj[0x29]==0, obj[7]=0; else no-op (holds at state 2 until the gate clears).
//   state >2: no-op.
// No other substrate calls in this body. "kill" writes the CHILD's own node[4]=2 (its OWN 4-state
// lifecycle then advances itself 2->3->despawn next ticks — see beh_variant_overlay_lifecycle.cpp),
// deliberately dereferenced unconditionally exactly as the recomp does (no defensive null-check:
// the state invariant guarantees obj[0x14] is non-null whenever state==1 is reached).
void Spawn::tickLinkedOverlay(uint32_t obj, int16_t recordId) {
  Core* c = this->core;
  uint8_t st = c->mem_r8(obj + 7);
  if (st == 1) {
    if (c->mem_r8(0x800BF816u) != 0 && c->mem_r8(0x800BF80Fu) == 0) {
      uint32_t child = c->mem_r32(obj + 0x14);
      if (c->mem_r8(child + 4) < 2) {
        c->mem_w8(child + 4, 2);
        c->mem_w32(obj + 0x14, 0);
      }
      c->mem_w8(obj + 7, 0);
      return;
    }
    int16_t countdown = (int16_t)(c->mem_r16(obj + 0x40) - 1);
    c->mem_w16(obj + 0x40, (uint16_t)countdown);
    if (countdown != -1) return;
    uint32_t child = c->mem_r32(obj + 0x14);
    if (c->mem_r8(child + 4) < 2) {
      c->mem_w8(child + 4, 2);
      c->mem_w32(obj + 0x14, 0);
    }
    c->mem_w8(obj + 7, (uint8_t)(c->mem_r8(obj + 7) + 1));
    return;
  }
  if (st > 1) {
    if (st != 2) return;
    if (c->mem_r8(obj + 0x29) != 0) return;
    c->mem_w8(obj + 7, 0);
    return;
  }
  // st == 0
  if (c->mem_r8(0x800BF816u) != 0) return;
  if (c->mem_r8(obj + 0x29) == 0) return;
  uint32_t node = spawnOverlayVariant((uint16_t)(uint32_t)(int32_t)recordId, 2);
  c->mem_w32(obj + 0x14, node);
  if (node == 0) return;
  c->mem_w16(obj + 0x40, 0x46);
  c->mem_w8(obj + 7, (uint8_t)(c->mem_r8(obj + 7) + 1));
}

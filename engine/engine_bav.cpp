// engine_bav.cpp — per-area BAV (effect/animation cel) LOADER, owned PC-native.
//
// TARGET: gen_func_80096590 -> ov_bav_load. This is the engine's per-area effect-cel loader: it
// takes a BAV descriptor, allocates a cel SLOT, parses the descriptor's cel-record + UV tables,
// computes the packed VRAM layout, calls the caller-supplied allocator/upload callback to reserve
// VRAM and upload the image, then patches the per-frame tpage/clut halfwords into the cel records
// and latches the cel-system slot globals. (The "walking-dust" effect is one such BAV cel.)
//
// === ABI ===
//   a0 (s3) = BAV descriptor pointer (struct laid out below).
//   a1 (s0) = requested SLOT index: -1 = auto-allocate the first free slot, else an explicit slot [0,16).
//   a2 (s6) = ALLOCATOR/UPLOAD CALLBACK fn-ptr: called as cb(a0=size_rounded64, a1=arg4, a2=slot);
//             returns the allocated VRAM word-address, or -1 on failure. KEPT as a genuine leaf
//             (rec_dispatch) — it reserves VRAM + uploads the cel image; the engine doesn't own that here.
//   a3 (s5) = arg4 forwarded verbatim to the callback (an upload context / VRAM hint).
//   ret v0  = the allocated slot index on success; -1 / a leftover value on the various error paths
//             (transcribed exactly — see each `return` below).
//
// === BAV DESCRIPTOR STRUCT (a0) ===
//   +0  (u32) header word. magic test: (word >> 8) == 0x00564142 ("BAV"); low byte = TYPE byte.
//              if TYPE == 112 (0x70): a "112" variant.
//   +4  (u32) "kind/bpp selector": if (<5) the UV/frame sizes are scaled <<2 (4bpp-ish),
//              else <<3 (8bpp-ish). Same field gates the 64-vs-128 clamp (only when type byte == 112).
//   +18 (u16) "hu" field: cel-record COUNT bound — must be <= the 64/128 clamp; also indexes 0x80105C10
//              (>>14) and advances the data pointer by field18<<9 (=*512) to reach the UV table.
//   +22 (u8)  "bu" field: number of UV/frame entries minus 1 (loops run a1 in [0, bu]).
//   +32 ..    DATA: a table of 16-byte CEL RECORDS (count = the 64/128 clamp), then the per-frame UV table.
//
// === CEL RECORD (16-byte stride, base = descriptor+32) ===
//   +0  (u8)  presence/U byte (0 => record skipped in the packed index).
//   +8  (u32) packed index of this record among non-zero records (written by this loader, loop 1).
//   +12 (u16) tpage/clut halfword for EVEN frames (written loop 3 = (vram_base + cum_offset) >> 3).
//   +14 (u16) tpage/clut halfword for ODD  frames (written loop 3).
//
// === CEL-SYSTEM GLOBALS (all in the 0x80105Cxx/0x80105Dxx font+cel region) ===
//   0x80105C10 [slot*4]           (u32) = data ptr (descriptor+32)   [index = (slot<<16)>>14 = slot*4]
//   0x80105C50 [slot*4]           (u32) = the BAV descriptor pointer for this slot
//   0x80105C98 [slot*4]           (u32) = UV-table base ptr (after the cel records)  [index = slot*4]
//   0x80105CDA                    (u16) = the 64/128 cel-record-count clamp (sh)
//   0x80105CF0                    (u32) = 0  (cleared at entry, sw)
//   0x80105D18 [slot]             (u8)  = per-slot STATE byte: 0=free, 1=allocating, 2=loaded
//   0x80105D30 [slot*4]           (u32) = total packed VRAM size for this slot
//   0x80105D70                    (u16) = active-slot REFCOUNT (inc on allocate, dec on error-cleanup)
//   0x80105D78 [slot*4]           (u32) = allocated VRAM base word-address for this slot
//   0x800AC638                    (u32) = re-entrancy LOCK (1=FREE, 0=BUSY). Guard FUN_80099478 returns
//                                          (lock^1)!=0 -> bails when BUSY(0). FUN_80099450(1) ACQUIRES
//                                          (delay-slot `sw zero` -> lock=0/busy); FUN_80099450(0) RELEASES
//                                          (lock=1/free). The success path leaves the lock ACQUIRED(0); only
//                                          the cleanup tail (error paths) releases it to 1.
//
// === CALLEES ===
//   FUN_80099478 (lock-ready test) + FUN_80099450 (lock set/clear) — TRIVIAL pure mem ops on 0x800AC638,
//       owned inline here (no behavior beyond the single word).
//   *a2 (the VRAM allocator/upload callback) — genuine leaf, rec_dispatch'd in the recomp's exact order.
//
// VERIFY: `bavload` full RAM+scratchpad A/B vs rec_super_call (below). 0-diff is the gate.
#include "core.h"
#include "cfg.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

void rec_super_call(Core*, uint32_t);
void rec_dispatch(Core*, uint32_t);

#define BAV_LOCK   0x800AC638u
#define G_C10      0x80105C10u
#define G_C50      0x80105C50u
#define G_C98      0x80105C98u
#define G_CDA      0x80105CDAu
#define G_CF0      0x80105CF0u
#define G_D18      0x80105D18u   // slot state-byte table (16 bytes)
#define G_D30      0x80105D30u
#define G_D70      0x80105D70u   // refcount (u16)
#define G_D78      0x80105D78u

// --- lock helpers (FUN_80099478 / FUN_80099450), inlined ---
static inline int bav_lock_ready(Core* c) {            // FUN_80099478: (*LOCK ^ 1) != 0
  return (int)(((c->mem_r32(BAV_LOCK) ^ 1u) != 0u) ? 1u : 0u);
}
static inline void bav_lock_set(Core* c, uint32_t a0) {// FUN_80099450(a0): a0==1 -> *LOCK=0 (delay-slot sw zero); else *LOCK=1
  c->mem_w32(BAV_LOCK, (a0 == 1u) ? 0u : 1u);
}

// cleanup tail at 0x80096878: release lock (a0=0 path) + decrement refcount. v0 is the caller's
// pre-jump leftover (the asm does not reset it), so it is passed in and returned unchanged.
static uint32_t bav_cleanup_tail(Core* c, uint32_t v0) {
  bav_lock_set(c, 0);                                  // jal FUN_80099450 (a0 already 0 in the asm)
  uint32_t v1 = (uint32_t)(uint16_t)(c->mem_r16(G_D70) - 1);   // refcount--
  c->mem_w16(G_D70, (uint16_t)v1);
  return v0;
}

static uint32_t bav_load_native(Core* c) {
  const uint32_t desc = c->r[4];   // s3
  const int32_t  arg_slot = (int32_t)(int16_t)c->r[5];  // (int16)a1
  const uint32_t cb    = c->r[6];  // s6 — allocator/upload callback fn-ptr
  const uint32_t arg4  = c->r[7];  // s5
  int32_t s2 = 16;                 // slot accumulator / overflow sentinel
  uint32_t v0;

  // --- lock guard (0x800965c4): require the lock held, else bail -1 ---
  if (bav_lock_ready(c) == 1) return (uint32_t)-1;     // beq v0,1 -> exit, v0=-1
  bav_lock_set(c, 1);                                  // acquire

  int32_t a2;  // resolved slot
  if (arg_slot >= 16) {
    a2 = s2;                                            // sentinel 16 -> error path below
  } else if (arg_slot == -1) {
    int32_t found = -1;
    for (int32_t i = 0; i < 16; i++) {
      if (c->mem_r8(G_D18 + (uint32_t)i) == 0) { found = i; break; }
    }
    if (found >= 0) {
      c->mem_w8(G_D18 + (uint32_t)found, 1);            // slottab[i]=1
      c->mem_w16(G_D70, (uint16_t)(c->mem_r16(G_D70) + 1)); // refcount++
      s2 = found;
    }
    a2 = (int32_t)(int16_t)s2;                          // 16 if none free
  } else {
    if (c->mem_r8(G_D18 + (uint32_t)arg_slot) != 0) {
      a2 = (int32_t)(int16_t)s2;                        // busy -> a2=16
    } else {
      c->mem_w8(G_D18 + (uint32_t)arg_slot, 1);         // slottab[slot]=1 (s1=1)
      c->mem_w16(G_D70, (uint16_t)(c->mem_r16(G_D70) + 1)); // refcount++
      s2 = arg_slot;                                    // s2 = s0(=arg_slot)
      a2 = (int32_t)(int16_t)s2;
    }
  }

  if (a2 >= 16) {                                       // 0x8009668c slti<16 fails -> error
    bav_lock_set(c, 0);                                 // jal FUN_80099450(0)
    return (uint32_t)-1;
  }
  const int32_t slot = a2;

  // --- 0x800966a8: record descriptor in the slot table, clear 0x80105CF0 ---
  c->mem_w32(G_C50 + (uint32_t)slot * 4, desc);         // slot descriptor ptr
  c->mem_w32(G_CF0, 0);

  uint32_t hdr = c->mem_r32(desc + 0);                  // a1 = *(desc+0)
  uint32_t a3  = desc + 32;                             // data ptr (skip 32-byte header)
  if ((hdr >> 8) != 0x00564142u) {                      // magic "BAV" fail
    c->mem_w8(G_D18 + (uint32_t)slot, 0);               // free slot
    v0 = hdr >> 8;                                      // leftover v0 (0x800966d0)
    return bav_cleanup_tail(c, v0);                     // -> 0x80096878
  }

  // --- valid BAV: pick the 64/128 cel-record-count clamp ---
  uint32_t type_byte = hdr & 0xffu;
  uint32_t clamp;
  if (type_byte == 112u) {
    clamp = (c->mem_r32(desc + 4) < 5u) ? 64u : 128u;
  } else {
    clamp = 64u;
  }
  c->mem_w16(G_CDA, (uint16_t)clamp);

  uint32_t field18 = c->mem_r16(desc + 18);             // hu
  int32_t  clamp_s = (int16_t)c->mem_r16(G_CDA);        // reload (== clamp)
  if (clamp_s < (int32_t)field18) {                     // slt(clamp, field18): field18 > clamp -> error
    c->mem_w8(G_D18 + (uint32_t)((int32_t)(int16_t)s2), 0); // free slot
    v0 = (uint32_t)(int32_t)(int16_t)s2;                // leftover v0 = slot (0x8009673c)
    return bav_cleanup_tail(c, v0);                     // -> 0x80096878
  }

  // --- 0x80096754: record data ptr keyed by SLOT; cel-record table base = desc+32 ---
  // Index = ((int16)slot << 16) >> 14 = slot*4 byte offset (the sll s2,16 in the beq delay slot feeds
  // the sra ,14) -> G_C10[slot].  (NOT field18 — field18 here only drove the bounds-check above.)
  c->mem_w32(G_C10 + (uint32_t)slot * 4, a3);
  uint32_t s3rec = a3;                                  // s3 = cel-record table base (desc+32)
  a3 = s3rec + clamp * 16;                              // advance past clamp cel records

  // --- loop 1 (0x80096770): pack non-zero record index into rec+8 ---
  int32_t s0 = 0;
  if ((int32_t)clamp > 0) {                             // blez clamp skips
    uint32_t rec = s3rec;
    for (int32_t i = 0; i < (int32_t)clamp; i++) {
      uint32_t b = c->mem_r8(rec + 0);
      c->mem_w32(rec + 8, (uint32_t)s0);                // rec+8 = packed index (stored before the ++)
      if (b != 0) s0++;
      rec += 16;
    }
  }

  // --- loop 2 (0x800967a8): record UV-table base, sum per-entry sizes into s0 + a scratch array ---
  s0 = 0;
  c->mem_w32(G_C98 + (uint32_t)slot * 4, a3);   // UV-table base, keyed by slot (sll s2,16 >> 14 = slot*4)
  int32_t sizes[256];                                   // = the sp+16 scratch array (per-entry sizes)
  {
    uint32_t f18 = c->mem_r16(desc + 18);
    a3 += f18 << 9;                                      // skip field18*512 to the UV table
    uint32_t s4 = c->mem_r8(desc + 22);                  // bu (byte)
    uint32_t kind = c->mem_r32(desc + 4);
    uint32_t bu = s4 & 0xffu;
    uint32_t pa3 = a3;
    for (int32_t i = 0; i < 256; i++) {
      if ((uint32_t)i <= bu) {                           // slt(bu,i) false -> run body (i<=bu)
        uint32_t v1 = c->mem_r16(pa3);                   // UV halfword
        uint32_t sz = (kind < 5u) ? (v1 << 2) : (v1 << 3);  // kind<5 -> <<2; else <<3 (asm 0x800967f0)
        sizes[i] = (int32_t)sz;
        s0 += (int32_t)sz;
      }
      pa3 += 2;                                          // a3 += 2 (every iter, even when skipped)
    }
  }

  // --- 0x80096824: round total size up to 64, call the allocator/upload callback ---
  uint32_t size_r = ((uint32_t)s0 + 63u) & ~63u;
  int32_t s1slot = (int32_t)(int16_t)s2;                // slot
  c->r[4] = size_r;                                     // a0 = rounded size
  c->r[5] = arg4;                                       // a1 = arg4 (s5)
  c->r[6] = (uint32_t)s1slot;                           // a2 = slot
  rec_dispatch(c, cb);                                  // jalr s6
  uint32_t a0v = c->r[2];                               // v0 = vram base addr (or -1)

  if (a0v == (uint32_t)-1) {                            // alloc fail -> exit, v0=-1
    return (uint32_t)-1;
  }

  // --- 0x80096854: VRAM overflow check (base+size > 0x80000) ---
  uint32_t endw = a0v + size_r;
  if (0x00080000u < endw) {                             // sltu(0x80000,end) -> overflow
    c->mem_w8(G_D18 + (uint32_t)s1slot, 0);             // free slot
    v0 = (uint32_t)((uint32_t)s1slot << 2);             // leftover v0 = slot*4 (0x80096864 delay slot)
    return bav_cleanup_tail(c, v0);                     // -> 0x80096878
  }

  // --- success (0x800968a0): latch VRAM base, write per-frame tpage/clut halfwords ---
  c->mem_w32(G_D78 + (uint32_t)s1slot * 4, a0v);        // VRAM base for slot
  int32_t off = 0;                                      // s0 = cumulative byte offset
  {
    int32_t bu = (int32_t)(c->mem_r8(desc + 22) & 0xffu);  // s4 & 0xff
    if (bu >= 0) {                                       // slti(v1,0) -> if v1<0 skip
      for (int32_t i = 0; i <= bu; i++) {                // slt(bu,i) -> while i<=bu
        off += sizes[i];                                 // s0 += size (BEFORE the offset compute)
        uint32_t rec = (uint32_t)(i >> 1) * 16 + s3rec;  // cel record (i/2)
        uint32_t word = (a0v + (uint32_t)off) >> 3;      // (vram_base + cum) >> 3
        if (i & 1) c->mem_w16(rec + 14, (uint16_t)word); // odd  -> +14
        else       c->mem_w16(rec + 12, (uint16_t)word); // even -> +12
      }
    }
  }

  // --- 0x8009692c: total size + mark slot loaded ---
  int32_t slot_f = (int32_t)(int16_t)s2;
  c->mem_w32(G_D30 + (uint32_t)slot_f * 4, (uint32_t)off);
  c->mem_w8(G_D18 + (uint32_t)slot_f, 2);               // slot state = 2 (loaded)

  // BONUS: log the loaded cel's atlas params (under the `bavload` debug channel).
  {
    static int dbg = -1; if (dbg < 0) dbg = cfg_dbg("bavload") ? 1 : 0;
    if (dbg) {
      uint32_t kind = c->mem_r32(desc + 4);
      uint32_t r0_12 = c->mem_r16(s3rec + 12), r0_14 = c->mem_r16(s3rec + 14);
      uint32_t bpp = (kind < 5u) ? 4u : 8u;            // kind<5 -> <<2 (4bpp); else <<3 (8bpp)
      fprintf(stderr,
        "[bavload] CEL slot=%d desc=%08x kind=%u(bpp~%u) frames(clamp)=%u uv_entries=%d "
        "vram_base_word=%05x size=%u rec0.w12=%04x rec0.w14=%04x\n",
        slot_f, desc, kind, bpp, clamp, (int)(c->mem_r8(desc + 22) & 0xff) + 1,
        a0v, (unsigned)off, r0_12 & 0xffff, r0_14 & 0xffff);
    }
  }

  return (uint32_t)slot_f;                              // v0 = slot
}

void ov_bav_load(Core* c) {
  static int s_v = -1; if (s_v < 0) s_v = cfg_dbg("bavload") ? 1 : 0;
  if (!s_v) { c->r[2] = bav_load_native(c); return; }

  // ---- full RAM + scratchpad A/B gate vs rec_super_call ----
  // The native path runs first; its writes are snapshotted and rolled back, then the recomp body runs and
  // we diff full RAM + scratchpad + v0. The allocator/upload callback (jalr s6 = FUN_800964b4 -> the VRAM
  // allocator FUN_800977c0) runs in BOTH passes — its free-list lives in tracked RAM (0x800AC5xx/6xx) and
  // is rolled back, so it returns the SAME VRAM base each pass (verified: D78 native==oracle). The only
  // benign residue is the top-of-RAM stack window: this fn's 1072-byte frame + the callee frames live
  // below entry sp with no native frame there, so the gate excludes [sp-0x800, sp) — far above all game
  // data (sp ~0x1FExxx) — same family rationale as gridresolve/scriptvm/pad931c0.
  static uint8_t* ramN = (uint8_t*)malloc(0x200000);
  uint8_t* ram0 = (uint8_t*)malloc(0x200000);
  uint8_t spad0[0x400], spadN[0x400];
  uint32_t regs0[32]; memcpy(regs0, c->r, sizeof regs0);
  uint32_t desc = c->r[4], aslot = c->r[5];
  memcpy(ram0, c->ram, 0x200000); memcpy(spad0, c->scratch, 0x400);

  uint32_t v0_n = bav_load_native(c);
  memcpy(ramN, c->ram, 0x200000); memcpy(spadN, c->scratch, 0x400);
  memcpy(c->ram, ram0, 0x200000); memcpy(c->scratch, spad0, 0x400); memcpy(c->r, regs0, sizeof regs0);
  rec_super_call(c, 0x80096590u);
  uint32_t v0_o = c->r[2];

  uint32_t sp = regs0[29] & 0x1FFFFFu, flo = (sp >= 0x800) ? sp - 0x800 : 0;
  int ro = -1; for (uint32_t a = 0; a < 0x200000; a++) if (c->ram[a] != ramN[a] && !(a >= flo && a < sp)) { ro = (int)a; break; }
  int so = -1; for (uint32_t a = 0; a < 0x400; a++) if (c->scratch[a] != spadN[a]) { so = (int)a; break; }
  static long ng = 0, nb = 0;
  if (ro >= 0 || so >= 0 || v0_n != v0_o) {
    if (nb++ < 40) fprintf(stderr, "[bavload] MISMATCH desc=%08x slot=%d v0 n=%x o=%x ram@%x spad@%x sp=%x\n",
                           desc, (int)(int16_t)aslot, v0_n, v0_o, ro, so, sp);
  } else if (++ng % 5 == 0) fprintf(stderr, "[bavload] %ld matches\n", ng);
  c->r[2] = v0_o;
  free(ram0);
}

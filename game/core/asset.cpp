// game/core/asset.cpp — class Asset — PC-native ASSET LOADING subsystem owned by Engine.
// Owns the engine's loading domain (per CLAUDE.md THE BOUNDARY): the texture-group loader
// orchestration, the LZ image decompressor, the CPU->VRAM upload, and the stage-0/1 boot preload
// chain. The CD read + the task terminal-yield stay the retained platform/content mechanism (called
// via rec_dispatch, not transcribed). See asset.h — instance-with-back-pointer subsystem, methods
// take typed args directly (no MIPS taxi c->r[4..7] marshal).
#include "core.h"
#include "game.h"    // c->game->hle.deliverEvent — Hle subsystem lives on Game
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include "asset.h"
#include "guest_call.h" // rc1-4 guest-call helpers (used by the preload chain below)
void rec_dispatch(Core*, uint32_t);
// gpu_native_load_image is declared in core.h (the native CPU->VRAM upload).

// PC-owned LZ image decompressor — replaces recompiled FUN_80044D8C (0x80044D8C). Rebuilds per-frame
// CLUTs (0x801FCDC0) and sprite/texture data from compressed area assets. It was the source of the
// gameplay 2D-sprite corruption: the SAME function gave correct output when recompiled but ZEROS
// when flat-interpreted by the coroutine interpreter (rec_coro_run) at runtime — a recompiler-vs-
// interpreter divergence. A pure decompressor belongs to the PC side, so we own it natively here.
//
// ABI (matches the MIPS at 0x80044D8C, verified by disassembly):
//   desc=descriptor, dst=dest, src0=src bytes, srclen=len. Returns bytes written.
//   Setup: build 8 back-ref offsets from the static table at 0x800153C8, scaled by the per-call
//   stride at desc+4:  offset[i] = base[i] + 2*(factor[i]*stride)  (2D image predictors: mode 1 =
//   previous byte, modes 2-7 = previous-row neighbours; row pitch = stride).
//   Stream of control bytes: len=ctrl>>3, mode=ctrl&7.  mode==0 -> literal copy `len` bytes from
//   src (ctrl byte 0 / len 0 terminates).  mode!=0 -> back-ref copy `len` bytes from dest+offset
//   [mode], BYTE-granular so overlapping copies replicate (RLE), exactly as the original loop.
#define LZ_OFFTAB_BASE 0x800153C8u
uint32_t Asset::lzDecompress(uint32_t desc, uint32_t dst, uint32_t src0, uint32_t srclen) {
  Core* c = this->core;
  const uint32_t src_end = src0 + srclen;
  const int32_t stride = c->mem_r16s(desc + 4);
  // Frame discipline (faithful-execution model): the substrate body descends sp by 32 and builds
  // the 8-entry back-ref offset table IN ITS GUEST FRAME (sp+0..+28) — compared task-stack bytes
  // under strict SBS. Keep the guest-frame copy authoritative alongside the host mirror.
  c->r[29] -= 32;
  int32_t offtab[8];
  for (int i = 0; i < 8; i++) {
    const int32_t base   = (int32_t)c->mem_r32(LZ_OFFTAB_BASE + i * 8 + 0);
    const int32_t factor = (int32_t)c->mem_r32(LZ_OFFTAB_BASE + i * 8 + 4);
    offtab[i] = base + 2 * (factor * stride);
    c->mem_w32(c->r[29] + (uint32_t)i * 4, (uint32_t)offtab[i]);
  }
  uint32_t src = src0, out = dst;
  while (src < src_end) {
    const uint8_t ctrl = c->mem_r8(src++);
    const uint32_t len = ctrl >> 3, mode = ctrl & 7u;
    if (mode != 0) {                                  // back-reference into the output so far
      uint32_t bsrc = out + (uint32_t)offtab[mode];
      for (uint32_t k = 0; k < len; k++) c->mem_w8(out++, c->mem_r8(bsrc++));
    } else {                                          // literal run from the source
      if (len == 0) break;                            // terminator
      for (uint32_t k = 0; k < len; k++) c->mem_w8(out++, c->mem_r8(src++));
    }
  }
  c->r[29] += 32;                                     // frame ascent
  return out - dst;                                   // total bytes written
}

// PC-owned texture-group unpacker — replaces recompiled FUN_80044E84 (0x80044E84). Verified by
// disassembly: tablePtr = descriptor table base, anchorEnd = scratch-end anchor (0x1FD000). Layout:
// [count:4] then [pad:4] then `count` 12-byte entries each { stride:2(@+4 from entry head),
// field:2(@+6), srclen:4(@+8) }; source data starts 0x800 after the table base and advances by
// srclen per entry. For each entry: dst = anchorEnd - 2*stride*field (outputs stack ending at the
// anchor — transient scratch), decompress the entry's image there, then upload it (FUN_80081218)
// and run the post step (FUN_80080f6c). Non-gameplay (asset unpack) → PC-owned, calling the native
// decompressor directly; the two gfx-library sub-calls still route through the recomp/dispatch.
void Asset::unpackGroup(uint32_t tablePtr, uint32_t anchorEnd) {
  Core* c = this->core;
  const int32_t count = (int32_t)c->mem_r32(tablePtr);
  uint32_t entry = tablePtr + 4;         // first 12-byte descriptor entry
  uint32_t src = tablePtr + 0x800;       // packed source data follows the table
  int dbg = cfg_dbg("unpack") != 0;
  if (dbg) fprintf(stderr, "[unpack] table=0x%08X count=%d src0=0x%08X ra=0x%08X\n",
                   tablePtr, count, src, c->r[31]);
  // PSXPORT_UNPACKDUMP=dir — dump the LIVE compressed input (table + 0x30000 bytes) the moment the
  // unpacker reads it, sequence-numbered, so it can be checked against the disc / oracle exactly.
  { const char* dd = cfg_str("PSXPORT_UNPACKDUMP");
    if (dd) { static int seq = 0; char p[300]; snprintf(p, sizeof p, "%s/unpack_%03d_c%d.bin", dd, seq++, count);
      FILE* uf = fopen(p, "wb"); if (uf) {
        // Dump from the staging base to the end of RAM (archives can be up to ~0x76000 from 0x8018A000).
        uint32_t off = tablePtr & 0x1FFFFF, len = 0x200000u - off;
        fwrite(&c->ram[off], 1, len, uf); fclose(uf);
        fprintf(stderr, "[unpack] dumped live input -> %s (table=0x%08X count=%d)\n", p, tablePtr, count); } } }
  for (int32_t i = 0; i < count; i++) {
    const uint32_t desc   = entry;
    const int32_t  stride = c->mem_r16s(desc + 4);
    const int32_t  field  = c->mem_r16s(desc + 6);
    const uint32_t srclen = c->mem_r32(desc + 8);
    const uint32_t dst    = anchorEnd - (uint32_t)(2 * stride * field);
    if (dbg) fprintf(stderr, "[unpack]  e%d dst=(%d,%d) %dx%d src=0x%08X len=%u srcbytes:"
                     " %02X %02X %02X %02X\n", i, c->mem_r16s(desc), c->mem_r16s(desc+2),
                     stride, field, src, srclen, c->mem_r8(src), c->mem_r8(src+1), c->mem_r8(src+2), c->mem_r8(src+3));
    lzDecompress(desc, dst, src, srclen);                // native decompress into transient scratch
    src   += srclen;
    entry += 12;
    uploadImage(desc, dst);                              // FUN_80081218(desc, dst): native VRAM upload
    //   (direct C call — the override table that routed 0x80081218 here is gone; top-down PC-driven)
    // Per-image post-step FUN_80080f6c(0) = libgs GPU DrawSync between uploads — meaningless for our
    // synchronous native upload (no async DMA to drain). Owned as a skip; see note above uploadImage.
    if (cfg_dbg("unpacksync")) { c->r[4] = 0; rec_dispatch(c, 0x80080F6Cu); }
  }
}

// FAITHFUL texture-group unpacker — FUN_80044E84 with full guest-stack discipline (faithful-
// execution model; used by the pc_faithful task-1 body loadTexgroup). Same algorithm as
// unpackGroup above, but: frame descent 48 with the RE'd live s-reg/ra spills at +16..+44, loop
// state maintained in the guest s-registers (nested callee prologues spill them into compared
// task-1 stack bytes), and the two libgs leaves — LoadImage FUN_80081218 and DrawSync
// FUN_80080F6C — dispatched at the live guest sp with their call-site ra constants, exactly as
// core B's substrate body runs them. Byte shape: generated gen_func_80044E84 (shard_5.c).
void Asset::unpackGroupFaithful(uint32_t tablePtr, uint32_t anchorEnd) {
  Core* c = this->core;
  c->r[29] -= 48;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 24, c->r[18]); c->r[18] = tablePtr;
  c->mem_w32(sp + 40, c->r[22]); c->r[22] = anchorEnd;
  c->mem_w32(sp + 36, c->r[21]); c->r[21] = tablePtr + 2048;   // packed source follows the table
  c->mem_w32(sp + 44, c->r[31]);
  c->mem_w32(sp + 32, c->r[20]);
  c->mem_w32(sp + 28, c->r[19]);
  c->mem_w32(sp + 20, c->r[17]);
  c->mem_w32(sp + 16, c->r[16]);
  const int32_t count = (int32_t)c->mem_r32(c->r[18]);
  c->r[18] += 4;                                      // first 12-byte descriptor entry
  c->r[20] = (uint32_t)(count - 1);                   // loop counter in s4 (branch-delay decrement)
  if (count > 0) {
    c->r[19] = c->r[18] - 6;
    for (;;) {
      c->r[19] += 12;
      const int32_t stride = c->mem_r16s(c->r[19] - 2);        // desc+4
      const int32_t field  = c->mem_r16s(c->r[19] + 0);        // desc+6
      c->r[17] = c->r[18];                                     // desc (s1)
      c->r[18] += 8;
      const uint32_t srclen = c->mem_r32(c->r[18]);            // desc+8
      c->r[18] += 4;                                           // next descriptor
      c->r[6] = c->r[21];                                      // src
      c->r[21] += srclen;
      c->r[16] = c->r[22] - (uint32_t)(2 * stride * field);    // dst = transient scratch (s0)
      c->r[4] = c->r[17]; c->r[5] = c->r[16]; c->r[7] = srclen; c->r[31] = 0x80044F10u;
      lzDecompress(c->r[17], c->r[16], c->r[6], srclen);       // FUN_80044D8C (guest-frame offtab)
      c->r[4] = c->r[17]; c->r[5] = c->r[16]; c->r[31] = 0x80044F1Cu;
      rec_dispatch(c, 0x80081218u);                            // libgs LoadImage at the live sp
      c->r[4] = 0; c->r[31] = 0x80044F24u;
      rec_dispatch(c, 0x80080F6Cu);                            // libgs DrawSync(0)
      const int32_t rem = (int32_t)c->r[20];
      c->r[20] = (uint32_t)(rem - 1);
      if (rem <= 0) break;
    }
  }
  c->r[31] = c->mem_r32(sp + 44);
  c->r[22] = c->mem_r32(sp + 40);
  c->r[21] = c->mem_r32(sp + 36);
  c->r[20] = c->mem_r32(sp + 32);
  c->r[19] = c->mem_r32(sp + 28);
  c->r[18] = c->mem_r32(sp + 24);
  c->r[17] = c->mem_r32(sp + 20);
  c->r[16] = c->mem_r32(sp + 16);
  c->r[29] += 48;
}

// PC-native TEXTURE-GROUP LOADER — owns the asset-load ORCHESTRATION FUN_80044F58 (0x80044F58): the
// per-group loader a level uses to stream a texture set into VRAM. RE (tools/disas.py + gen_func_80044F58):
// the current task selects a set via task[0x6D]=mode / task[0x6E]=set, then
//   1. CD-load a 2KB HEADER from sector (filebase0 = *0x800BE0F0) + set  [+ a 4/26 bias in mode 2] -> 0x800EF478
//   2. CD-load the compressed ARCHIVE from sector (filebase1 = *0x800BE0F8) + (hdr[0]>>11), len hdr[1]-hdr[0]
//      -> the fixed staging buffer 0x8018A000 (descriptor table in its first 0x800 bytes, packed data after)
//   3. UNPACK the archive (unpackGroup) -> decompress + upload each image to its VRAM (x,y) (owned)
//   4. copy a 42-word metadata table from hdr+0x100 (0x800EF578) -> 0x800FB170 (per-set sprite/CLUT meta the
//      game content reads back)
//   5. (mode==0 only) set _DAT_1f80019b=1 and run the task TERMINAL YIELD FUN_80051FB4 (suspend until next
//      frame — this is what streams the groups one-per-frame).
// ENGINE asset orchestration -> reimplemented PC-native; the CD read (via 0x8001DC40) and the terminal task
// yield (the scheduler) stay the retained platform/content mechanism (called, not transcribed). The
// mode/set inputs and the 0x800FB170 metadata are CONTENT-interface state, so this is gated on the main-
// RAM A/B diff (later-177). For mode==0 the terminal yield does not return (switch longjmps mid-game),
// exactly like eng_stage_transition's tail; there is no code after it.
// FAITHFUL guest-stack discipline (faithful-execution model): frame descent 24 with ra/s0 spills
// at +20/+16 (live values), header/archive CD reads dispatched with their call-site ra constants,
// unpackGroupFaithful for step 3 (its libgs leaves run at the live guest sp), and the metadata-
// copy cursor kept in the guest s0 (selfClose's prologue spills it). Byte shape: generated
// gen_func_80044F58. This method only runs on the pc_faithful task-1 path (runTask1PreloadStanza);
// the pc_skip shortcut is preloadTexgroup below.
void Asset::loadTexgroup() {
  Core* c = this->core;
  c->r[29] -= 24;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 20, c->r[31]);
  c->mem_w32(sp + 16, c->r[16]);
  const uint32_t task = c->mem_r32(0x1F800138u);
  const uint32_t mode = c->mem_r8(task + 0x6Du);
  const uint32_t set  = c->mem_r8(task + 0x6Eu);
  uint32_t hdr_sector = c->mem_r32(0x800BE0F0u) + set;             // filebase0 + set
  if (mode == 2) {                                                 // mode-2 per-set 4/26-sector bias
    uint16_t mask = c->mem_r16(0x800BFE56u);
    hdr_sector += ((mask >> (set & 31)) & 1) ? 26u : 4u;
  }
  const uint32_t HDR = 0x800EF478u;
  c->r[4] = HDR; c->r[5] = hdr_sector; c->r[6] = 2048; c->r[31] = 0x80044FD8u;
  rec_dispatch(c, 0x8001DC40u);                                    // 1. CD-load 2KB header (platform)
  c->r[16] = HDR;                                                  // s0 = header base (live for spills)
  uint32_t h0 = c->mem_r32(HDR + 0), h1 = c->mem_r32(HDR + 4);
  c->r[4] = 0x8018A000u; c->r[5] = c->mem_r32(0x800BE0F8u) + (h0 >> 11); c->r[6] = h1 - h0;
  c->r[31] = 0x80045008u;
  rec_dispatch(c, 0x8001DC40u);                                    // 2. CD-load compressed archive -> staging
  c->r[4] = 0x8018A000u; c->r[5] = 0x001FD000u; c->r[31] = 0x8004501Cu;
  unpackGroupFaithful(0x8018A000u, 0x1FD000u);                     // 3. unpack -> decompress + VRAM upload
  for (uint32_t i = 0; i < 42; i++) {                              // 4. per-set metadata table (cursor in s0)
    c->mem_w32(0x800FB170u + i * 4, c->mem_r32(c->r[16] + 256u));
    c->r[16] += 4;
  }
  if (c->mem_r8(c->mem_r32(0x1F800138u) + 0x6Du) == 0) {           // 5. terminal task end (mode 0)
    c->mem_w8(0x1F80019Bu, 1);
    c->r[31] = 0x80045070u;
    c->game->pcSched.selfClose();                                  // FUN_80051FB4 — does not return on a task
  }
  c->r[31] = c->mem_r32(c->r[29] + 20);
  c->r[16] = c->mem_r32(c->r[29] + 16);
  c->r[29] += 24;
}

// Per-image post-step FUN_80080f6c(0) = the game's libgs frame DrawSync: FUN_80083364(0) waits for the
// GPU's ordering-table DMA to drain and the GPU to go idle (polls GPUSTAT @0x800a5ab4 bit 0x01000000 /
// @0x800a5aa8 bit 0x04000000), having queued the (now-empty, since uploadImage bypasses it) ring.
// IMPORTANT: this same function is the MAIN-LOOP per-frame DrawSync, so it must NOT be globally overridden
// (a global no-op stalls all presentation). Inside the unpack loop, however, it is a between-uploads GPU
// sync that is meaningless for our SYNCHRONOUS native VRAM upload — there is no async DMA to wait on — so
// the unpack loop owns it as a skip. A/B-verified VRAM+RAM-identical across a full menu+seaside load
// (later-177): the unpack call site only ever passes a0=0 with an empty ring. DIAG: PSXPORT_DEBUG=unpacksync
// restores the per-image super-call to prove equivalence.

// PC-native CPU->VRAM upload — replaces the game's libgs-style upload library FUN_80081218
// (0x80081218). RE (verified empirically vs the A0 upload log, later-62/63): desc = descriptor
// { x:s16@0, y:s16@2, w:s16@4, h:s16@6 }, src = source pixel data (w*h contiguous 16-bit pixels,
// row-major). The recomp body ENQUEUES an entry into the GsSortObject ring at 0x800A5AC8 (head/
// tail @0x800A5AC8/5ACC) which is DMA'd to the GPU later as a 0xA0 packet. It is the SINGLE
// chokepoint for BOTH the scene-load texture atlas AND every per-frame 16x1 CLUT — 5300+ calls
// per attract run. The user's directive: the GPU library must be PC-native, not a faithful recomp.
// So we write the rect straight into native VRAM here and DO NOT enqueue (the later ring flush/sync
// then no-ops over an empty ring). Ordering is preserved: the upload still happens before this
// frame's draws are processed, and CLUTs are double-buffered across frames (parity-alternated
// slots), so no draw reads a slot mid-overwrite.
void Asset::uploadImage(uint32_t desc, uint32_t src) {
  Core* c = this->core;
  const int x = c->mem_r16s(desc + 0), y = c->mem_r16s(desc + 2);
  const int w = c->mem_r16s(desc + 4), h = c->mem_r16s(desc + 6);
  if (w > 0 && h > 0) gpu_native_load_image(c, x, y, w, h, src);
}

// ===== Stage-0/stage-1 area/asset PRELOAD — PC-native + SYNCHRONOUS (moved from native_boot.cpp,
// 2026-07 restructure). Reuses the existing leaf natives above (Cd::loadFile for the sync CD
// reads, unpackGroup for decompress+VRAM upload) — same content-loading domain as the texgroup loader.

// FUN_80044F58 texture-group load, synchronous. (Mirrors loadTexgroup but driven by explicit
// (mode,set) — no task-1 spawn, no terminal yield.) Header sector -> archive -> unpack -> copy the
// 42-word per-set metadata table the still-recomp content reads back.
void Asset::preloadTexgroup(uint32_t mode, uint32_t set) {
  Core* c = this->core;
  uint32_t hdr_sector = c->mem_r32(0x800BE0F0u) + set;             // filebase0 + set
  if (mode == 2) {                                                 // mode-2 per-set 4/26-sector bias
    uint16_t mask = (uint16_t)c->mem_r16(0x800BFE56u);
    hdr_sector += ((mask >> (set & 31)) & 1) ? 26u : 4u;
  }
  c->game->cd.loadFile(0x800EF478u, hdr_sector, 2048);           // 1. 2KB header
  uint32_t h0 = c->mem_r32(0x800EF478u), h1 = c->mem_r32(0x800EF47Cu);
  c->game->cd.loadFile(0x8018A000u, c->mem_r32(0x800BE0F8u) + (h0 >> 11), h1 - h0);  // 2. compressed archive
  unpackGroup(0x8018A000u, 0x1FD000u);                             // 3. decompress + VRAM upload (native)
  for (uint32_t i = 0; i < 42; i++)                                // 4. per-set metadata table
    c->mem_w32(0x800FB170u + i * 4, c->mem_r32(0x800EF478u + 0x100u + i * 4));
}

// FUN_800753D4 cel-load, SYNCHRONOUS. Original: FUN_80096480 (slot alloc + BAV cel load) -> store slot
// at `out` -> FUN_80096980 (kick the upload state machine) -> cross-frame poll FUN_80096a40 until the
// GPU-DMA upload completes. The alloc + kick carry no async wait (they leave the slot in state 1), so
// run them as the recomp REFERENCE; our native GPU upload is synchronous, so we DROP the cross-frame
// poll (the "no async" directive) instead of yielding for a DMA that already happened.
static void preload_cel(Core* c, uint32_t out, uint32_t desc, uint32_t cbarg) {
  int16_t slot = (int16_t)(rc3(c, 0x80096480u, desc, (uint32_t)-1, cbarg), c->r[2]);  // FUN_80096480(desc,-1,cbarg)
  c->mem_w16(out, (uint16_t)slot);                               // *(u16*)out = allocated slot
  rc2(c, 0x80096980u, cbarg, (uint32_t)slot);                    // FUN_80096980(cbarg, slot): kick upload
  // FUN_800753D4 writes `out` a SECOND time with FUN_80096980's RETURN (the real slot handle; -1 when
  // the SPU-DMA kick found no free channel). Dropping this write-back left a stale positive slot id in
  // the cel handle (0x800BED84/0x800BED82) on the failure branch — the sprite/cel registrations then
  // reference the wrong SPU sample bank: bug #29 "right instrument, wrong sample" on the skip path.
  // RE: scratch/decomp/vab_open.c (FUN_800753d4) + vab_kick.c (FUN_80096980). 2026-07-08.
  c->mem_w16(out, (uint16_t)c->r[2]);
  // Drain the BAV upload queue SYNCHRONOUSLY so this cel's VAB bank reaches SPU (and its slot frees so the
  // NEXT cel can allocate). The real FUN_800753d4 polls 0x80096a40, whose 0x800993a0 sync busy-waits on the
  // upload's DMA-complete IRQ event — which never fires in this no-IRQ preload, so the original code dropped
  // the poll and silently skipped a whole VAB bank (proved by the dual-core SPU-DMA diff: 1 transfer vs PSX's
  // 2). Deliver the sound-DMA-complete event first so 0x800993a0 returns immediately (no busy-wait, no yield),
  // then run the sync once. (later: in-game music VAB.)
  c->game->hle.deliverEvent(0xF0000009u, 0xFFFFFFFFu);           // sound/DMA-complete event
  rc1(c, 0x80096a40u, 0);                                        // FUN_80096a40(0): upload sync (now non-blocking)
}

// FUN_800754F4 cel/sprite VRAM build, synchronous. FUN_800753ac is itself an async CD read -> use the
// sync loadfile; the two FUN_800753d4 cel-loads go through preload_cel; the ten FUN_80075448
// sprite-cell registrations carry no CD/async wait, so run as recomp. `base` = work base 0x80182000.
static uint32_t preload_build_vram(Core* c, uint32_t base) {
  uint32_t s0 = base + 0x51000u;                                   // descriptor table (filled by the read)
  c->game->cd.loadFile(base, c->mem_r32(0x800BE108u), 0x51800u);  // FUN_800753ac: read SND file (idx3)
  preload_cel(c, 0x800BED84u, base + c->mem_r32(s0 + 0x28), base + c->mem_r32(s0 + 0x30));
  preload_cel(c, 0x800BED82u, base + c->mem_r32(s0 + 0x2c), base + c->mem_r32(s0 + 0x34));
  static const struct { uint32_t off, sz; } cells[10] = {
    {0x0c,14},{0x08,14},{0x04,14},{0x00,14},{0x10,8},{0x14,8},{0x18,8},{0x1c,14},{0x20,14},{0x24,14},
  };
  uint32_t cell_h = (uint32_t)c->mem_r16s(0x800BED82u);
  for (int i = 0; i < 10; i++)
    rc4(c, 0x80075448u, (uint32_t)i, base + c->mem_r32(s0 + cells[i].off), cells[i].sz, cell_h);
  return base + 26356;                                             // v0 = base + 0x66f4
}

// FUN_8004514C — the stage-1 callback body. SWDATA + DAT load, shared texgroup sub-load,
// relocation table, cel/sprite VRAM build. Direct entry: no done_flag / task-end.
void Asset::preloadStage1() {
  Core* c = this->core;
  c->game->cd.loadFile(0x80157000u, c->mem_r32(0x800BE110u), c->mem_r32(0x800BE114u));  // SWDATA.BIN
  preloadTexgroup(1, 1);                                           // shared texgroup sub-load
  uint32_t lo = c->mem_r32(0x800EF480u), hi = c->mem_r32(0x800EF484u);
  c->game->cd.loadFile(0x80158000u, c->mem_r32(0x800BE100u) + (lo >> 11), hi - lo);     // DAT payload
  uint32_t dat_end = (hi - lo) + 0x80158000u;
  c->mem_w32(0x1F800228u, dat_end);
  c->mem_w32(0x800ED014u, dat_end);
  int32_t n = (int32_t)c->mem_r32(0x800EF488u);                    // relocation table (blez -> skip)
  for (int32_t i = 0; i < n; i++) {
    uint32_t word = c->mem_r32(0x800EF48Cu + i * 4);
    c->mem_w32(0x800ECF58u + (word >> 24) * 4, (word & 0x00FFFFFFu) + 0x80158000u);
  }
  uint32_t v0 = preload_build_vram(c, 0x80182000u);
  c->mem_w32(0x1F80022Cu, v0);
}

// Task-1 body — FAITHFUL FUN_8004514C, run on a PcScheduler native fiber. Guest-stack
// discipline (faithful-execution model): frame descent 32 with s0/s1/ra spills at +16/+20/+24,
// r16/r17 carried live along the shard's flow (deeper callees — 8001DC40, 800754F4 — spill
// them into THEIR frames, so the values must match core B's at each dispatch boundary). The
// SEQ/VAB VRAM build stays the substrate leaf 0x800754F4: its FUN_800753D4 poll loop yields
// through scheduler_yield each frame the SsVabTransCompleted flag is still clear, which is what
// spreads this body across two slices on core B — dispatching the real leaf reproduces both the
// cadence and its stack bytes organically. Byte shape: generated gen_func_8004514C. The pc_skip
// shortcut is preloadStage1() above (synchronous, no yields — must never run on this path).
void Asset::preloadStage1AsTask() {
  Core* c = this->core;
  c->r[29] -= 32;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 16, c->r[16]);
  c->mem_w32(sp + 24, c->r[31]);
  c->mem_w32(sp + 20, c->r[17]);
  c->r[16] = 0x800BE0F0u;                                          // s0 = boot file table
  c->r[4] = 0x80157000u;                                           // 1. SWDATA.BIN
  c->r[5] = c->mem_r32(0x800BE110u);
  c->r[6] = c->mem_r32(0x800BE114u);
  c->r[31] = 0x80045178u;
  rec_dispatch(c, 0x8001DC40u);
  c->r[31] = 0x80045180u;                                          // 2. texgroup sub-load
  c->r[17] = 0x800F0000u;                                          //    (s1 upper set in the jal delay slot)
  loadTexgroup();                                                  //    faithful FUN_80044F58
  c->r[17] = 0x800EF478u;                                          // s1 = SWDATA descriptor block
  uint32_t lo = c->mem_r32(0x800EF480u), hi = c->mem_r32(0x800EF484u);
  c->r[4] = 0x80158000u;                                           // 3. DAT payload
  c->r[5] = c->mem_r32(0x800BE100u) + (lo >> 11);
  c->r[6] = hi - lo;
  c->r[16] = hi - lo;                                              // s0 = payload size
  c->r[31] = 0x800451ACu;
  rec_dispatch(c, 0x8001DC40u);
  const uint32_t dat_end = (hi - lo) + 0x80158000u;
  c->mem_w32(0x1F800228u, dat_end);
  c->mem_w32(0x800ED014u, dat_end);
  const int32_t n = (int32_t)c->mem_r32(0x800EF488u);              // 4. relocation table
  for (int32_t i = 0; i < n; i++) {
    uint32_t word = c->mem_r32(0x800EF48Cu + i * 4);
    c->r[16] = word >> 24;                                         // shard clobbers s0 with the slot index
    c->mem_w32(0x800ECF58u + (word >> 24) * 4, (word & 0x00FFFFFFu) + 0x80158000u);
  }
  c->r[4] = 0x80182000u;                                           // 5. SEQ/VAB VRAM build — substrate
  c->r[31] = 0x80045228u;                                          //    leaf; parks in its VAB poll
  rec_dispatch(c, 0x800754F4u);
  c->mem_w32(0x1F80022Cu, c->r[2]);                                // v0 = work-area end
  c->mem_w8(0x1F80019Bu, 1);                                       // done_flag -> task-0 wait exits
  c->r[31] = 0x80045244u;
  c->game->pcSched.selfClose();                                    // FUN_80051FB4 — does not return on a task
  c->r[31] = c->mem_r32(sp + 24);
  c->r[17] = c->mem_r32(sp + 20);
  c->r[16] = c->mem_r32(sp + 16);
  c->r[29] += 32;
}

// Task-1 body — FAITHFUL FUN_800452C0 (the walkable-field AREA-DATA loader), run on a PcScheduler
// native fiber. Spawned by submode1Faithful case 0: 0x80044BD4(0x800452C0, area, 0, 2) — the spawn
// latches p3=0 -> sm[0x6D], p2=area -> sm[0x6E] on the slot-1 task struct. Guest-stack discipline:
// frame descent 32 with ra/s1/s0 spills at +24/+20/+16 (live caller values), r16/r17 carried along
// the shard's flow so deeper callees' spills match core B byte-for-byte. Every un-owned leaf is the
// substrate dispatch at its RE'd jal site; owned leaves (0x80044F58 texgroup) run the native mirror.
// Byte shape: generated gen_func_800452C0; RE: scratch/decomp/800452C0.c (Ghidra 2026-07-07).
void Asset::areaDataLoadAsTask() {
  Core* c = this->core;
  uint32_t sm = c->mem_r32(0x1F800138u);           // read before the frame descent (gen order)
  c->r[29] -= 32;
  const uint32_t sp = c->r[29];
  c->mem_w32(sp + 24, c->r[31]);
  c->mem_w32(sp + 20, c->r[17]);
  c->mem_w32(sp + 16, c->r[16]);
  if (c->mem_r8(sm + 0x6D) == 0) {
    c->r[16] = 1;
    uint32_t set = c->mem_r8(sm + 0x6E);
    uint32_t mask = 1u << (set & 31);
    // SAME-AREA fast path: last-loaded set matches and the texgroup-bias mask bit is unchanged.
    if ((uint32_t)c->mem_r8(0x1F8001FFu) == set &&
        (c->mem_r16(0x800BFE56u) & mask) == (c->mem_r16(0x1F800278u) & mask)) {
      loadDescriptorChunk(((uint32_t)c->mem_r16(0x800BF89Eu) & 15u) << 1, 47);  // FUN_80045258
      c->mem_w8(0x1F800206u, 0);
      c->mem_w8(0x1F80019Bu, 1);                   // done_flag -> spawn-and-wait exits
      c->r[31] = 0x80045350u;
      rec_dispatch(c, 0x80051FB4u);                // terminal task end — does not return on a task
    }
  }
  c->r[31] = 0x80045358u;
  rec_dispatch(c, 0x8001CF2Cu);                    // kill the CD/load task (settle slot 2)
  while (c->mem_r16(0x801FE0E0u) != 0) {           // drain: wait for the slot-2 task to finish
    c->r[4] = 1;
    c->r[31] = 0x80045374u;
    rec_dispatch(c, 0x80051F80u);                  // yield — parks the fiber one frame
  }
  c->r[16] = 0x800C0000u;
  c->r[17] = 0x800BF870u;
  sm = c->mem_r32(0x1F800138u);
  c->mem_w8(sm + 0x6D, 2);                         // mode = 2 (texgroup loader takes the bias path)
  sm = c->mem_r32(0x1F800138u);
  c->mem_w16(0x1F800278u, c->mem_r16(0x800BFE56u));// latch the bias mask for the fast-path compare
  c->mem_w8(0x1F8001FFu, c->mem_r8(sm + 0x6E));    // latch the loaded set
  c->mem_w8(0x800BF872u, c->mem_r8(sm + 0x6E));
  uint32_t area = c->mem_r8(sm + 0x6E);
  c->r[4] = 0x80108F9Cu;                           // area filename/descriptor table (GAME.BIN)
  c->r[5] = (area + 3) & 0xFFu;
  c->mem_w8(0x800BF870u, (uint8_t)area);
  c->r[31] = 0x800453DCu;
  rec_dispatch(c, 0x80045080u);                    // area descriptor load
  c->r[4] = c->mem_r8(0x800BF870u);
  c->r[5] = c->mem_r32(0x1F80022Cu);
  c->r[31] = 0x800453F0u;
  rec_dispatch(c, 0x8007566Cu);                    // per-area audio/vab select
  c->r[31] = 0x800453F8u;
  loadTexgroup();                                  // 0x80044F58 — native mirror (direct call, mode 2)
  c->r[4] = 0x8018A000u;                           // DAT payload staging buffer
  c->r[16] = 0x800EF478u;                          // s0 = header base (live for callee spills)
  c->r[5] = c->mem_r32(0x800BE100u);
  uint32_t lo = c->mem_r32(0x800EF480u);
  c->r[6] = c->mem_r32(0x800EF484u) - lo;
  c->r[7] = lo >> 11;
  c->r[5] += c->r[7];
  c->mem_w32(0x800A3EC8u, lo >> 11);
  c->r[31] = 0x80045430u;
  rec_dispatch(c, 0x8001DC40u);                    // CD read: DAT payload -> 0x8018A000
  if (c->mem_r8(0x800BF89Cu) == 2) {
    c->r[4] = 0;
    c->r[31] = 0x80045448u;
    rec_dispatch(c, 0x80045558u);
  }
  loadDescriptorChunk(((uint32_t)c->mem_r16(0x800BF89Eu) & 15u) << 1, 47);  // FUN_80045258
  {                                                // relocation table -> module pointer table
    uint32_t p = c->r[16] + 20;                    // 0x800EF48C
    int32_t n = (int32_t)c->mem_r32(c->r[16] + 16);
    c->r[16] = (uint32_t)n;
    for (int32_t i = 0; i < n; i++) {
      uint32_t word = c->mem_r32(p + (uint32_t)i * 4);
      c->mem_w32(0x800ECF58u + (word >> 24) * 4, (word & 0x00FFFFFFu) + 0x8018A000u);
    }
  }
  if (c->mem_r8(0x800BF870u) == 8) {               // AREA 8: extra texgroup by sub-area band
    c->mem_w32(0x800ECF94u, c->mem_r32(0x800ECF94u) + 0x11000u);
    uint32_t sub = c->mem_r8(0x800BF871u);
    uint32_t g;
    if (sub < 9)       g = 34;
    else if (sub < 16) g = 38;
    else if (sub >= 21) g = 40;
    else               g = 36;
    c->r[16] = g;
    loadDescriptorChunk(g, 8);  // FUN_80045258
    c->mem_w8(0x800BFE60u, (uint8_t)g);
  }
  c->mem_w8(0x1F800206u, 1);
  c->mem_w8(0x1F80019Bu, 1);                       // done_flag -> spawn-and-wait exits
  c->r[31] = 0x80045544u;
  rec_dispatch(c, 0x80051FB4u);                    // terminal task end — does not return on a task
  c->r[31] = c->mem_r32(sp + 24);
  c->r[17] = c->mem_r32(sp + 20);
  c->r[16] = c->mem_r32(sp + 16);
  c->r[29] += 32;
}

// loadDescriptorChunk(descIdx, slot): FAITHFUL FUN_80045258 — a leaf indexed-chunk CD reader.
//   FUN_8001DC40((&DAT_800ECF58)[slot], DAT_800BE100 + ((&DAT_800FB170)[descIdx] >> 11),
//                (&DAT_800FB170)[descIdx+1] - (&DAT_800FB170)[descIdx]).
// Byte shape: ram_f1000_all.c:24254 (Ghidra, GAME project). Both tables are word-indexed
// (4 bytes/entry): 0x800FB170 = per-chunk byte-offset descriptor table (sector = offset>>11,
// size = next-offset - offset); 0x800ECF58 = the module/dest-pointer table this leaf reads FROM
// (already latched — e.g. slot 47/0x2f is the collision-grid buffer, slot 8 the area-8 extra
// texgroup buffer — NOT the table areaDataLoadAsTask's reloc loop writes moments later on the
// general path).
void Asset::loadDescriptorChunk(uint32_t descIdx, uint32_t slot) {   // FUN_80045258
  Core* c = this->core;
  uint32_t dest = c->mem_r32(0x800ECF58u + slot * 4u);
  uint32_t off0 = c->mem_r32(0x800FB170u + descIdx * 4u);
  uint32_t off1 = c->mem_r32(0x800FB170u + (descIdx + 1u) * 4u);
  c->game->cd.dc40Sync(dest, c->mem_r32(0x800BE100u) + (off0 >> 11), off1 - off0);
}

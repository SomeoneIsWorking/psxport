// engine/asset.cpp — PC-native ASSET LOADING subsystem.
// The engine owns asset loading (per CLAUDE.md THE BOUNDARY): the texture-group loader orchestration,
// the LZ image decompressor, and the CPU->VRAM upload. The CD read + the task terminal-yield stay the
// retained platform/content mechanism (called via rec_dispatch, not transcribed). Extracted verbatim
// from game_tomba2.cpp (one behavior, byte-identical) into its own module for PC-game code structure.
#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include "asset.h"
#include "guest_call.h" // rc1-4 guest-call helpers (used by the preload_* chain below)
void rec_dispatch(Core*, uint32_t);
// gpu_native_load_image is declared in core.h (the native CPU->VRAM upload).

// PC-owned LZ image decompressor — replaces recompiled FUN_80044D8C (0x80044D8C). This routine
// rebuilds the per-frame CLUTs (0x801FCDC0) and sprite/texture data from compressed area assets.
// It was the source of the gameplay 2D-sprite corruption: the SAME function gave correct output
// when recompiled but ZEROS when flat-interpreted by the coroutine interpreter (rec_coro_run) at
// runtime — a recompiler-vs-interpreter divergence. A pure decompressor belongs to the PC side,
// so we own it natively here (one implementation, reached identically from both engines).
//
// ABI (matches the MIPS at 0x80044D8C, verified by disassembly):
//   a0=descriptor, a1=dest, a2=src, a3=srclen. Returns v0 = bytes written.
//   Setup: build 8 back-ref offsets from the static table at 0x800153C8, scaled by the per-call
//   stride at desc+4:  offset[i] = base[i] + 2*(factor[i]*stride)  (2D image predictors: mode 1 =
//   previous byte, modes 2-7 = previous-row neighbours; row pitch = stride).
//   Stream of control bytes: len=ctrl>>3, mode=ctrl&7.  mode==0 -> literal copy `len` bytes from
//   src (ctrl byte 0 / len 0 terminates).  mode!=0 -> back-ref copy `len` bytes from dest+offset
//   [mode], BYTE-granular so overlapping copies replicate (RLE), exactly as the original loop.
#define LZ_OFFTAB_BASE 0x800153C8u
static uint32_t lz_decompress(Core* c, uint32_t desc, uint32_t dst, uint32_t src0, uint32_t srclen) {
  const uint32_t src_end = src0 + srclen;
  const int32_t stride = (int16_t)c->mem_r16(desc + 4);
  int32_t offtab[8];
  for (int i = 0; i < 8; i++) {
    const int32_t base   = (int32_t)c->mem_r32(LZ_OFFTAB_BASE + i * 8 + 0);
    const int32_t factor = (int32_t)c->mem_r32(LZ_OFFTAB_BASE + i * 8 + 4);
    offtab[i] = base + 2 * (factor * stride);
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
  return out - dst;                                   // total bytes written
}
void ov_lz_decompress(Core* c) {
  c->r[2] = lz_decompress(c, c->r[4], c->r[5], c->r[6], c->r[7]);
}

// PC-owned texture-group unpacker — replaces recompiled FUN_80044E84 (0x80044E84). Verified by
// disassembly: a0 = descriptor table base, a1 = scratch-end anchor (0x1FD000). Layout: [count:4]
// then [pad:4] then `count` 12-byte entries each { stride:2(@+4 from entry head), field:2(@+6),
// srclen:4(@+8) }; source data starts 0x800 after the table base and advances by srclen per entry.
// For each entry: dst = anchor - 2*stride*field (outputs stack ending at the anchor — transient
// scratch), decompress the entry's image there, then upload it (FUN_80081218) and run the post
// step (FUN_80080f6c). Non-gameplay (asset unpack) → PC-owned, calling the native decompressor
// directly; the two gfx-library sub-calls still route through the recomp/dispatch for now.
void rec_dispatch(Core*, uint32_t);
void ov_unpack_group(Core* c) {
  const uint32_t table = c->r[4], anchor = c->r[5];
  const int32_t count = (int32_t)c->mem_r32(table);
  uint32_t entry = table + 4;            // first 12-byte descriptor entry
  uint32_t src = table + 0x800;          // packed source data follows the table
  int dbg = cfg_dbg("unpack") != 0;
  if (dbg) fprintf(stderr, "[unpack] table=0x%08X count=%d src0=0x%08X ra=0x%08X\n",
                   table, count, src, c->r[31]);
  // PSXPORT_UNPACKDUMP=dir — dump the LIVE compressed input (table + 0x30000 bytes) the moment the
  // unpacker reads it, sequence-numbered, so it can be checked against the disc / oracle exactly.
  { const char* dd = cfg_str("PSXPORT_UNPACKDUMP");
    if (dd) { static int seq = 0; char p[300]; snprintf(p, sizeof p, "%s/unpack_%03d_c%d.bin", dd, seq++, count);
      FILE* uf = fopen(p, "wb"); if (uf) {
        // Dump from the staging base to the end of RAM (archives can be up to ~0x76000 from 0x8018A000).
        uint32_t off = table & 0x1FFFFF, len = 0x200000u - off;
        fwrite(&c->ram[off], 1, len, uf); fclose(uf);
        fprintf(stderr, "[unpack] dumped live input -> %s (table=0x%08X count=%d)\n", p, table, count); } } }
  for (int32_t i = 0; i < count; i++) {
    const uint32_t desc   = entry;
    const int32_t  stride = (int16_t)c->mem_r16(desc + 4);
    const int32_t  field  = (int16_t)c->mem_r16(desc + 6);
    const uint32_t srclen = c->mem_r32(desc + 8);
    const uint32_t dst    = anchor - (uint32_t)(2 * stride * field);
    if (dbg) fprintf(stderr, "[unpack]  e%d dst=(%d,%d) %dx%d src=0x%08X len=%u srcbytes:"
                     " %02X %02X %02X %02X\n", i, (int16_t)c->mem_r16(desc), (int16_t)c->mem_r16(desc+2),
                     stride, field, src, srclen, c->mem_r8(src), c->mem_r8(src+1), c->mem_r8(src+2), c->mem_r8(src+3));
    lz_decompress(c, desc, dst, src, srclen);            // native decompress into transient scratch
    src   += srclen;
    entry += 12;
    c->r[4] = desc; c->r[5] = dst; ov_upload_image(c);   // FUN_80081218(desc, dst): native VRAM upload
    //   (direct C call — the override table that routed 0x80081218 here is gone; top-down PC-driven)
    // Per-image post-step FUN_80080f6c(0) = libgs GPU DrawSync between uploads — meaningless for our
    // synchronous native upload (no async DMA to drain). Owned as a skip; see note above ov_upload_image.
    if (cfg_dbg("unpacksync")) { c->r[4] = 0; rec_dispatch(c, 0x80080F6Cu); }
  }
}

// PC-native TEXTURE-GROUP LOADER — owns the asset-load ORCHESTRATION FUN_80044F58 (0x80044F58): the
// per-group loader a level uses to stream a texture set into VRAM. RE (tools/disas.py + gen_func_80044F58):
// the current task selects a set via task[0x6D]=mode / task[0x6E]=set, then
//   1. CD-load a 2KB HEADER from sector (filebase0 = *0x800BE0F0) + set  [+ a 4/26 bias in mode 2] -> 0x800EF478
//   2. CD-load the compressed ARCHIVE from sector (filebase1 = *0x800BE0F8) + (hdr[0]>>11), len hdr[1]-hdr[0]
//      -> the fixed staging buffer 0x8018A000 (descriptor table in its first 0x800 bytes, packed data after)
//   3. UNPACK the archive (ov_unpack_group) -> decompress + upload each image to its VRAM (x,y) (owned)
//   4. copy a 42-word metadata table from hdr+0x100 (0x800EF578) -> 0x800FB170 (per-set sprite/CLUT meta the
//      game content reads back)
//   5. (mode==0 only) set _DAT_1f80019b=1 and run the task TERMINAL YIELD FUN_80051FB4 (suspend until next
//      frame — this is what streams the groups one-per-frame).
// ENGINE asset orchestration -> reimplemented PC-native; the CD read (ov_cd_loadfile, via 0x8001DC40) and the
// terminal task yield (the scheduler) stay the retained platform/content mechanism (called, not transcribed).
// The mode/set inputs and the 0x800FB170 metadata are CONTENT-interface state, so this is gated on the main-
// RAM A/B diff (later-177). For mode==0 the terminal yield does not return (ov_switch longjmps mid-game),
// exactly like eng_stage_transition's tail; there is no code after it.
void ov_load_texgroup(Core* c) {
  uint32_t ra = c->r[31], sp = c->r[29], s0 = c->r[16];   // preserve for the (non-yield) epilogue
  uint32_t task = c->mem_r32(0x1F800138u);
  uint32_t mode = c->mem_r8(task + 0x6Du);
  uint32_t set  = c->mem_r8(task + 0x6Eu);
  uint32_t hdr_sector = c->mem_r32(0x800BE0F0u) + set;             // filebase0 + set
  if (mode == 2) {                                                 // mode-2 per-set 4/26-sector bias
    uint16_t mask = c->mem_r16(0x800BFE56u);
    hdr_sector += ((mask >> (set & 31)) & 1) ? 26u : 4u;
  }
  const uint32_t HDR = 0x800EF478u;
  c->r[4] = HDR; c->r[5] = hdr_sector; c->r[6] = 2048;
  rec_dispatch(c, 0x8001DC40u);                                    // 1. CD-load 2KB header (platform)
  uint32_t h0 = c->mem_r32(HDR + 0), h1 = c->mem_r32(HDR + 4);
  c->r[4] = 0x8018A000u; c->r[5] = c->mem_r32(0x800BE0F8u) + (h0 >> 11); c->r[6] = h1 - h0;
  rec_dispatch(c, 0x8001DC40u);                                    // 2. CD-load compressed archive -> staging
  c->r[4] = 0x8018A000u; c->r[5] = 0x1FD000u;
  ov_unpack_group(c);                                              // 3. unpack -> decompress + VRAM upload (owned)
  for (uint32_t i = 0; i < 42; i++)                                // 4. per-set metadata table
    c->mem_w32(0x800FB170u + i * 4, c->mem_r32(HDR + 0x100u + i * 4));
  c->r[16] = s0; c->r[29] = sp; c->r[31] = ra;
  if (mode == 0) {                                                 // 5. terminal yield (streams one group/frame)
    c->mem_w8(0x1F80019Bu, 1);
    rec_dispatch(c, 0x80051FB4u);                                  // ov_switch tail — does not return mid-game
  }
}

// Per-image post-step FUN_80080f6c(0) = the game's libgs frame DrawSync: FUN_80083364(0) waits for the
// GPU's ordering-table DMA to drain and the GPU to go idle (polls GPUSTAT @0x800a5ab4 bit 0x01000000 /
// @0x800a5aa8 bit 0x04000000), having queued the (now-empty, since ov_upload_image bypasses it) ring.
// IMPORTANT: this same function is the MAIN-LOOP per-frame DrawSync, so it must NOT be globally overridden
// (a global no-op stalls all presentation). Inside the unpack loop, however, it is a between-uploads GPU
// sync that is meaningless for our SYNCHRONOUS native VRAM upload — there is no async DMA to wait on — so
// the unpack loop owns it as a skip. A/B-verified VRAM+RAM-identical across a full menu+seaside load
// (later-177): the unpack call site only ever passes a0=0 with an empty ring. DIAG: PSXPORT_DEBUG=unpacksync
// restores the per-image super-call to prove equivalence.

// PC-native CPU->VRAM upload — replaces the game's libgs-style upload library FUN_80081218
// (0x80081218). RE (verified empirically vs the A0 upload log, later-62/63): a0 = descriptor
// { x:s16@0, y:s16@2, w:s16@4, h:s16@6 }, a1 = source pixel data (w*h contiguous 16-bit pixels,
// row-major). The recomp body ENQUEUES an entry into the GsSortObject ring at 0x800A5AC8 (head/
// tail @0x800A5AC8/5ACC) which is DMA'd to the GPU later as a 0xA0 packet. It is the SINGLE
// chokepoint for BOTH the scene-load texture atlas (256x256/192x256/… into the texpages the
// characters sample) AND every per-frame 16x1 CLUT — 5300+ calls per attract run. The user's
// directive: the GPU library must be PC-native, not a faithful recomp. So we write the rect
// straight into native VRAM here and DO NOT enqueue (the later ring flush/sync then no-ops over
// an empty ring). Ordering is preserved: the upload still happens before this frame's draws are
// processed, and CLUTs are double-buffered across frames (parity-alternated slots), so no draw
// reads a slot mid-overwrite.
void ov_upload_image(Core* c) {
  const uint32_t desc = c->r[4], src = c->r[5];
  const int x = (int16_t)c->mem_r16(desc + 0), y = (int16_t)c->mem_r16(desc + 2);
  const int w = (int16_t)c->mem_r16(desc + 4), h = (int16_t)c->mem_r16(desc + 6);
  if (w > 0 && h > 0) gpu_native_load_image(c, x, y, w, h, src);
}

// ===== Stage-0/stage-1 area/asset PRELOAD — PC-native + SYNCHRONOUS (moved from native_boot.cpp,
// 2026-07 restructure). Reuses the existing leaf natives above (cd_loadfile_native for the sync CD
// reads, ov_unpack_group for decompress+VRAM upload) — same content-loading domain as the texgroup
// loader.

void cd_loadfile_native(Core* c, uint32_t dest, uint32_t lba, uint32_t size);  // cd_override.cpp (sync 0x8001DB8C/DC40)

// FUN_80044F58 texture-group load, synchronous. (Mirrors engine/asset.cpp ov_load_texgroup but driven
// by explicit (mode,set) — no task-1 spawn, no terminal yield.) Header sector -> archive -> unpack ->
// copy the 42-word per-set metadata table the still-recomp content reads back.
void preload_texgroup(Core* c, uint32_t mode, uint32_t set) {
  uint32_t hdr_sector = c->mem_r32(0x800BE0F0u) + set;             // filebase0 + set
  if (mode == 2) {                                                 // mode-2 per-set 4/26-sector bias
    uint16_t mask = (uint16_t)c->mem_r16(0x800BFE56u);
    hdr_sector += ((mask >> (set & 31)) & 1) ? 26u : 4u;
  }
  cd_loadfile_native(c, 0x800EF478u, hdr_sector, 2048);           // 1. 2KB header
  uint32_t h0 = c->mem_r32(0x800EF478u), h1 = c->mem_r32(0x800EF47Cu);
  cd_loadfile_native(c, 0x8018A000u, c->mem_r32(0x800BE0F8u) + (h0 >> 11), h1 - h0);  // 2. compressed archive
  c->r[4] = 0x8018A000u; c->r[5] = 0x1FD000u; ov_unpack_group(c); // 3. decompress + VRAM upload (native)
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
  // Drain the BAV upload queue SYNCHRONOUSLY so this cel's VAB bank reaches SPU (and its slot frees so the
  // NEXT cel can allocate). The real FUN_800753d4 polls 0x80096a40, whose 0x800993a0 sync busy-waits on the
  // upload's DMA-complete IRQ event — which never fires in this no-IRQ preload, so the original code dropped
  // the poll and silently skipped a whole VAB bank (proved by the dual-core SPU-DMA diff: 1 transfer vs PSX's
  // 2). Deliver the sound-DMA-complete event first so 0x800993a0 returns immediately (no busy-wait, no yield),
  // then run the sync once. (later: in-game music VAB.)
  void hle_deliver_event(Core* c, uint32_t ev_class, uint32_t spec);
  hle_deliver_event(c, 0xF0000009u, 0xFFFFFFFFu);                // sound/DMA-complete event
  rc1(c, 0x80096a40u, 0);                                        // FUN_80096a40(0): upload sync (now non-blocking)
}

// FUN_800754F4 cel/sprite VRAM build, synchronous. FUN_800753ac is itself an async CD read -> use the
// sync loadfile; the two FUN_800753d4 cel-loads go through preload_cel; the ten FUN_80075448
// sprite-cell registrations carry no CD/async wait, so run as recomp. `base` = work base 0x80182000.
static void preload_build_vram(Core* c, uint32_t base) {
  uint32_t s0 = base + 0x51000u;                                   // descriptor table (filled by the read)
  cd_loadfile_native(c, base, c->mem_r32(0x800BE108u), 0x51800u);  // FUN_800753ac: read SND file (idx3)
  preload_cel(c, 0x800BED84u, base + c->mem_r32(s0 + 0x28), base + c->mem_r32(s0 + 0x30));
  preload_cel(c, 0x800BED82u, base + c->mem_r32(s0 + 0x2c), base + c->mem_r32(s0 + 0x34));
  static const struct { uint32_t off, sz; } cells[10] = {
    {0x0c,14},{0x08,14},{0x04,14},{0x00,14},{0x10,8},{0x14,8},{0x18,8},{0x1c,14},{0x20,14},{0x24,14},
  };
  uint32_t cell_h = (uint32_t)(int32_t)(int16_t)c->mem_r16(0x800BED82u);
  for (int i = 0; i < 10; i++)
    rc4(c, 0x80075448u, (uint32_t)i, base + c->mem_r32(s0 + cells[i].off), cells[i].sz, cell_h);
  c->r[2] = base + 26356;                                          // v0 = base + 0x66f4
}

// FUN_8004514C — the stage-1 callback. SWDATA + DAT load, shared texgroup sub-load, relocation table,
// then the cel/sprite VRAM build.
void preload_stage1(Core* c) {
  cd_loadfile_native(c, 0x80157000u, c->mem_r32(0x800BE110u), c->mem_r32(0x800BE114u));  // SWDATA.BIN
  preload_texgroup(c, 1, 1);                                       // shared texgroup sub-load
  uint32_t lo = c->mem_r32(0x800EF480u), hi = c->mem_r32(0x800EF484u);
  cd_loadfile_native(c, 0x80158000u, c->mem_r32(0x800BE100u) + (lo >> 11), hi - lo);     // DAT payload
  uint32_t dat_end = (hi - lo) + 0x80158000u;
  c->mem_w32(0x1F800228u, dat_end);
  c->mem_w32(0x800ED014u, dat_end);
  int32_t n = (int32_t)c->mem_r32(0x800EF488u);                    // relocation table (blez -> skip)
  for (int32_t i = 0; i < n; i++) {
    uint32_t word = c->mem_r32(0x800EF48Cu + i * 4);
    c->mem_w32(0x800ECF58u + (word >> 24) * 4, (word & 0x00FFFFFFu) + 0x80158000u);
  }
  preload_build_vram(c, 0x80182000u);
  c->mem_w32(0x1F80022Cu, c->r[2]);
}

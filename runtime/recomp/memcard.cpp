// Native MEMORY CARD backend + overrides for the recompiled core.
//
// "No emulation" — same philosophy as the CD (disc.c/cd_override.c): the PSX memory card
// is a serial (SIO) device whose every transfer the game completes by spinning on the SIO
// IRQ-set "transfer done" status. Our runtime raises no IRQs, so those spins would hang.
// Instead the card becomes a real 128 KB file on the host, and the BIOS libcard/libmcrd
// frame primitives are replaced by direct, synchronous file I/O.
//
// HOW TOMBA!2 ACCESSES THE CARD (decomp = scratch/decomp/ram_f1000_all.c):
//   It uses Sony's BIOS libcard/libmcrd B0-vector API — NOT a direct-SIO bit-bang driver
//   (that is the controller/pad path at DAT_1f801040, handled by pad_input.c) and NOT the
//   high-level MemCard file API. The card primitives are bare B0-vector trampolines
//   (`li $t0,0xB0; jr $t0; li $t1,<idx>` — Ghidra renders the body as
//   `(*(code*)&SUB_000000b0)()` because the index lives in $t1):
//     FUN_8009C030  B0:0x4A  InitCard            (wrapped by FUN_8009bb10 = InitCARD)
//     FUN_8009C040  B0:0x4B  StartCard           (wrapped by FUN_8009bb7c = StartCARD)
//     FUN_8009C050  B0:0x4C  _card_info(chan)
//     FUN_8009BAF0  B0:0x4E  _card_read (chan, sector, buf)   <- issue 128B frame read
//     FUN_8009BB00  B0:0x50  _card_chan ()                    <- query active port/channel
//     FUN_8009C600  B0:0x4F  _card_write(chan, sector, buf)   <- issue 128B frame write
//     FUN_8009C610  B0:0x5C  _card_status(chan)               <- poll: bit0 set = complete
//   The 128-byte frame transfer + checksum/retry wrapper is FUN_8009C2B0(chan, frame, buf):
//   it XOR-checksums buf[0..0x7e] into buf[0x7f], _card_read/_card_write the frame, then
//   spins `do { s = _card_status(chan>>4); } while ((s & 1) == 0)` until the (IRQ-set) done
//   bit appears, retrying up to 8 times. FUN_8009C3F4 formats the card (writes the 16 dir
//   frames + the "MC" 0x434D header at frame 0 via FUN_8009C2B0). FUN_8009C620(0)=card init.
//
//   TRANSFER UNIT: a 128-byte FRAME (the card is 128 KB = 1024 frames; sector index passed
//   in $a1). CHANNEL: $a0 encodes the SIO port (0x00 / 0x10); _card_status takes chan>>4
//   (port 0 or 1). BUFFER: a flat 128-byte g_ram buffer in $a2 (e.g. DAT_80106198).
//
// OVERRIDE STRATEGY: replace the three I/O trampolines at the true SIO/IRQ hardware
// boundary so each completes synchronously against the host file. InitCard/StartCard/
// _card_chan/_card_info stay on their existing HLE stubs (they touch no blocking IRQ);
// _card_status always reports "complete" so the spin falls through on its first check.

#include "core.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

#define CARD_FRAME_SIZE  128u
#define CARD_FRAMES      1024u                                  // 16 blocks * 64 frames
#define CARD_SIZE        (CARD_FRAME_SIZE * CARD_FRAMES)        // 131072 = 128 KB
// PSX memory-card filesystem geometry (block 0 = header+directory; blocks 1..15 hold files).
#define CARD_BLOCKS       16u
#define CARD_BLOCK_FRAMES 64u
#define DIR_FREE         0xA0u                                  // directory entry: free block
#define DIR_USED_FIRST   0x51u                                  // directory entry: first block of a file

static FILE* s_card = 0;                 // host-backed 128 KB card image (read+write)
static char  s_card_path[1024] = {0};

// ---- path resolution (mirrors disc.c: env override, else scratch/saves default) ---------
static char* mc_dup_trim(const char* s) {
  while (*s == ' ' || *s == '\t') s++;
  size_t n = strlen(s);
  while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t')) n--;
  char* o = (char*)malloc(n + 1); memcpy(o, s, n); o[n] = 0; return o;
}
static char* mc_env_from_dotenv(const char* key) {
  FILE* f = fopen(".env", "rb");
  if (!f) return 0;
  char line[1024]; char* found = 0; size_t klen = strlen(key);
  while (fgets(line, sizeof line, f)) {
    char* p = line; while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, key, klen) == 0) {
      char* eq = strchr(p, '=');
      if (eq && (size_t)(eq - p) == klen) { found = mc_dup_trim(eq + 1); break; }
    }
  }
  fclose(f);
  return found;
}
static char* mc_resolve_path(void) {
  const char* e = cfg_str("PSXPORT_TOMBA2_CARD");
  if (e && *e) return mc_dup_trim(e);
  char* d = mc_env_from_dotenv("PSXPORT_TOMBA2_CARD");
  if (d) return d;
  // Default: per-user save data under the gitignored scratch/ tree (never committed).
  return mc_dup_trim("scratch/saves/tomba2.mcr");
}

// Best-effort `mkdir -p` of the parent directory of `path` (no external deps).
static void mc_mkparents(const char* path) {
  char buf[1024]; size_t n = strlen(path);
  if (n >= sizeof buf) return;
  memcpy(buf, path, n + 1);
  for (char* p = buf + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
#ifdef _WIN32
      _mkdir(buf);
#else
      mkdir(buf, 0777);                  // ignore "already exists"
#endif
      *p = '/';
    }
  }
}

void card_init(void) {
  if (s_card) return;
  char* path = mc_resolve_path();
  snprintf(s_card_path, sizeof s_card_path, "%s", path);
  free(path);
  mc_mkparents(s_card_path);

  // Open existing for read+write; if absent, create zero-filled (a freshly formatted blank
  // card — the game's own format/dir scan handles the empty image and will write its header).
  s_card = fopen(s_card_path, "r+b");
  if (!s_card) {
    s_card = fopen(s_card_path, "w+b");
    if (s_card) {
      static const uint8_t zero[CARD_FRAME_SIZE] = {0};
      for (uint32_t i = 0; i < CARD_FRAMES; i++) fwrite(zero, 1, CARD_FRAME_SIZE, s_card);
      fflush(s_card);
    }
  }
  if (!s_card) {
    fprintf(stderr, "[card] FAILED to open/create card image: %s\n", s_card_path);
    return;
  }
  // Ensure the file is at least full card size (in case of a truncated prior file).
  fseek(s_card, 0, SEEK_END);
  long sz = ftell(s_card);
  if (sz < (long)CARD_SIZE) {
    static const uint8_t zero[CARD_FRAME_SIZE] = {0};
    fseek(s_card, sz, SEEK_SET);
    for (long i = sz; i < (long)CARD_SIZE; i += CARD_FRAME_SIZE)
      fwrite(zero, 1, CARD_FRAME_SIZE, s_card);
    fflush(s_card);
  }

  // FORMAT a blank/unformatted card so the file API can allocate directory blocks immediately.
  // A zero-filled image has dir-entry byte[0]=0x00 (not 0xA0=free), so without this the native
  // open(create) would find no free block and saving would fail. We write the standard PSX layout
  // (frame 0 = "MC" magic; frames 1..15 = free directory entries) exactly like the game's own format
  // routine FUN_8009C3F4 would, but proactively, so a brand-new card image is usable out of the box.
  // (We only format when UNFORMATTED — an existing "MC" card with saves is left untouched.)
  {
    fseek(s_card, 0, SEEK_SET);
    uint8_t hdr[CARD_FRAME_SIZE];
    fread(hdr, 1, CARD_FRAME_SIZE, s_card);
    if (hdr[0] != 'M' || hdr[1] != 'C') {
      uint8_t fr[CARD_FRAME_SIZE];
      // frame 0: "MC" + XOR checksum at byte 0x7F.
      memset(fr, 0, sizeof fr); fr[0] = 'M'; fr[1] = 'C';
      { uint8_t x = 0; for (uint32_t i = 0; i < 0x7F; i++) x ^= fr[i]; fr[0x7F] = x; }
      fseek(s_card, 0, SEEK_SET); fwrite(fr, 1, CARD_FRAME_SIZE, s_card);
      // frames 1..15: free directory entries (state 0xA0, no link, valid checksum).
      memset(fr, 0, sizeof fr); fr[0] = 0xA0; fr[8] = 0xFF; fr[9] = 0xFF;
      { uint8_t x = 0; for (uint32_t i = 0; i < 0x7F; i++) x ^= fr[i]; fr[0x7F] = x; }
      for (uint32_t blk = 1; blk < CARD_BLOCKS; blk++) {
        fseek(s_card, (long)blk * CARD_FRAME_SIZE, SEEK_SET);
        fwrite(fr, 1, CARD_FRAME_SIZE, s_card);
      }
      fflush(s_card);
      fprintf(stderr, "[card] formatted blank card image (MC header + 15 free dir entries)\n");
    }
  }
  fprintf(stderr, "[card] %s (%u frames / 128 KB)\n", s_card_path, CARD_FRAMES);
}

int card_present(void) { return 1; }    // a card is always "inserted"

void card_read_frame(uint32_t frame, uint8_t* out128) {
  if (!s_card) card_init();
  memset(out128, 0, CARD_FRAME_SIZE);
  if (!s_card || frame >= CARD_FRAMES) return;
  fseek(s_card, (long)frame * CARD_FRAME_SIZE, SEEK_SET);
  fread(out128, 1, CARD_FRAME_SIZE, s_card);
}

void card_write_frame(uint32_t frame, const uint8_t* in128) {
  if (!s_card) card_init();
  if (!s_card || frame >= CARD_FRAMES) return;
  fseek(s_card, (long)frame * CARD_FRAME_SIZE, SEEK_SET);
  fwrite(in128, 1, CARD_FRAME_SIZE, s_card);
  fflush(s_card);                        // persist immediately so saves survive a crash
}

// ---- B0-vector overrides (kept out of the standalone test via PSXPORT_CARD_NO_OVERRIDES) --
#ifndef PSXPORT_CARD_NO_OVERRIDES

enum { V0 = 2, A0 = 4, A1 = 5, A2 = 6 };

static int g_card_verbose = 0;           // PSXPORT_CARD_VERBOSE=1

// Deliver the libcard I/O-complete event so a caller waiting on it (TestEvent spin / WaitEvent) wakes.
// Our I/O is synchronous, so completion is "now". EvSpIOE (0x0004) = I/O end. Delivered to the standard
// card event classes (HwCARD/SwCARD); an unmatched class is a harmless no-op (hle_deliver_event only
// fires OPEN events whose class+spec match). Confirmed classes recorded in the journal once captured.
extern void hle_deliver_event(Core* c, uint32_t ev_class, uint32_t spec);
static void card_deliver_complete(Core* c) {
  // RE'd from CRD.BIN (the SAVE/LOAD card menu overlay) + the resident card-event init FUN_8001cc00:
  // the front-end opens these card events (struct base 0x800BF498) and EACH save/load wait loop polls
  // them every frame with TestEvent, treating the FIRST one as SUCCESS and the rest as error/timeout:
  //   +28  SwCARD 0xF4000001 spec 0x8000  -> code 1 = SUCCESS  (what the menu waits for)
  //   +32  SwCARD            spec 0x0100  -> code 2 = error
  //   +36  SwCARD            spec 0x2000  -> code 3 = error (new/unformatted)
  //   +40  HwCARD 0xF0000011 spec 0x0004  -> code 4 = error path too (only +28 is "ok")
  // The "Checking MEMORY CARD" detect flow (A0:0xAB/0xAC) instead polls EvSpIOE (0x0004). Our card I/O
  // is synchronous, so on a completed transfer we signal SUCCESS to BOTH consumers: SwCARD 0x8000 (the
  // save/load success the menu's +28 poll requires) and SwCARD/HwCARD IOE 0x0004 (the detect flow). We
  // deliberately do NOT deliver SwCARD 0x0100/0x2000 (those are the menu's error/new-card codes) nor
  // HwCARD 0x8000, so a real successful host-file write/read can never be misread as an error/timeout.
  hle_deliver_event(c, 0xF4000001u, 0x8000u);   // SwCARD, save/load SUCCESS — the menu's +28 wait
  hle_deliver_event(c, 0xF4000001u, 0x0004u);   // SwCARD, EvSpIOE — card-detect completion
  hle_deliver_event(c, 0xF0000011u, 0x0004u);   // HwCARD, EvSpIOE — BIOS-level completion
}

// A0 libcard table (_card_info / _card_load) — Tomba2's "Checking MEMORY CARD" uses A0:0xAB, not the B0
// path. The card is host-backed and always present/healthy, so accept and deliver the detect/complete
// event so the caller's event-wait falls through immediately (PC-native, zero delay).
int card_hle_a0(uint32_t fn, Core* c) {
  switch (fn) {
    case 0xABu:   // _card_info(port)
    case 0xACu:   // _card_load(slot)
      if (g_card_verbose) fprintf(stderr, "[card] A0:0x%02X(a0=%X a1=%X a2=%X)\n", fn, c->r[A0], c->r[A1], c->r[A2]);
      card_deliver_complete(c); c->r[V0] = 1; return 1;
    default: return 0;
  }
}

// B0:0x4E _card_read(chan, sector, buf): read frame `sector` (128 B) into g_ram[buf].
// Returns 1 (issue accepted) like the BIOS; the actual data is delivered immediately so the
// caller's subsequent _card_status spin completes at once.
static void ov_card_read(Core* c) {
  uint32_t sector = c->r[A1], buf = c->r[A2];
  uint8_t f[CARD_FRAME_SIZE];
  card_read_frame(sector, f);
  for (uint32_t i = 0; i < CARD_FRAME_SIZE; i++) c->mem_w8(buf + i, f[i]);
  if (g_card_verbose) fprintf(stderr, "[card] read  frame %u -> 0x%08X\n", sector, buf);
  c->r[V0] = 1;
}

// B0:0x4F _card_write(chan, sector, buf): write frame `sector` (128 B) from g_ram[buf].
static void ov_card_write(Core* c) {
  uint32_t sector = c->r[A1], buf = c->r[A2];
  uint8_t f[CARD_FRAME_SIZE];
  for (uint32_t i = 0; i < CARD_FRAME_SIZE; i++) f[i] = c->mem_r8(buf + i);
  card_write_frame(sector, f);
  if (g_card_verbose) fprintf(stderr, "[card] write frame %u <- 0x%08X\n", sector, buf);
  c->r[V0] = 1;
}

// B0:0x5C _card_status(chan): the game spins `while((status & 1) == 0)`; since our read/write
// already completed synchronously, always report bit0 set (transfer complete, no error).
static void ov_card_status(Core* c) { c->r[V0] = 1; }

// ===== THE SAVE / LOAD HANG — REAL ROOT CAUSE (RE'd from the CRD.BIN card-menu overlay) ==========
// A previous fix overrode the libcard ASYNC single-frame primitives FUN_8009BBFC/BC8C/BBEC and it did
// NOT fix the hang — because Tomba!2's save/load DOES NOT CALL THEM. Disassembling the actual save/load
// menu overlay (scratch/bin/overlays/CRD.BIN; its jal targets resolve to MAIN.EXE addresses) shows the
// save/load flow uses the BIOS **FILE API** on the memory-card device, exactly like a fopen/fwrite:
//     0x800808B0 = B0:0x32 open("bu00:<name>", mode)     0x800808C0 = B0:0x33 lseek(fd,off,whence)
//     0x800808D0 = B0:0x34 read(fd,buf,len)              0x800808E0 = B0:0x35 write(fd,buf,len)
//     0x800808F0 = B0:0x36 close(fd)                     0x80080900 = B0:0x43 firstfile
//     0x80080910 = B0:0x45 erase(name)                   0x80080840 = B0:0x0B TestEvent (the WAIT)
// SAVE  (CRD 0x801071BC..0x80107228): open(create) -> [poll] -> write(fd,buf,2432) -> close.
// LOAD  (CRD 0x80107924..0x801079D4): open -> lseek(fd,896,0) -> read(fd,buf,1536) -> close.
//
// THE EXACT WAIT (CRD completion loop 0x801080BC..0x8010812C, identical shape in every save/load page):
//   every frame it TestEvents 4 card events stored in the resident card struct 0x800BF498 (opened at
//   boot by FUN_8001CC00) at +28/+32/+36/+40 and treats the FIRST as SUCCESS, the rest as error:
//       +28  SwCARD 0xF4000001 spec 0x8000  -> code 1 = SUCCESS  <-- the only "ok" signal
//       +32  SwCARD            spec 0x0100  -> code 2 = error/timeout
//       +36  SwCARD            spec 0x2000  -> code 3 = error (new card)
//       +40  HwCARD 0xF0000011 spec 0x0004  -> code 4 = error
//   If none fire it bumps a per-frame counter and after 255 frames reports timeout (code 2 = error).
//
// WHY IT HANGS in the no-IRQ HLE: the BIOS file API is unimplemented for card fds. B0:0x35 (write) in
// hle.cpp only handles stdout/stderr (fd 1/2) and returns len without touching the card; B0:0x32/0x33/
// 0x34/0x36/0x43/0x45 are not handled at all. Nothing ever performs the card I/O and — critically —
// nothing ever delivers the SwCARD-0x8000 SUCCESS event the menu polls. So TestEvent(+28) stays 0, the
// loop counts to 255 and reports a timeout/error forever: the save never completes (and never persists).
//
// FIX (PC-native, no IRQ): implement the BIOS card FILE API natively, backed by the host card file
// (card_read_frame/card_write_frame), with a real PSX card directory, and deliver card_deliver_complete
// (which now signals SwCARD-0x8000 SUCCESS) on each completed read/write. We override the seven BIOS
// file-API trampolines directly (resident MAIN.EXE addresses — the same technique threads.cpp uses for
// the OpenThread/CloseThread trampolines at 0x80080860/70/80), so we never touch hle.cpp and the save
// completes on the menu's very next frame poll. LOAD is handled symmetrically by the read override.
//
// ---- PSX memory-card filesystem (128 KB = 16 blocks x 8 KB; block 0 = header+directory) ----------
//   frame 0       : "MC" magic header.
//   frames 1..15  : directory, one 128-byte entry per data block 1..15:
//       +0  state  : 0x51 = used (first block of a file), 0x52/0x53 = link/last, 0xA0 = free.
//       +4  u32    : file size in bytes.        +8  u16 : next-block link (0xFFFF = none).
//       +10 ASCII  : filename (e.g. "BASLUS-xxxxxSAVE0"), NUL-terminated.
//   A file's data lives in its block's 63 usable frames: data byte offset O within block N maps to
//   frame (N*64 + 1 + O/128) at byte O%128 (frame 0 of each block is reserved by convention; Tomba's
//   files fit in one block and never exceed it, so single-block mapping is sufficient and correct).

// Parse "bu00:NAME" / "bu10:NAME" (and tolerate a bare "NAME"): return the bare filename pointer and
// the SIO port (0 or 1). Reads the guest string from g_ram up to NUL.
static void mc_read_guest_str(Core* c, uint32_t va, char* out, size_t cap) {
  size_t i = 0;
  for (; i + 1 < cap; i++) { uint8_t ch = c->mem_r8(va + (uint32_t)i); out[i] = (char)ch; if (!ch) break; }
  out[i < cap ? i : cap - 1] = 0;
}
static const char* mc_strip_dev(const char* name) {
  // skip a leading "<2 letters><digits>:" device prefix (e.g. "bu00:") if present.
  const char* colon = strchr(name, ':');
  return colon ? colon + 1 : name;
}

// Read/scan the directory: find the block index (1..15) holding the file `name`, or -1 if absent.
static int mc_dir_find(const char* name) {
  const char* want = mc_strip_dev(name);
  for (uint32_t blk = 1; blk < CARD_BLOCKS; blk++) {
    uint8_t e[CARD_FRAME_SIZE];
    card_read_frame(blk, e);                       // dir entry for block `blk` is frame `blk`
    if (e[0] != DIR_USED_FIRST) continue;          // only first-block-of-file entries hold a name
    char fn[0x80]; size_t k = 0;
    for (uint32_t j = 0x0A; j < CARD_FRAME_SIZE && e[j] && k + 1 < sizeof fn; j++) fn[k++] = (char)e[j];
    fn[k] = 0;
    if (strcmp(fn, want) == 0) return (int)blk;
  }
  return -1;
}

// Allocate a free directory block, write its entry (name + size), return the block index or -1 (full).
static int mc_dir_create(const char* name, uint32_t size) {
  const char* bare = mc_strip_dev(name);
  for (uint32_t blk = 1; blk < CARD_BLOCKS; blk++) {
    uint8_t e[CARD_FRAME_SIZE];
    card_read_frame(blk, e);
    if (e[0] != DIR_FREE) continue;
    memset(e, 0, sizeof e);
    e[0] = DIR_USED_FIRST;
    e[4] = (uint8_t)(size & 0xFF); e[5] = (uint8_t)((size >> 8) & 0xFF);
    e[6] = (uint8_t)((size >> 16) & 0xFF); e[7] = (uint8_t)((size >> 24) & 0xFF);
    e[8] = 0xFF; e[9] = 0xFF;                       // no next-block link (single-block file)
    size_t k = 0;
    for (; bare[k] && (0x0A + k) < CARD_FRAME_SIZE - 1; k++) e[0x0A + k] = (uint8_t)bare[k];
    // checksum byte (frame XOR over 0..0x7E) — match the format routine's convention.
    uint8_t x = 0; for (uint32_t i = 0; i < 0x7F; i++) x ^= e[i]; e[0x7F] = x;
    card_write_frame(blk, e);
    return (int)blk;
  }
  return -1;
}

// ---- BIOS file-descriptor table (native) -------------------------------------------------------
// The BIOS file API hands the game a small fd; we map it to a card block + a byte position. A handful
// of fds suffices (the menu opens one at a time). fds 0/1/2 are the BIOS console device (stdin/out/err);
// card fds are allocated at index >= MC_FD_BASE so they can never collide with the console fds.
struct McFd { int used; int block; uint32_t pos; uint32_t size; };
enum { MC_FD_BASE = 3, MC_FD_MAX = 11 };
static McFd s_fd[MC_FD_MAX];
static int mc_fd_alloc(int block, uint32_t size) {
  for (int i = MC_FD_BASE; i < MC_FD_MAX; i++) if (!s_fd[i].used) {
    s_fd[i].used = 1; s_fd[i].block = block; s_fd[i].pos = 0; s_fd[i].size = size; return i;
  }
  return -1;
}
// Map a file byte offset within `block` to (frame, byte-in-frame). Frame 0 of the block is reserved,
// so data starts at the block's frame 1: global frame = block*64 + 1 + off/128.
static inline uint32_t mc_data_frame(int block, uint32_t off) {
  return (uint32_t)block * CARD_BLOCK_FRAMES + 1u + (off / CARD_FRAME_SIZE);
}

// B0:0x32 open(name, mode): mode bit 0x0200 = create; block count = (mode >> 16) (Tomba uses 1).
static void ov_file_open(Core* c) {
  char name[0x100]; mc_read_guest_str(c, c->r[A0], name, sizeof name);
  uint32_t mode = c->r[A1];
  int blk = mc_dir_find(name);
  if (blk < 0 && (mode & 0x0200u)) {
    uint32_t nblocks = (mode >> 16) & 0xFFFFu; if (!nblocks) nblocks = 1;
    blk = mc_dir_create(name, nblocks * (CARD_BLOCK_FRAMES - 1) * CARD_FRAME_SIZE);
  }
  if (blk < 0) { c->r[V0] = 0xFFFFFFFFu; if (g_card_verbose) fprintf(stderr, "[card] open '%s' mode=%X -> FAIL\n", name, mode); return; }
  // file size from the directory entry.
  uint8_t e[CARD_FRAME_SIZE]; card_read_frame((uint32_t)blk, e);
  uint32_t sz = (uint32_t)e[4] | ((uint32_t)e[5] << 8) | ((uint32_t)e[6] << 16) | ((uint32_t)e[7] << 24);
  int fd = mc_fd_alloc(blk, sz);
  c->r[V0] = (fd < 0) ? 0xFFFFFFFFu : (uint32_t)fd;
  if (g_card_verbose) fprintf(stderr, "[card] open '%s' mode=%X -> fd=%d block=%d size=%u\n", name, mode, fd, blk, sz);
}

// B0:0x33 lseek(fd, off, whence): 0=SET,1=CUR,2=END. Returns new position.
static void ov_file_lseek(Core* c) {
  int fd = (int)c->r[A0]; int32_t off = (int32_t)c->r[A1]; uint32_t whence = c->r[A2];
  if (fd >= 0 && fd <= 2) { c->r[V0] = 0; return; }   // console device fds: lseek is a no-op
  if (fd <= 0 || fd >= MC_FD_MAX || !s_fd[fd].used) { c->r[V0] = 0xFFFFFFFFu; return; }
  uint32_t base = (whence == 1) ? s_fd[fd].pos : (whence == 2) ? s_fd[fd].size : 0u;
  s_fd[fd].pos = base + (uint32_t)off;
  c->r[V0] = s_fd[fd].pos;
  if (g_card_verbose) fprintf(stderr, "[card] lseek fd=%d off=%d whence=%u -> pos=%u\n", fd, off, whence, s_fd[fd].pos);
}

// B0:0x34 read(fd, buf, len): copy `len` bytes from the card file into g_ram[buf]; deliver completion.
static void ov_file_read(Core* c) {
  int fd = (int)c->r[A0]; uint32_t buf = c->r[A1], len = c->r[A2];
  if (fd >= 0 && fd <= 2) { c->r[V0] = 0; return; }   // console device fds: nothing to read
  if (fd <= 0 || fd >= MC_FD_MAX || !s_fd[fd].used) { c->r[V0] = 0xFFFFFFFFu; return; }
  uint8_t fr[CARD_FRAME_SIZE]; uint32_t loaded_frame = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < len; i++) {
    uint32_t off = s_fd[fd].pos + i;
    uint32_t gframe = mc_data_frame(s_fd[fd].block, off), boff = off % CARD_FRAME_SIZE;
    if (gframe != loaded_frame) { card_read_frame(gframe, fr); loaded_frame = gframe; }
    c->mem_w8(buf + i, fr[boff]);
  }
  s_fd[fd].pos += len;
  c->r[V0] = len;
  if (g_card_verbose) fprintf(stderr, "[card] read  fd=%d -> 0x%08X len=%u (pos now %u)\n", fd, buf, len, s_fd[fd].pos);
  card_deliver_complete(c);   // SwCARD-0x8000 SUCCESS — the load menu's per-frame TestEvent(+28)
}

// B0:0x35 write(fd, buf, len): copy `len` bytes from g_ram[buf] into the card file; deliver completion.
static void ov_file_write(Core* c) {
  int fd = (int)c->r[A0]; uint32_t buf = c->r[A1], len = c->r[A2];
  if (fd == 1 || fd == 2) {   // stdout/stderr (BIOS console device) — preserve printf/puts behavior
    for (uint32_t i = 0; i < len; i++) fputc(c->mem_r8(buf + i), stderr);
    c->r[V0] = len; return;
  }
  if (fd <= 0 || fd >= MC_FD_MAX || !s_fd[fd].used) { c->r[V0] = 0xFFFFFFFFu; return; }
  uint8_t fr[CARD_FRAME_SIZE]; uint32_t cur_frame = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < len; i++) {
    uint32_t off = s_fd[fd].pos + i;
    uint32_t gframe = mc_data_frame(s_fd[fd].block, off), boff = off % CARD_FRAME_SIZE;
    if (gframe != cur_frame) {                          // read-modify-write 128-byte frames
      if (cur_frame != 0xFFFFFFFFu) card_write_frame(cur_frame, fr);
      card_read_frame(gframe, fr); cur_frame = gframe;
    }
    fr[boff] = c->mem_r8(buf + i);
  }
  if (cur_frame != 0xFFFFFFFFu) card_write_frame(cur_frame, fr);   // flush the last partial frame
  s_fd[fd].pos += len;
  c->r[V0] = len;
  if (g_card_verbose) fprintf(stderr, "[card] write fd=%d <- 0x%08X len=%u (pos now %u)\n", fd, buf, len, s_fd[fd].pos);
  card_deliver_complete(c);   // SwCARD-0x8000 SUCCESS — the save menu's per-frame TestEvent(+28)
}

// B0:0x36 close(fd).
static void ov_file_close(Core* c) {
  int fd = (int)c->r[A0];
  if (fd >= 0 && fd <= 2) { c->r[V0] = (uint32_t)fd; return; }   // console fds: nothing to free
  if (fd >= MC_FD_BASE && fd < MC_FD_MAX) s_fd[fd].used = 0;
  c->r[V0] = (fd >= MC_FD_BASE && fd < MC_FD_MAX) ? (uint32_t)fd : 0xFFFFFFFFu;
  if (g_card_verbose) fprintf(stderr, "[card] close fd=%d\n", fd);
}

// B0:0x45 erase(name): mark the file's directory block free. Returns 1 on success.
static void ov_file_erase(Core* c) {
  char name[0x100]; mc_read_guest_str(c, c->r[A0], name, sizeof name);
  int blk = mc_dir_find(name);
  if (blk < 0) { c->r[V0] = 0; if (g_card_verbose) fprintf(stderr, "[card] erase '%s' -> not found\n", name); return; }
  uint8_t e[CARD_FRAME_SIZE]; memset(e, 0, sizeof e);
  e[0] = DIR_FREE; e[8] = 0xFF; e[9] = 0xFF;
  uint8_t x = 0; for (uint32_t i = 0; i < 0x7F; i++) x ^= e[i]; e[0x7F] = x;
  card_write_frame((uint32_t)blk, e);
  c->r[V0] = 1;
  if (g_card_verbose) fprintf(stderr, "[card] erase '%s' (block %d) -> ok\n", name, blk);
}

// B0:0x43 firstfile(name_pattern, dir_entry_out): the menu uses this to enumerate save slots. Returns
// a pointer to a populated DIRENTRY (and v0!=0) for the first match, else 0. Tomba opens the slots by
// name directly (the 7-slot open loop), so a faithful firstfile is not strictly required here; report
// "no more files" (v0=0) which the menu's open-by-name path handles gracefully. Kept as an explicit
// override so the B0:0x43 trampoline never falls through to the unhandled HLE default.
static void ov_file_firstfile(Core* c) { c->r[V0] = 0; }

// B0:0x4C _card_info(chan): the BIOS issues a "get card info" and delivers a completion event; the
// card is always present + healthy here (host-backed), so accept and deliver completion immediately.
static void ov_card_info(Core* c) { card_deliver_complete(c); c->r[V0] = 1; }

// B0 libcard dispatch — these run through the HLE B0 vector (the game's statically-linked libcard
// wrappers call B0:idx via the `li t0,0xB0; jr t0` trampoline -> rec_dispatch_miss -> recomp_hle ->
// here). They complete SYNCHRONOUSLY against the host file (PC-native, zero delay) and deliver the
// libcard completion event so the caller's event-wait / status-spin falls through at once. Returns 1
// if handled. (The old rec_set_override on BIOS addresses 0x8009xxxx was DEAD — those addresses are
// never executed in this pure-HLE-BIOS build; the trampoline funnels everything through recomp_hle.)
int card_hle_b0(uint32_t fn, Core* c) {
  switch (fn) {
    case 0x4Cu: case 0x4Eu: case 0x4Fu: case 0x50u: case 0x5Cu:
      if (g_card_verbose) fprintf(stderr, "[card] B0:0x%02X(a0=%X a1=%X a2=%X)\n", fn, c->r[A0], c->r[A1], c->r[A2]);
      break;
    default: return 0;
  }
  switch (fn) {
    case 0x4Cu: ov_card_info(c);   return 1;   // _card_info(chan)
    case 0x4Eu: ov_card_read(c);   card_deliver_complete(c); return 1;   // _card_read(chan,sector,buf)
    case 0x4Fu: ov_card_write(c);  card_deliver_complete(c); return 1;   // _card_write(chan,sector,buf)
    case 0x50u: c->r[V0] = 0;      return 1;   // _card_chan() -> active channel (single card = 0)
    case 0x5Cu: ov_card_status(c); return 1;   // _card_status(chan)
    default: return 0;
  }
}

void card_overrides_init(void) {
  if (cfg_dbg("card")) g_card_verbose = 1;
  card_init();
  // The low-level libcard B0 frame indices (_card_read/write/status/info, used by the FORMAT path
  // FUN_8009C2B0/C3F4 and the card-CHECK) are serviced in the HLE B0 dispatch (recomp_hle ->
  // card_hle_b0); no address overrides needed for those.
  //
  // SAVE/LOAD uses the BIOS FILE API on the card device (open/lseek/read/write/close/erase). Those are
  // statically-linked BIOS trampolines resident in MAIN.EXE text (0x800808xx), so they ARE real
  // overridable addresses — exactly like the OpenThread/CloseThread/ChangeThread trampolines that
  // threads.cpp overrides at 0x80080860/70/80. We back them with the host card file + a native PSX
  // card directory and deliver the SwCARD-0x8000 SUCCESS event on each completed I/O, so the save/load
  // menu's per-frame TestEvent(+28) wait falls through and the operation completes (and persists).
  // (The prior overrides on the ASYNC libcard primitives FUN_8009BBFC/BC8C/BBEC were REMOVED: the
  // save/load flow never calls them — verified by disassembling the CRD.BIN card-menu overlay.)
}

#endif // PSXPORT_CARD_NO_OVERRIDES

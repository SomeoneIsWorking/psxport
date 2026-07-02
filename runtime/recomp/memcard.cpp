// Native MEMORY CARD backend + overrides for the recompiled core.
//
// "No emulation" — same philosophy as the CD (disc.c/cd_override.c): the PSX memory card is a serial
// (SIO) device whose every transfer the game completes by spinning on the SIO IRQ-set "transfer done"
// status. Our runtime raises no IRQs, so those spins would hang. Instead the card becomes a real 128 KB
// file on the host, and the BIOS libcard/libmcrd frame primitives are replaced by direct, synchronous
// file I/O.
//
// HOW TOMBA!2 ACCESSES THE CARD (decomp = scratch/decomp/ram_f1000_all.c):
//   Sony's BIOS libcard/libmcrd B0-vector API. See the historical commentary in git-blame from before
//   the class refactor for the full RE (frame layouts, save/load menu wait loops, the SwCARD 0x8000
//   SUCCESS signal). The card state (host file + FD table + directory helpers) lives on `class Memcard`
//   in memcard.h; BIOS dispatch entries (card_hle_a0/b0, card_overrides_init) route through it.

#include "memcard.h"
#include "core.h"
#include "game.h"     // c->game->hle.deliverEvent — Hle subsystem lives on Game
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

// ---- Memcard: singleton --------------------------------------------------------------------------

Memcard& Memcard::instance() { static Memcard s; return s; }

// path resolution: env override > .env > default (scratch/saves/tomba2.mcr).
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
char* Memcard::resolvePath() {
  const char* e = cfg_str("PSXPORT_TOMBA2_CARD");
  if (e && *e) return mc_dup_trim(e);
  char* d = mc_env_from_dotenv("PSXPORT_TOMBA2_CARD");
  if (d) return d;
  return mc_dup_trim("scratch/saves/tomba2.mcr");
}

void Memcard::mkParents(const char* path) {
  char buf[1024]; size_t n = strlen(path);
  if (n >= sizeof buf) return;
  memcpy(buf, path, n + 1);
  for (char* p = buf + 1; *p; p++) {
    if (*p == '/') {
      *p = 0;
#ifdef _WIN32
      _mkdir(buf);
#else
      mkdir(buf, 0777);
#endif
      *p = '/';
    }
  }
}

void Memcard::init() {
  if (mCard) return;
  char* path = resolvePath();
  snprintf(mPath, sizeof mPath, "%s", path);
  free(path);
  mkParents(mPath);

  // Open existing for read+write; if absent, create zero-filled (a freshly formatted blank card).
  mCard = fopen(mPath, "r+b");
  if (!mCard) {
    mCard = fopen(mPath, "w+b");
    if (mCard) {
      static const uint8_t zero[kFrameSize] = {0};
      for (uint32_t i = 0; i < kFrames; i++) fwrite(zero, 1, kFrameSize, mCard);
      fflush(mCard);
    }
  }
  if (!mCard) {
    fprintf(stderr, "[card] FAILED to open/create card image: %s\n", mPath);
    return;
  }
  // Ensure the file is at least full card size (in case of a truncated prior file).
  fseek(mCard, 0, SEEK_END);
  long sz = ftell(mCard);
  if (sz < (long)kSize) {
    static const uint8_t zero[kFrameSize] = {0};
    fseek(mCard, sz, SEEK_SET);
    for (long i = sz; i < (long)kSize; i += kFrameSize)
      fwrite(zero, 1, kFrameSize, mCard);
    fflush(mCard);
  }

  // FORMAT a blank/unformatted card so the file API can allocate directory blocks immediately.
  // A zero-filled image has dir-entry byte[0]=0x00 (not 0xA0=free), so without this the native
  // open(create) would find no free block and saving would fail. Standard PSX layout: frame 0 = "MC"
  // magic; frames 1..15 = free directory entries. Only formats when UNFORMATTED.
  {
    fseek(mCard, 0, SEEK_SET);
    uint8_t hdr[kFrameSize];
    fread(hdr, 1, kFrameSize, mCard);
    if (hdr[0] != 'M' || hdr[1] != 'C') {
      uint8_t fr[kFrameSize];
      // frame 0: "MC" + XOR checksum at byte 0x7F.
      memset(fr, 0, sizeof fr); fr[0] = 'M'; fr[1] = 'C';
      { uint8_t x = 0; for (uint32_t i = 0; i < 0x7F; i++) x ^= fr[i]; fr[0x7F] = x; }
      fseek(mCard, 0, SEEK_SET); fwrite(fr, 1, kFrameSize, mCard);
      // frames 1..15: free directory entries.
      memset(fr, 0, sizeof fr); fr[0] = kDirFree; fr[8] = 0xFF; fr[9] = 0xFF;
      { uint8_t x = 0; for (uint32_t i = 0; i < 0x7F; i++) x ^= fr[i]; fr[0x7F] = x; }
      for (uint32_t blk = 1; blk < kBlocks; blk++) {
        fseek(mCard, (long)blk * kFrameSize, SEEK_SET);
        fwrite(fr, 1, kFrameSize, mCard);
      }
      fflush(mCard);
      fprintf(stderr, "[card] formatted blank card image (MC header + 15 free dir entries)\n");
    }
  }
  fprintf(stderr, "[card] %s (%u frames / 128 KB)\n", mPath, kFrames);
}

void Memcard::readFrame(uint32_t frame, uint8_t* out128) {
  if (!mCard) init();
  memset(out128, 0, kFrameSize);
  if (!mCard || frame >= kFrames) return;
  fseek(mCard, (long)frame * kFrameSize, SEEK_SET);
  fread(out128, 1, kFrameSize, mCard);
}

void Memcard::writeFrame(uint32_t frame, const uint8_t* in128) {
  if (!mCard) init();
  if (!mCard || frame >= kFrames) return;
  fseek(mCard, (long)frame * kFrameSize, SEEK_SET);
  fwrite(in128, 1, kFrameSize, mCard);
  fflush(mCard);
}

// ---- PSX card-filesystem: directory helpers ------------------------------------------------------

static const char* mc_strip_dev(const char* name) {
  const char* colon = strchr(name, ':');
  return colon ? colon + 1 : name;
}

int Memcard::dirFind(const char* name) {
  const char* want = mc_strip_dev(name);
  for (uint32_t blk = 1; blk < kBlocks; blk++) {
    uint8_t e[kFrameSize];
    readFrame(blk, e);
    if (e[0] != kDirUsedFirst) continue;
    char fn[0x80]; size_t k = 0;
    for (uint32_t j = 0x0A; j < kFrameSize && e[j] && k + 1 < sizeof fn; j++) fn[k++] = (char)e[j];
    fn[k] = 0;
    if (strcmp(fn, want) == 0) return (int)blk;
  }
  return -1;
}

int Memcard::dirCreate(const char* name, uint32_t size) {
  const char* bare = mc_strip_dev(name);
  for (uint32_t blk = 1; blk < kBlocks; blk++) {
    uint8_t e[kFrameSize];
    readFrame(blk, e);
    if (e[0] != kDirFree) continue;
    memset(e, 0, sizeof e);
    e[0] = kDirUsedFirst;
    e[4] = (uint8_t)(size & 0xFF); e[5] = (uint8_t)((size >> 8) & 0xFF);
    e[6] = (uint8_t)((size >> 16) & 0xFF); e[7] = (uint8_t)((size >> 24) & 0xFF);
    e[8] = 0xFF; e[9] = 0xFF;                                    // no next-block link
    size_t k = 0;
    for (; bare[k] && (0x0A + k) < kFrameSize - 1; k++) e[0x0A + k] = (uint8_t)bare[k];
    uint8_t x = 0; for (uint32_t i = 0; i < 0x7F; i++) x ^= e[i]; e[0x7F] = x;
    writeFrame(blk, e);
    return (int)blk;
  }
  return -1;
}

int Memcard::fdAlloc(int block, uint32_t size) {
  for (int i = kFdBase; i < kFdMax; i++) if (!mFd[i].used) {
    mFd[i].used = 1; mFd[i].block = block; mFd[i].pos = 0; mFd[i].size = size; return i;
  }
  return -1;
}

// ---- BIOS dispatch handlers (call into Memcard::instance) ----------------------------------------
#ifndef PSXPORT_CARD_NO_OVERRIDES

enum { V0 = 2, A0 = 4, A1 = 5, A2 = 6 };

// Frame 0 of each block is reserved by convention; data starts at frame 1 of the block.
static inline uint32_t mc_data_frame(int block, uint32_t off) {
  return (uint32_t)block * Memcard::kBlockFrames + 1u + (off / Memcard::kFrameSize);
}

static void mc_read_guest_str(Core* c, uint32_t va, char* out, size_t cap) {
  size_t i = 0;
  for (; i + 1 < cap; i++) { uint8_t ch = c->mem_r8(va + (uint32_t)i); out[i] = (char)ch; if (!ch) break; }
  out[i < cap ? i : cap - 1] = 0;
}

// Deliver the libcard I/O-complete event so callers waiting on TestEvent (SwCARD-0x8000 SUCCESS +
// EvSpIOE for save/load and card-detect flows) fall through immediately.
void Memcard::deliverComplete(Core* c) {
  c->game->hle.deliverEvent(0xF4000001u, 0x8000u);   // SwCARD save/load SUCCESS
  c->game->hle.deliverEvent(0xF4000001u, 0x0004u);   // SwCARD card-detect completion
  c->game->hle.deliverEvent(0xF0000011u, 0x0004u);   // HwCARD BIOS-level completion
}

// B0:0x4E _card_read(chan, sector, buf).
static void card_read(Core* c) {
  Memcard& m = Memcard::instance();
  uint32_t sector = c->r[A1], buf = c->r[A2];
  uint8_t f[Memcard::kFrameSize];
  m.readFrame(sector, f);
  for (uint32_t i = 0; i < Memcard::kFrameSize; i++) c->mem_w8(buf + i, f[i]);
  if (m.verbose()) fprintf(stderr, "[card] read  frame %u -> 0x%08X\n", sector, buf);
  c->r[V0] = 1;
}

// B0:0x4F _card_write(chan, sector, buf).
static void card_write(Core* c) {
  Memcard& m = Memcard::instance();
  uint32_t sector = c->r[A1], buf = c->r[A2];
  uint8_t f[Memcard::kFrameSize];
  for (uint32_t i = 0; i < Memcard::kFrameSize; i++) f[i] = c->mem_r8(buf + i);
  m.writeFrame(sector, f);
  if (m.verbose()) fprintf(stderr, "[card] write frame %u <- 0x%08X\n", sector, buf);
  c->r[V0] = 1;
}

// B0:0x5C _card_status(chan): always transfer-complete (our I/O is synchronous).
static void card_status(Core* c) { c->r[V0] = 1; }

// B0:0x32 open(name, mode). mode bit 0x0200 = create; block count = (mode >> 16) (Tomba uses 1).
static void file_open(Core* c) {
  Memcard& m = Memcard::instance();
  char name[0x100]; mc_read_guest_str(c, c->r[A0], name, sizeof name);
  uint32_t mode = c->r[A1];
  int blk = m.dirFind(name);
  if (blk < 0 && (mode & 0x0200u)) {
    uint32_t nblocks = (mode >> 16) & 0xFFFFu; if (!nblocks) nblocks = 1;
    blk = m.dirCreate(name, nblocks * (Memcard::kBlockFrames - 1) * Memcard::kFrameSize);
  }
  if (blk < 0) { c->r[V0] = 0xFFFFFFFFu; if (m.verbose()) fprintf(stderr, "[card] open '%s' mode=%X -> FAIL\n", name, mode); return; }
  uint8_t e[Memcard::kFrameSize]; m.readFrame((uint32_t)blk, e);
  uint32_t sz = (uint32_t)e[4] | ((uint32_t)e[5] << 8) | ((uint32_t)e[6] << 16) | ((uint32_t)e[7] << 24);
  int fd = m.fdAlloc(blk, sz);
  c->r[V0] = (fd < 0) ? 0xFFFFFFFFu : (uint32_t)fd;
  if (m.verbose()) fprintf(stderr, "[card] open '%s' mode=%X -> fd=%d block=%d size=%u\n", name, mode, fd, blk, sz);
}

// B0:0x33 lseek(fd, off, whence).
static void file_lseek(Core* c) {
  Memcard& m = Memcard::instance();
  int fd = (int)c->r[A0]; int32_t off = (int32_t)c->r[A1]; uint32_t whence = c->r[A2];
  if (fd >= 0 && fd <= 2) { c->r[V0] = 0; return; }              // console device fds: no-op
  auto* f = m.fdAt(fd);
  if (!f) { c->r[V0] = 0xFFFFFFFFu; return; }
  uint32_t base = (whence == 1) ? f->pos : (whence == 2) ? f->size : 0u;
  f->pos = base + (uint32_t)off;
  c->r[V0] = f->pos;
  if (m.verbose()) fprintf(stderr, "[card] lseek fd=%d off=%d whence=%u -> pos=%u\n", fd, off, whence, f->pos);
}

// B0:0x34 read(fd, buf, len).
static void file_read(Core* c) {
  Memcard& m = Memcard::instance();
  int fd = (int)c->r[A0]; uint32_t buf = c->r[A1], len = c->r[A2];
  if (fd >= 0 && fd <= 2) { c->r[V0] = 0; return; }
  auto* f = m.fdAt(fd);
  if (!f) { c->r[V0] = 0xFFFFFFFFu; return; }
  uint8_t fr[Memcard::kFrameSize]; uint32_t loaded_frame = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < len; i++) {
    uint32_t off = f->pos + i;
    uint32_t gframe = mc_data_frame(f->block, off), boff = off % Memcard::kFrameSize;
    if (gframe != loaded_frame) { m.readFrame(gframe, fr); loaded_frame = gframe; }
    c->mem_w8(buf + i, fr[boff]);
  }
  f->pos += len;
  c->r[V0] = len;
  if (m.verbose()) fprintf(stderr, "[card] read  fd=%d -> 0x%08X len=%u (pos now %u)\n", fd, buf, len, f->pos);
  Memcard::deliverComplete(c);
}

// B0:0x35 write(fd, buf, len).
static void file_write(Core* c) {
  Memcard& m = Memcard::instance();
  int fd = (int)c->r[A0]; uint32_t buf = c->r[A1], len = c->r[A2];
  if (fd == 1 || fd == 2) {                                       // stdout/stderr
    for (uint32_t i = 0; i < len; i++) fputc(c->mem_r8(buf + i), stderr);
    c->r[V0] = len; return;
  }
  auto* f = m.fdAt(fd);
  if (!f) { c->r[V0] = 0xFFFFFFFFu; return; }
  uint8_t fr[Memcard::kFrameSize]; uint32_t cur_frame = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < len; i++) {
    uint32_t off = f->pos + i;
    uint32_t gframe = mc_data_frame(f->block, off), boff = off % Memcard::kFrameSize;
    if (gframe != cur_frame) {                                    // read-modify-write frames
      if (cur_frame != 0xFFFFFFFFu) m.writeFrame(cur_frame, fr);
      m.readFrame(gframe, fr); cur_frame = gframe;
    }
    fr[boff] = c->mem_r8(buf + i);
  }
  if (cur_frame != 0xFFFFFFFFu) m.writeFrame(cur_frame, fr);      // flush the last partial frame
  f->pos += len;
  c->r[V0] = len;
  if (m.verbose()) fprintf(stderr, "[card] write fd=%d <- 0x%08X len=%u (pos now %u)\n", fd, buf, len, f->pos);
  Memcard::deliverComplete(c);
}

// B0:0x36 close(fd).
static void file_close(Core* c) {
  Memcard& m = Memcard::instance();
  int fd = (int)c->r[A0];
  if (fd >= 0 && fd <= 2) { c->r[V0] = (uint32_t)fd; return; }
  bool ok = m.fdValid(fd);
  if (ok) m.fdFree(fd);
  c->r[V0] = ok ? (uint32_t)fd : 0xFFFFFFFFu;
  if (m.verbose()) fprintf(stderr, "[card] close fd=%d\n", fd);
}

// B0:0x45 erase(name).
static void file_erase(Core* c) {
  Memcard& m = Memcard::instance();
  char name[0x100]; mc_read_guest_str(c, c->r[A0], name, sizeof name);
  int blk = m.dirFind(name);
  if (blk < 0) { c->r[V0] = 0; if (m.verbose()) fprintf(stderr, "[card] erase '%s' -> not found\n", name); return; }
  uint8_t e[Memcard::kFrameSize]; memset(e, 0, sizeof e);
  e[0] = Memcard::kDirFree; e[8] = 0xFF; e[9] = 0xFF;
  uint8_t x = 0; for (uint32_t i = 0; i < 0x7F; i++) x ^= e[i]; e[0x7F] = x;
  m.writeFrame((uint32_t)blk, e);
  c->r[V0] = 1;
  if (m.verbose()) fprintf(stderr, "[card] erase '%s' (block %d) -> ok\n", name, blk);
}

// B0:0x43 firstfile — the menu opens slots by name directly, so "no more files" is sufficient.
static void file_firstfile(Core* c) { c->r[V0] = 0; }

// B0:0x4C _card_info(chan): host-backed card is always healthy/present.
static void card_info(Core* c) { Memcard::deliverComplete(c); c->r[V0] = 1; }

// ---- Public BIOS dispatch entries (called from `class Hle::dispatchBios`) -----------------------

// A0 libcard: _card_info / _card_load. Tomba2's "Checking MEMORY CARD" uses A0:0xAB.
extern "C" int card_hle_a0(uint32_t fn, Core* c) {
  Memcard& m = Memcard::instance();
  switch (fn) {
    case 0xABu:   // _card_info(port)
    case 0xACu:   // _card_load(slot)
      if (m.verbose()) fprintf(stderr, "[card] A0:0x%02X(a0=%X a1=%X a2=%X)\n", fn, c->r[A0], c->r[A1], c->r[A2]);
      Memcard::deliverComplete(c); c->r[V0] = 1; return 1;
    default: return 0;
  }
}

// B0 libcard: _card_info/_card_read/_card_write/_card_chan/_card_status + the file API used by the
// save/load menu (open/lseek/read/write/close/erase/firstfile).
extern "C" int card_hle_b0(uint32_t fn, Core* c) {
  Memcard& m = Memcard::instance();
  switch (fn) {
    case 0x4Cu: case 0x4Eu: case 0x4Fu: case 0x50u: case 0x5Cu:
      if (m.verbose()) fprintf(stderr, "[card] B0:0x%02X(a0=%X a1=%X a2=%X)\n", fn, c->r[A0], c->r[A1], c->r[A2]);
      break;
    default: break;
  }
  switch (fn) {
    case 0x32u: file_open(c);      return 1;                     // open(name, mode)
    case 0x33u: file_lseek(c);     return 1;                     // lseek(fd, off, whence)
    case 0x34u: file_read(c);      return 1;                     // read(fd, buf, len)
    case 0x35u: file_write(c);     return 1;                     // write(fd, buf, len)
    case 0x36u: file_close(c);     return 1;                     // close(fd)
    case 0x43u: file_firstfile(c); return 1;                     // firstfile(pattern, dirent)
    case 0x45u: file_erase(c);     return 1;                     // erase(name)
    case 0x4Cu: card_info(c);      return 1;                     // _card_info(chan)
    case 0x4Eu: card_read(c);      Memcard::deliverComplete(c); return 1;  // _card_read
    case 0x4Fu: card_write(c);     Memcard::deliverComplete(c); return 1;  // _card_write
    case 0x50u: c->r[V0] = 0;      return 1;                     // _card_chan() -> 0
    case 0x5Cu: card_status(c);    return 1;                     // _card_status(chan)
    default: return 0;
  }
}

void card_overrides_init(void) {
  Memcard& m = Memcard::instance();
  if (cfg_dbg("card")) m.setVerbose(true);
  m.init();
  // Low-level libcard B0 frame primitives + BIOS file API dispatch through card_hle_b0 above;
  // no address overrides needed.
}

#endif // PSXPORT_CARD_NO_OVERRIDES

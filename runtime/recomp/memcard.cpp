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
#include <lucent/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#else
#include <sys/stat.h>
#include <sys/types.h>
#endif

// class Memcard is a Game member (see memcard.h). All BIOS handlers below reach it via `c->game->memcard`.

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
// The consuming game supplies its own env key and default path (GameConfig::cardEnvVar /
// cardDefaultPath). This used to hardcode the FIRST consumer's key and filename, which meant a
// second consumer's saves silently went to the reference game's card file — the same defect the disc
// resolver had (WART-06 in the Spider-Man port). Order: the game's key, then the generic
// PSXPORT_CARD, each from the environment then .env, then the game's default path.
char* Memcard::resolvePath(const GameConfig* cfg) {
  const char* keys[2] = { cfg ? cfg->cardEnvVar : nullptr, "PSXPORT_CARD" };
  for (int i = 0; i < 2; i++) {
    if (!keys[i] || !*keys[i]) continue;
    const char* e = cfg_str(keys[i]);
    if (e && *e) return mc_dup_trim(e);
    char* d = mc_env_from_dotenv(keys[i]);
    if (d) return d;
  }
  if (cfg && cfg->cardDefaultPath && *cfg->cardDefaultPath) return mc_dup_trim(cfg->cardDefaultPath);
  return mc_dup_trim("scratch/saves/card.mcr");
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
  char* path = resolvePath(game ? game->core.cfg : nullptr);
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
    lucent::error("card", "FAILED to open/create card image: {}", mPath);
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
      lucent::info("card", "formatted blank card image (MC header + 15 free dir entries)");
    }
  }
  lucent::info("card", "{} ({} frames / 128 KB)", mPath, kFrames);
}

// A frame that cannot be moved returns FALSE and says why once. It used to return silently — zeros
// for a read, nothing at all for a write — which is the "silently-skipped input" failure: the file
// API above could not tell a completed transfer from one that never happened, so it announced
// success either way. The caller is what turns this into the card ERROR event the guest reads.
bool Memcard::readFrame(uint32_t frame, uint8_t* out128) {
  if (!mCard) init();
  memset(out128, 0, kFrameSize);
  if (!mCard) { lucent::error("card", "read frame {}: no card image is open ({})", frame, mPath); return false; }
  if (frame >= kFrames) {
    lucent::error("card", "read frame {} is past the end of a {}-frame card — refusing", frame, kFrames);
    return false;
  }
  fseek(mCard, (long)frame * kFrameSize, SEEK_SET);
  return fread(out128, 1, kFrameSize, mCard) == kFrameSize;
}

bool Memcard::writeFrame(uint32_t frame, const uint8_t* in128) {
  if (!mCard) init();
  if (!mCard) { lucent::error("card", "write frame {}: no card image is open ({})", frame, mPath); return false; }
  if (frame >= kFrames) {
    lucent::error("card", "write frame {} is past the end of a {}-frame card — refusing", frame, kFrames);
    return false;
  }
  fseek(mCard, (long)frame * kFrameSize, SEEK_SET);
  const bool ok = fwrite(in128, 1, kFrameSize, mCard) == kFrameSize;
  fflush(mCard);
  return ok;
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

// Shell-glob match for a card filename: `*` any run, `?` any one character. Sony's BIOS matches the
// same two, and the browser only ever asks for "*" — but a pattern the matcher silently ignored
// would enumerate the whole card and look exactly like a correct answer, so it is implemented.
static bool mc_glob(const char* pat, const char* s) {
  const char *star = nullptr, *sAtStar = nullptr;
  while (*s) {
    if (*pat == '?' || *pat == *s) { pat++; s++; continue; }
    if (*pat == '*') { star = pat++; sAtStar = s; continue; }
    if (!star) return false;
    pat = star + 1; s = ++sAtStar;
  }
  while (*pat == '*') pat++;
  return *pat == 0;
}

void Memcard::dirScanBegin(const char* pattern) {
  snprintf(mScanPat, sizeof mScanPat, "%s", pattern);
  mScanBlk = 1;                                   // block 0 is the card header; entries are 1..15
}

bool Memcard::dirScanNext(Core* c, uint32_t direntVa) {
  if (!direntVa) return false;
  for (; mScanBlk < kBlocks; mScanBlk++) {
    uint8_t e[kFrameSize];
    if (!readFrame(mScanBlk, e)) continue;
    if (e[0] != kDirUsedFirst) continue;          // free, or a continuation block of a longer file
    char fn[0x80]; size_t k = 0;
    for (uint32_t j = 0x0A; j < kFrameSize && e[j] && k + 1 < sizeof fn; j++) fn[k++] = (char)e[j];
    fn[k] = 0;
    if (!mc_glob(mScanPat, fn)) continue;
    const uint32_t size = (uint32_t)e[4] | ((uint32_t)e[5] << 8)
                        | ((uint32_t)e[6] << 16) | ((uint32_t)e[7] << 24);
    for (uint32_t i = 0; i < kDirEntBytes; i++) c->mem_w8(direntVa + i, 0);
    for (uint32_t i = 0; i < kDirEntNameLen - 1 && fn[i]; i++) c->mem_w8(direntVa + i, (uint8_t)fn[i]);
    c->mem_w32(direntVa + kDirEntAttr, 0);
    c->mem_w32(direntVa + kDirEntSize, size);
    c->mem_w32(direntVa + kDirEntNext, 0);
    c->mem_w32(direntVa + kDirEntHead, mScanBlk);
    if (mVerbose) lucent::info("card", "dir scan '{}' -> '{}' (block {}, {} bytes)", mScanPat, fn,
                               mScanBlk, size);
    mScanBlk++;
    return true;
  }
  if (mVerbose) lucent::info("card", "dir scan '{}' -> no (further) match; {} directory block(s) "
                                     "examined", mScanPat, kBlocks - 1);
  return false;
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
  // ONLY the I/O-end spec. 0x8000 is EvSpERROR, not success — this used to deliver it as well, on a
  // comment that called it "SwCARD save/load SUCCESS", and that is an announcement of a card FAULT.
  //
  // It is not harmless, and Spider-Man shows exactly how it bites. Its card-status routine at
  // 0x80015300 TestEvents all four opened SwCARD specs IN ORDER and writes a status code each time
  // one fires, with NO early exit:
  //
  //   TestEvent(0x0004) -> status 1   (I/O end)
  //   TestEvent(0x8000) -> status 2   (error)
  //   TestEvent(0x0100) -> status 3
  //   TestEvent(0x2000) -> status 4
  //
  // so a later match OVERWRITES an earlier one. Delivering 0x0004 and 0x8000 together set success and
  // then immediately replaced it with error, and the game sat on "CHECKING MEMORY CARD" forever.
  //   python3 external/psxport/tools/disasm.py <ram.bin> 0x80015300 0x80015370
  //
  // A caller that needs to report a genuine FAILURE should deliver 0x8000 explicitly at that point,
  // not have it ride along with every completion.
  c->game->hle.deliverEvent(0xF4000001u, 0x0004u);   // SwCARD I/O end (EvSpIOE)
  c->game->hle.deliverEvent(0xF0000011u, 0x0004u);   // HwCARD BIOS-level completion
}

// The failure half. THE RETURN VALUE CANNOT CARRY AN ERROR on this API — libmcrd retries the BIOS
// call while it is non-zero (see file_read below) — so a transfer the backend could not perform is
// still ACCEPTED and then completed with the ERROR spec. That is not a convention invented here: the
// guest's next state reads exactly this distinction. Spyro's 0x8006841C sums the four SwCARD flags
// and 0x80068264 maps them to a code, where the error code drives a bounded retry (16) and then an
// access code the game reports to the player.
void Memcard::deliverError(Core* c) {
  c->game->hle.deliverEvent(0xF4000001u, 0x8000u);   // SwCARD error (EvSpERROR)
  c->game->hle.deliverEvent(0xF0000011u, 0x8000u);   // HwCARD error
}

// B0:0x4E _card_read(chan, sector, buf).
static void card_read(Core* c) {
  Memcard& m = c->game->memcard;
  uint32_t sector = c->r[A1], buf = c->r[A2];
  uint8_t f[Memcard::kFrameSize];
  m.readFrame(sector, f);
  for (uint32_t i = 0; i < Memcard::kFrameSize; i++) c->mem_w8(buf + i, f[i]);
  if (m.verbose()) lucent::info("card", "read  frame {} -> 0x{:08X}", sector, buf);
  c->r[V0] = 1;
}

// B0:0x4F _card_write(chan, sector, buf).
static void card_write(Core* c) {
  Memcard& m = c->game->memcard;
  uint32_t sector = c->r[A1], buf = c->r[A2];
  uint8_t f[Memcard::kFrameSize];
  for (uint32_t i = 0; i < Memcard::kFrameSize; i++) f[i] = c->mem_r8(buf + i);
  m.writeFrame(sector, f);
  if (m.verbose()) lucent::info("card", "write frame {} <- 0x{:08X}", sector, buf);
  c->r[V0] = 1;
}

// B0:0x5C _card_status(chan): always transfer-complete (our I/O is synchronous).
static void card_status(Core* c) { c->r[V0] = 1; }

// B0:0x32 open(name, mode). mode bit 0x0200 = create; block count = (mode >> 16) (Tomba uses 1).
static void file_open(Core* c) {
  Memcard& m = c->game->memcard;
  char name[0x100]; mc_read_guest_str(c, c->r[A0], name, sizeof name);
  uint32_t mode = c->r[A1];
  int blk = m.dirFind(name);
  if (blk < 0 && (mode & 0x0200u)) {
    uint32_t nblocks = (mode >> 16) & 0xFFFFu; if (!nblocks) nblocks = 1;
    blk = m.dirCreate(name, nblocks * (Memcard::kBlockFrames - 1) * Memcard::kFrameSize);
  }
  if (blk < 0) { c->r[V0] = 0xFFFFFFFFu; if (m.verbose()) lucent::info("card", "open '{}' mode={:X} -> FAIL", name, mode); return; }
  uint8_t e[Memcard::kFrameSize]; m.readFrame((uint32_t)blk, e);
  uint32_t sz = (uint32_t)e[4] | ((uint32_t)e[5] << 8) | ((uint32_t)e[6] << 16) | ((uint32_t)e[7] << 24);
  int fd = m.fdAlloc(blk, sz);
  c->r[V0] = (fd < 0) ? 0xFFFFFFFFu : (uint32_t)fd;
  if (m.verbose()) lucent::info("card", "open '{}' mode={:X} -> fd={} block={} size={}", name, mode, fd, blk, sz);
}

// B0:0x33 lseek(fd, off, whence).
static void file_lseek(Core* c) {
  Memcard& m = c->game->memcard;
  int fd = (int)c->r[A0]; int32_t off = (int32_t)c->r[A1]; uint32_t whence = c->r[A2];
  if (fd >= 0 && fd <= 2) { c->r[V0] = 0; return; }              // console device fds: no-op
  auto* f = m.fdAt(fd);
  if (!f) { c->r[V0] = 0xFFFFFFFFu; return; }
  uint32_t base = (whence == 1) ? f->pos : (whence == 2) ? f->size : 0u;
  f->pos = base + (uint32_t)off;
  c->r[V0] = f->pos;
  if (m.verbose()) lucent::info("card", "lseek fd={} off={} whence={} -> pos={}", fd, off, whence, f->pos);
}

// B0:0x34 read(fd, buf, len).
//
// ── THE RETURN VALUE IS NOT A BYTE COUNT ──────────────────────────────────────────────────────────
//
// On a memory-card fd this call only STARTS a transfer; completion arrives as a card EVENT, and the
// caller's next act is to wait for it. Sony's stock libmcrd encodes that contract as a retry loop —
// Spyro's read op state machine (generated/shard_0.c gen_func_80066F34 case 0x14) is
//
//     clear_card_events(); do { v0 = read(fd, buf, len); } while (v0 != 0); state = wait_for_event;
//
// and its write op (gen_func_800671F0) is the same shape. So 0 means "accepted", non-zero means
// "busy, ask again", and returning `len` is an unbounded loop in the guest. This ran inside the
// game's own vblank callback, so the whole frame loop stopped: the port presented nothing further
// and the watchdog killed it. That is the user-reported "SELECT MEMORY CARD" softlock (the consuming
// game's docs/issues/0051, claim C161). tests/test_memcard_file_api.cpp pins it.
//
// A FAILED TRANSFER IS REPORTED THROUGH THE EVENT, NOT THE RETURN, for the same reason: any non-zero
// value re-runs the loop. See Memcard::deliverError.
//
// fd 0..2 are the console devices, not the card: they keep the ordinary synchronous file semantics
// and announce nothing.
static void file_read(Core* c) {
  Memcard& m = c->game->memcard;
  int fd = (int)c->r[A0]; uint32_t buf = c->r[A1], len = c->r[A2];
  if (fd >= 0 && fd <= 2) { c->r[V0] = 0; return; }
  auto* f = m.fdAt(fd);
  // A bad descriptor is a BIOS ARGUMENT error — synchronous, -1, and no card event, because no card
  // operation was ever started. libmcrd cannot reach here (it abandons the op when open fails).
  if (!f) {
    lucent::error("card", "read on fd {} which is not an open card file — returning -1", fd);
    c->r[V0] = 0xFFFFFFFFu; return;
  }
  uint8_t fr[Memcard::kFrameSize]; uint32_t loaded_frame = 0xFFFFFFFFu;
  bool ok = true;
  for (uint32_t i = 0; i < len; i++) {
    uint32_t off = f->pos + i;
    uint32_t gframe = mc_data_frame(f->block, off), boff = off % Memcard::kFrameSize;
    if (gframe != loaded_frame) { ok = m.readFrame(gframe, fr) && ok; loaded_frame = gframe; }
    c->mem_w8(buf + i, fr[boff]);
  }
  f->pos += len;
  c->r[V0] = 0;                       // accepted; the result is the event below
  if (m.verbose()) lucent::info("card", "read  fd={} -> 0x{:08X} len={} (pos now {}) {}", fd, buf, len,
                                f->pos, ok ? "-> IOEND" : "-> ERROR");
  if (ok) Memcard::deliverComplete(c);
  else    Memcard::deliverError(c);
}

// B0:0x35 write(fd, buf, len).
static void file_write(Core* c) {
  Memcard& m = c->game->memcard;
  int fd = (int)c->r[A0]; uint32_t buf = c->r[A1], len = c->r[A2];
  if (fd == 1 || fd == 2) {                                       // stdout/stderr
    for (uint32_t i = 0; i < len; i++) fputc(c->mem_r8(buf + i), stderr);
    c->r[V0] = len; return;
  }
  auto* f = m.fdAt(fd);
  if (!f) {                                                       // see file_read: argument error
    lucent::error("card", "write on fd {} which is not an open card file — returning -1", fd);
    c->r[V0] = 0xFFFFFFFFu; return;
  }
  uint8_t fr[Memcard::kFrameSize]; uint32_t cur_frame = 0xFFFFFFFFu;
  bool ok = true;
  for (uint32_t i = 0; i < len; i++) {
    uint32_t off = f->pos + i;
    uint32_t gframe = mc_data_frame(f->block, off), boff = off % Memcard::kFrameSize;
    if (gframe != cur_frame) {                                    // read-modify-write frames
      if (cur_frame != 0xFFFFFFFFu) ok = m.writeFrame(cur_frame, fr) && ok;
      ok = m.readFrame(gframe, fr) && ok; cur_frame = gframe;
    }
    fr[boff] = c->mem_r8(buf + i);
  }
  if (cur_frame != 0xFFFFFFFFu) ok = m.writeFrame(cur_frame, fr) && ok;  // flush the last partial frame
  f->pos += len;
  c->r[V0] = 0;                       // accepted; see file_read for why this is not `len`
  if (m.verbose()) lucent::info("card", "write fd={} <- 0x{:08X} len={} (pos now {}) {}", fd, buf, len,
                                f->pos, ok ? "-> IOEND" : "-> ERROR");
  if (ok) Memcard::deliverComplete(c);
  else    Memcard::deliverError(c);
}

// B0:0x36 close(fd).
static void file_close(Core* c) {
  Memcard& m = c->game->memcard;
  int fd = (int)c->r[A0];
  if (fd >= 0 && fd <= 2) { c->r[V0] = (uint32_t)fd; return; }
  bool ok = m.fdValid(fd);
  if (ok) m.fdFree(fd);
  c->r[V0] = ok ? (uint32_t)fd : 0xFFFFFFFFu;
  if (m.verbose()) lucent::info("card", "close fd={}", fd);
}

// B0:0x45 erase(name).
static void file_erase(Core* c) {
  Memcard& m = c->game->memcard;
  char name[0x100]; mc_read_guest_str(c, c->r[A0], name, sizeof name);
  int blk = m.dirFind(name);
  if (blk < 0) { c->r[V0] = 0; if (m.verbose()) lucent::info("card", "erase '{}' -> not found", name); return; }
  uint8_t e[Memcard::kFrameSize]; memset(e, 0, sizeof e);
  e[0] = Memcard::kDirFree; e[8] = 0xFF; e[9] = 0xFF;
  uint8_t x = 0; for (uint32_t i = 0; i < 0x7F; i++) x ^= e[i]; e[0x7F] = x;
  m.writeFrame((uint32_t)blk, e);
  c->r[V0] = 1;
  if (m.verbose()) lucent::info("card", "erase '{}' (block {}) -> ok", name, blk);
}

// B0:0x42 firstfile(name, dirent) / B0:0x43 nextfile(dirent) — DIRECTORY ENUMERATION.
//
// This is how a save/load browser finds out what is on the card: it cannot open a slot by name until
// it knows the names. B0:0x43 used to be wired to a stub that answered "no more files" — and it was
// wired under the wrong number as well (0x42 is firstfile, 0x43 is nextfile), so firstfile was not
// handled AT ALL: the BIOS-miss path returns without writing $v0 and the guest read a stale register
// as its DIRENTRY pointer. Tomba!2's browser reaches both through FUN_80080940 / FUN_80080900.
//
// `name` is a full device path ("bu00:*"); the pattern is everything after the colon. The device
// PORT digits are deliberately not honoured — this backend is one host card file and open()/erase()
// resolve every port to it, so enumerating per port would report files on a card that open() would
// then read through the other port's name. One card, consistently, until the backend grows a second
// image; that is a backend limitation, not a per-call decision.
static void file_firstfile(Core* c) {
  Memcard& m = c->game->memcard;
  char name[0x100]; mc_read_guest_str(c, c->r[A0], name, sizeof name);
  const uint32_t dirent = c->r[A1];
  m.dirScanBegin(mc_strip_dev(name));
  // Guest code patches the DCB's firstfile slot with its own restore-trampoline before calling here
  // and expects the BIOS to run it; this HLE *is* the device's firstfile, so put the slot back
  // rather than dispatching a pointer with no code behind it. See the DCB note in hle.cpp.
  c->game->hle.deviceUnhook(c->game->hle.deviceFind("bu"));
  c->r[V0] = m.dirScanNext(c, dirent) ? dirent : 0;
  if (m.verbose()) lucent::info("card", "firstfile '{}' dirent=0x{:08X} -> 0x{:08X}", name, dirent,
                                c->r[V0]);
}

static void file_nextfile(Core* c) {
  Memcard& m = c->game->memcard;
  const uint32_t dirent = c->r[A0];
  c->r[V0] = m.dirScanNext(c, dirent) ? dirent : 0;
  if (m.verbose()) lucent::info("card", "nextfile dirent=0x{:08X} -> 0x{:08X}", dirent, c->r[V0]);
}

// B0:0x4C _card_info(chan): host-backed card is always healthy/present.
static void card_info(Core* c) { Memcard::deliverComplete(c); c->r[V0] = 1; }

// ---- Public BIOS dispatch entries (called from `class Hle::dispatchBios`) -----------------------

// A0 libcard: _card_info / _card_load. Tomba2's "Checking MEMORY CARD" uses A0:0xAB.
extern "C" int card_hle_a0(uint32_t fn, Core* c) {
  Memcard& m = c->game->memcard;
  switch (fn) {
    case 0xABu:   // _card_info(port)
    case 0xACu:   // _card_load(slot)
      if (m.verbose()) lucent::info("card", "A0:0x{:02X}(a0={:X} a1={:X} a2={:X})", fn, c->r[A0], c->r[A1], c->r[A2]);
      Memcard::deliverComplete(c); c->r[V0] = 1; return 1;
    default: return 0;
  }
}

// B0 libcard: _card_info/_card_read/_card_write/_card_chan/_card_status + the file API used by the
// save/load menu (open/lseek/read/write/close/erase/firstfile).
extern "C" int card_hle_b0(uint32_t fn, Core* c) {
  Memcard& m = c->game->memcard;
  switch (fn) {
    case 0x4Cu: case 0x4Eu: case 0x4Fu: case 0x50u: case 0x5Cu:
      if (m.verbose()) lucent::info("card", "B0:0x{:02X}(a0={:X} a1={:X} a2={:X})", fn, c->r[A0], c->r[A1], c->r[A2]);
      break;
    default: break;
  }
  switch (fn) {
    case 0x32u: file_open(c);      return 1;                     // open(name, mode)
    case 0x33u: file_lseek(c);     return 1;                     // lseek(fd, off, whence)
    case 0x34u: file_read(c);      return 1;                     // read(fd, buf, len)
    case 0x35u: file_write(c);     return 1;                     // write(fd, buf, len)
    case 0x36u: file_close(c);     return 1;                     // close(fd)
    case 0x42u: file_firstfile(c); return 1;                     // firstfile(path, dirent)
    case 0x43u: file_nextfile(c);  return 1;                     // nextfile(dirent)
    case 0x45u: file_erase(c);     return 1;                     // erase(name)
    case 0x4Cu: card_info(c);      return 1;                     // _card_info(chan)
    case 0x4Eu: card_read(c);      Memcard::deliverComplete(c); return 1;  // _card_read
    case 0x4Fu: card_write(c);     Memcard::deliverComplete(c); return 1;  // _card_write
    case 0x50u: c->r[V0] = 0;      return 1;                     // _card_chan() -> 0
    case 0x5Cu: card_status(c);    return 1;                     // _card_status(chan)
    default: return 0;
  }
}

void card_overrides_init(Game* game) {
  Memcard& m = game->memcard;
  if (lucent::channel_on("card")) m.setVerbose(true);
  m.init();
  // Publish the card as an installed BIOS device. Guest code that resolves a path prefix walks the
  // kernel device table itself instead of calling a BIOS vector (Tomba!2's card browser does, in
  // FUN_80080940), and an unpublished table is not "no devices" — it is a garbage base and length
  // that the walk dereferences. See the DCB note in hle.cpp.
  game->hle.deviceAdd("bu");
  // Low-level libcard B0 frame primitives + BIOS file API dispatch through card_hle_b0 above;
  // no address overrides needed.
}

#endif // PSXPORT_CARD_NO_OVERRIDES

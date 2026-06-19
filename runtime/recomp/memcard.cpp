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
  // Tomba2 uses libmcrd: its "Checking MEMORY CARD" loop TestEvents the SwCARD class (0xF4000001) for
  // completion (captured: it polls specs IOE/NEW/TIMOUT/ERROR every frame). Our I/O is synchronous, so
  // signal SUCCESS via EvSpIOE (0x0004) on SwCARD (and HwCARD for any lower-level waiter). NOT NEW
  // (0x2000) — that would mean "unformatted/new card" and trigger a format prompt.
  hle_deliver_event(c, 0xF4000001u, 0x0004u);   // SwCARD, EvSpIOE — libmcrd completion (the game polls this)
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
  // Card B0 indices are serviced in the HLE B0 dispatch (recomp_hle -> card_hle_b0); no address
  // overrides (those BIOS addresses never run in the pure-HLE build).
}

#endif // PSXPORT_CARD_NO_OVERRIDES

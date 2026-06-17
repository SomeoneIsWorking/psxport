// Native by-LBA disc backend for the recompiled core (S3).
// Reads 2048-byte user data per sector directly from the Tomba!2 CHD via libchdr —
// the same extraction tools/discdump.cpp does, so the bytes match the oracle disc.
// This is the data source the CD overrides (cd_override.c) read from; "no emulation"
// means no CD controller / IRQ handshake — the game's read functions are replaced by
// direct native reads of this image. Pure data path: form/mode-2 audio/video framing
// (XA/STR) is a later front-end concern; data files are mode2/form1 with 2048 user bytes.
#include "r3000.h"
#include "cfg.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libchdr/chd.h>

#define RAW_FRAME 2448u   // 2352 raw + 96 subcode, CHD CD unit
#define USER_DATA 2048u

static chd_file*  s_chd = 0;
static uint32_t   s_frames_per_hunk = 0;
static uint32_t   s_hunk_count = 0;
static uint32_t   s_hunk_bytes = 0;
static uint8_t*   s_hunk_buf = 0;
static uint32_t   s_cached_hunk = 0xFFFFFFFFu;

// Resolve the disc path: PSXPORT_TOMBA2_DISC, then PSXPORT_DISC, then a KEY=VALUE line in
// ./.env (the build/runtime run from the repo root). Mirrors common/env.h's order for the
// keys this port uses; kept minimal (no parent-walk) since boot runs from the repo root.
static char* dup_trim(const char* s) {
  while (*s == ' ' || *s == '\t') s++;
  size_t n = strlen(s);
  while (n && (s[n-1] == '\n' || s[n-1] == '\r' || s[n-1] == ' ' || s[n-1] == '\t')) n--;
  char* o = malloc(n + 1); memcpy(o, s, n); o[n] = 0; return o;
}
static char* env_from_dotenv(const char* key) {
  FILE* f = fopen(".env", "rb");
  if (!f) return 0;
  char line[1024]; char* found = 0; size_t klen = strlen(key);
  while (fgets(line, sizeof line, f)) {
    char* p = line; while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, key, klen) == 0) {
      char* eq = strchr(p, '=');
      if (eq && (size_t)(eq - p) == klen) { found = dup_trim(eq + 1); break; }
    }
  }
  fclose(f);
  return found;
}
static char* resolve_disc_path(void) {
  const char* e = cfg_str("PSXPORT_TOMBA2_DISC");
  if (e && *e) return dup_trim(e);
  e = cfg_str("PSXPORT_DISC");
  if (e && *e) return dup_trim(e);
  char* d = env_from_dotenv("PSXPORT_TOMBA2_DISC");
  if (d) return d;
  return env_from_dotenv("PSXPORT_DISC");
}

int disc_open(void) {
  if (s_chd) return 1;
  char* path = resolve_disc_path();
  if (!path) { fprintf(stderr, "[disc] no disc image (PSXPORT_TOMBA2_DISC/PSXPORT_DISC/.env)\n"); return 0; }
  if (chd_open(path, CHD_OPEN_READ, 0, &s_chd) != CHDERR_NONE) {
    fprintf(stderr, "[disc] failed to open CHD: %s\n", path); free(path); return 0;
  }
  const chd_header* h = chd_get_header(s_chd);
  s_hunk_bytes = h->hunkbytes;
  s_frames_per_hunk = h->hunkbytes / RAW_FRAME;
  s_hunk_count = h->totalhunks;
  s_hunk_buf = malloc(s_hunk_bytes);
  fprintf(stderr, "[disc] opened %s (%u hunks, %u frames/hunk)\n",
          path, s_hunk_count, s_frames_per_hunk);
  free(path);
  return s_frames_per_hunk > 0;
}

// Read one sector's 2048-byte user data. Returns 1 on success.
int disc_read_sector(uint32_t lba, uint8_t* out) {
  if (!s_chd && !disc_open()) return 0;
  uint32_t hunk = lba / s_frames_per_hunk;
  uint32_t off = (lba % s_frames_per_hunk) * RAW_FRAME;
  if (hunk >= s_hunk_count) { fprintf(stderr, "[disc] LBA %u out of range\n", lba); return 0; }
  if (hunk != s_cached_hunk) {
    if (chd_read(s_chd, hunk, s_hunk_buf) != CHDERR_NONE) return 0;
    s_cached_hunk = hunk;
  }
  const uint8_t* raw = s_hunk_buf + off;
  // Mode byte at raw[15]; mode-2 sectors carry an 8-byte subheader before user data.
  uint32_t data_off = (raw[15] == 2) ? 24 : 16;
  memcpy(out, raw + data_off, USER_DATA);
  return 1;
}

// Copy the first `n` bytes of the raw 2352-byte CD sector (sync+header+subheader+data).
// Needed for CD-XA framing: the Mode2 subheader at raw[16..23] (submode raw[18], coding
// raw[19]) distinguishes XA-ADPCM audio (Form2, 2324B) from MDEC video (Form1, 2048B)
// sectors, which disc_read_sector() hides. Returns 1 on success. n is clamped to 2352.
int disc_read_raw(uint32_t lba, uint8_t* out, uint32_t n) {
  if (!s_chd && !disc_open()) return 0;
  if (n > 2352u) n = 2352u;
  uint32_t hunk = lba / s_frames_per_hunk;
  uint32_t off = (lba % s_frames_per_hunk) * RAW_FRAME;
  if (hunk >= s_hunk_count) return 0;
  if (hunk != s_cached_hunk) {
    if (chd_read(s_chd, hunk, s_hunk_buf) != CHDERR_NONE) return 0;
    s_cached_hunk = hunk;
  }
  memcpy(out, s_hunk_buf + off, n);
  return 1;
}

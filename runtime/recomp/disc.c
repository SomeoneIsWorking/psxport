// Native by-LBA disc backend for the recompiled core (S3).
// Reads 2048-byte user data per sector directly from the Tomba!2 CHD via libchdr —
// the same extraction tools/discdump.cpp does, so the bytes match the oracle disc.
// This is the data source the CD overrides (cd_override.c) read from; "no emulation"
// means no CD controller / IRQ handshake — the game's read functions are replaced by
// direct native reads of this image. Pure data path: form/mode-2 audio/video framing
// (XA/STR) is a later front-end concern; data files are mode2/form1 with 2048 user bytes.
#include "r3000.h"
#include "cfg.h"
#include "disc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <libchdr/chd.h>

#define RAW_FRAME 2448u   // 2352 raw + 96 subcode, CHD CD unit
#define USER_DATA 2048u

void disc_state_init(DiscState* d) { memset(d, 0, sizeof *d); d->cached_hunk = 0xFFFFFFFFu; }

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
// Last-resort resolution order: CLI arg (run.sh) > PSXPORT_TOMBA2_DISC/PSXPORT_DISC env > .env >
// a *.chd dropped into the working directory (repo root). The drop-in scan itself is implemented
// in disc_provision.cpp (Fs::findFirstWithExtension, std::filesystem — no hand-rolled dirent).
static char* resolve_disc_path(void) {
  const char* e = cfg_str("PSXPORT_TOMBA2_DISC");
  if (e && *e) return dup_trim(e);
  e = cfg_str("PSXPORT_DISC");
  if (e && *e) return dup_trim(e);
  char* d = env_from_dotenv("PSXPORT_TOMBA2_DISC");
  if (d) return d;
  d = env_from_dotenv("PSXPORT_DISC");
  if (d) return d;
  char buf[512];
  if (disc_dropin_scan(buf, sizeof buf)) return dup_trim(buf);
  return 0;
}

int disc_open(DiscState* d) {
  if (d->chd) return 1;
  char* path = resolve_disc_path();
  if (!path) { cfg_logi("disc", "no disc image (PSXPORT_TOMBA2_DISC/PSXPORT_DISC/.env)"); return 0; }
  if (chd_open(path, CHD_OPEN_READ, 0, &d->chd) != CHDERR_NONE) {
    cfg_loge("disc", "failed to open CHD: %s", path); free(path); return 0;
  }
  const chd_header* h = chd_get_header(d->chd);
  d->hunk_bytes = h->hunkbytes;
  d->frames_per_hunk = h->hunkbytes / RAW_FRAME;
  d->hunk_count = h->totalhunks;
  d->hunk_buf = malloc(d->hunk_bytes);
  cfg_logi("disc", "opened %s (%u hunks, %u frames/hunk)", path, d->hunk_count, d->frames_per_hunk);
  free(path);
  return d->frames_per_hunk > 0;
}

// Read one sector's 2048-byte user data. Returns 1 on success.
int disc_read_sector(DiscState* d, uint32_t lba, uint8_t* out) {
  if (!d->chd && !disc_open(d)) return 0;
  uint32_t hunk = lba / d->frames_per_hunk;
  uint32_t off = (lba % d->frames_per_hunk) * RAW_FRAME;
  if (hunk >= d->hunk_count) { cfg_logi("disc", "LBA %u out of range", lba); return 0; }
  if (hunk != d->cached_hunk) {
    if (chd_read(d->chd, hunk, d->hunk_buf) != CHDERR_NONE) return 0;
    d->cached_hunk = hunk;
  }
  const uint8_t* raw = d->hunk_buf + off;
  // Mode byte at raw[15]; mode-2 sectors carry an 8-byte subheader before user data.
  uint32_t data_off = (raw[15] == 2) ? 24 : 16;
  memcpy(out, raw + data_off, USER_DATA);
  return 1;
}

// Copy the first `n` bytes of the raw 2352-byte CD sector (sync+header+subheader+data).
// Needed for CD-XA framing: the Mode2 subheader at raw[16..23] (submode raw[18], coding
// raw[19]) distinguishes XA-ADPCM audio (Form2, 2324B) from MDEC video (Form1, 2048B)
// sectors, which disc_read_sector() hides. Returns 1 on success. n is clamped to 2352.
int disc_read_raw(DiscState* d, uint32_t lba, uint8_t* out, uint32_t n) {
  if (!d->chd && !disc_open(d)) return 0;
  if (n > 2352u) n = 2352u;
  uint32_t hunk = lba / d->frames_per_hunk;
  uint32_t off = (lba % d->frames_per_hunk) * RAW_FRAME;
  if (hunk >= d->hunk_count) return 0;
  if (hunk != d->cached_hunk) {
    if (chd_read(d->chd, hunk, d->hunk_buf) != CHDERR_NONE) return 0;
    d->cached_hunk = hunk;
  }
  memcpy(out, d->hunk_buf + off, n);
  return 1;
}

static uint32_t rd_le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

// Case-insensitive compare of an ISO9660 directory record name (length `nlen`, no NUL) against a
// path component `want`. The on-disc file name carries a ";version" suffix (e.g. "START.BIN;1");
// directory names do not. Matches `want` against the name with any ";.." suffix stripped.
static int iso_name_eq(const uint8_t* name, int nlen, const char* want) {
  int i = 0;
  for (; i < nlen; i++) {
    char nc = (char)name[i];
    char wc = want[i];
    if (nc == ';') break;                        // on-disc name reached its version suffix
    if (wc == ';' || wc == 0) return 0;          // want ended/versioned before the name did → no match
    char a = (nc >= 'a' && nc <= 'z') ? (char)(nc - 32) : nc;
    char b = (wc >= 'a' && wc <= 'z') ? (char)(wc - 32) : wc;
    if (a != b) return 0;
  }
  return want[i] == 0 || want[i] == ';';         // both ended (want may carry its own ";version")
}

// Resolve an absolute ISO9660 path (PSX backslash style, e.g. "\\BIN\\START.BIN"; '/' also accepted;
// ";version" and case are ignored) to its data LBA and byte size by walking the directory tree from
// the Primary Volume Descriptor. This is the native CdSearchFile replacement for the game's
// FUN_8008b8f0, which did the same ISO directory walk via busy-wait CD reads. Returns 1 on success.
int disc_find_file(DiscState* d, const char* path, uint32_t* out_lba, uint32_t* out_size) {
  if (!d->chd && !disc_open(d)) return 0;
  uint8_t sec[2048];
  // Primary Volume Descriptor at sector 16; its root directory record sits at offset 156 (34 bytes).
  if (!disc_read_sector(d, 16, sec)) return 0;
  if (sec[0] != 1) return 0;                     // PVD type byte
  uint32_t dir_lba  = rd_le32(sec + 156 + 2);    // root extent LBA  (LE form of the 8-byte field)
  uint32_t dir_size = rd_le32(sec + 156 + 10);   // root data length (LE form)

  const char* p = path;
  while (*p == '\\' || *p == '/') p++;           // skip leading separator(s)
  while (*p) {
    // isolate this path component into comp[]
    char comp[64]; int cn = 0;
    while (*p && *p != '\\' && *p != '/' && cn < (int)sizeof(comp) - 1) comp[cn++] = *p++;
    comp[cn] = 0;
    while (*p == '\\' || *p == '/') p++;
    int is_last = (*p == 0);

    // scan the directory's records (records never cross a 2048-byte sector boundary)
    uint32_t nsec = (dir_size + 2047u) / 2048u, found = 0;
    for (uint32_t s = 0; s < nsec && !found; s++) {
      if (!disc_read_sector(d, dir_lba + s, sec)) return 0;
      uint32_t o = 0;
      while (o < 2048u) {
        uint32_t rlen = sec[o];
        if (rlen == 0) break;                    // zero pad → rest of sector is empty
        if (o + rlen > 2048u) break;
        int nlen = sec[o + 32];
        if (iso_name_eq(sec + o + 33, nlen, comp)) {
          uint32_t e_lba  = rd_le32(sec + o + 2);
          uint32_t e_size = rd_le32(sec + o + 10);
          if (is_last) { *out_lba = e_lba; *out_size = e_size; return 1; }
          dir_lba = e_lba; dir_size = e_size; found = 1;
          break;
        }
        o += rlen;
      }
    }
    if (!found) return 0;                         // component not present
  }
  return 0;                                       // path ended on a directory, not a file
}


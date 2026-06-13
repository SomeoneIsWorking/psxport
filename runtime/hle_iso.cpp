// psxport HLE BIOS — ISO9660 lookup + PS-X EXE boot. See hle_bios.h.
//
// All disc reads go through psxport_cd_read_sectors(lba, count, buf), which
// hands back 2048-byte Mode2/Form1 user-data sectors. ISO9660 places the
// Primary Volume Descriptor at filesystem LBA 16; from there we reach the root
// directory and walk its records to resolve a filename to an (LBA, size).

#include "hle_bios.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cctype>

namespace {

// Little-endian field reads from a raw byte buffer (ISO + EXE are LE on PSX).
inline uint32_t rd_le32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

inline uint32_t sectors_for(uint32_t bytes) { return (bytes + 2047u) / 2048u; }

// Compare an ISO directory identifier against `name`, case-insensitively and
// ignoring a trailing ";1" version suffix on either side. ISO stores e.g.
// "SYSTEM.CNF;1"; callers may pass "SYSTEM.CNF" or "SYSTEM.CNF;1".
bool iso_name_match(const char* name, const uint8_t* id, uint32_t id_len) {
  // Trim a ";N" suffix from the ISO identifier.
  uint32_t ilen = id_len;
  for (uint32_t i = 0; i < id_len; ++i) {
    if (id[i] == ';') { ilen = i; break; }
  }
  // Trim a ";N" suffix from the requested name too.
  uint32_t nlen = (uint32_t)strlen(name);
  for (uint32_t i = 0; i < nlen; ++i) {
    if (name[i] == ';') { nlen = i; break; }
  }
  if (ilen != nlen) return false;
  for (uint32_t i = 0; i < ilen; ++i) {
    if (toupper((unsigned char)name[i]) != toupper((unsigned char)id[i])) return false;
  }
  return true;
}

} // namespace

// Find a file by name in the ISO9660 ROOT directory. Returns 1 on match.
int psxport_hle_iso_find(const char* name, uint32_t* out_lba, uint32_t* out_size) {
  if (!name) return 0;

  // Primary Volume Descriptor at LBA 16: type byte == 1, identifier "CD001".
  uint8_t pvd[2048];
  if (psxport_cd_read_sectors(16, 1, pvd) != 1) return 0;
  if (pvd[0] != 1 || memcmp(pvd + 1, "CD001", 5) != 0) return 0;

  // Root directory record is 34 bytes at PVD offset 156. LBA at record+2 (LE),
  // data length at record+10 (LE).
  const uint8_t* root = pvd + 156;
  uint32_t root_lba = rd_le32(root + 2);
  uint32_t root_len = rd_le32(root + 10);
  if (root_len == 0) return 0;

  // Root dir may span multiple 2048-byte sectors; read them all in.
  uint32_t nsec = sectors_for(root_len);
  uint8_t* dir = (uint8_t*)malloc((size_t)nsec * 2048);
  if (!dir) return 0;
  if (psxport_cd_read_sectors((int32_t)root_lba, (int)nsec, dir) != (int)nsec) {
    free(dir);
    return 0;
  }

  // Walk directory records. A record never straddles a sector boundary: a
  // length byte of 0 means "no more records in this sector" — skip to the next
  // sector. record[0]=record length, record[32]=id length, id at record[33].
  int found = 0;
  for (uint32_t off = 0; off + 33 < root_len; ) {
    const uint8_t* rec = dir + off;
    uint8_t rec_len = rec[0];
    if (rec_len == 0) {
      // Advance to next sector boundary.
      uint32_t next = ((off / 2048) + 1) * 2048;
      if (next <= off) break;
      off = next;
      continue;
    }
    uint8_t id_len = rec[32];
    if (off + 33 + id_len <= root_len && iso_name_match(name, rec + 33, id_len)) {
      if (out_lba) *out_lba = rd_le32(rec + 2);
      if (out_size) *out_size = rd_le32(rec + 10);
      found = 1;
      break;
    }
    off += rec_len;
  }

  free(dir);
  return found;
}

namespace {

// Extract the value side of a `KEY = value` line from SYSTEM.CNF text, matching
// KEY case-insensitively. Copies the (whitespace-trimmed) value token into
// `out` (NUL-terminated, capped at out_cap). Returns true if KEY was found.
bool cnf_get(const char* text, size_t len, const char* key, char* out, size_t out_cap) {
  size_t klen = strlen(key);
  size_t i = 0;
  while (i < len) {
    // Start of a logical line.
    size_t line = i;
    while (i < len && text[i] != '\n' && text[i] != '\r') ++i;
    size_t line_end = i;
    while (i < len && (text[i] == '\n' || text[i] == '\r')) ++i;

    // Skip leading whitespace on the line.
    size_t p = line;
    while (p < line_end && (text[p] == ' ' || text[p] == '\t')) ++p;

    // Match KEY (case-insensitive).
    if ((size_t)(line_end - p) < klen) continue;
    bool ok = true;
    for (size_t k = 0; k < klen; ++k) {
      if (toupper((unsigned char)text[p + k]) != toupper((unsigned char)key[k])) { ok = false; break; }
    }
    if (!ok) continue;
    p += klen;

    // Expect '=' after optional whitespace.
    while (p < line_end && (text[p] == ' ' || text[p] == '\t')) ++p;
    if (p >= line_end || text[p] != '=') continue;
    ++p;
    while (p < line_end && (text[p] == ' ' || text[p] == '\t')) ++p;

    // Copy the value up to trailing whitespace.
    size_t v = p;
    while (v < line_end && text[v] != ' ' && text[v] != '\t') ++v;
    size_t n = v - p;
    if (n >= out_cap) n = out_cap - 1;
    memcpy(out, text + p, n);
    out[n] = '\0';
    return true;
  }
  return false;
}

} // namespace

namespace {

// Strip a "cdrom:" / "cdrom0:" device prefix and leading path separators from a
// BOOT/LoadExec path, leaving just the filename (e.g. "MAIN.EXE;1"). Returns a
// pointer into the same buffer.
const char* strip_cdrom_prefix(const char* path) {
  const char* name = path;
  const char* colon = strchr(name, ':');
  if (colon) name = colon + 1;
  while (*name == '\\' || *name == '/') ++name;
  return name;
}

} // namespace

// Load a PS-X EXE from the disc by name into RAM at its T_ADDR, clear its BSS,
// and return its entry context (pc, gp, stack top). Mirrors OpenBIOS
// loadExe()+exec() (kernel/psxexe.{c,s}): read header (60 bytes at file +0x10),
// copy text to text_addr, zero [bss_addr, bss_addr+bss_size), compute the stack
// top as stack_start+stack_size. The actual SP/GP install + PC jump is done by
// the caller (A0:0x51 LoadExec) so it can redirect the live CPU.
int psxport_hle_load_exe(uint8_t* ram, const char* name_in,
                         uint32_t* out_pc, uint32_t* out_gp, uint32_t* out_stack_top) {
  if (!name_in) return 0;
  const char* name = strip_cdrom_prefix(name_in);
  if (*name == '\0') return 0;

  uint32_t exe_lba = 0, exe_size = 0;
  if (!psxport_hle_iso_find(name, &exe_lba, &exe_size)) return 0;
  if (exe_size < 0x800) return 0;

  uint32_t exe_sec = sectors_for(exe_size);
  uint8_t* exe = (uint8_t*)malloc((size_t)exe_sec * 2048);
  if (!exe) return 0;
  if (psxport_cd_read_sectors((int32_t)exe_lba, (int)exe_sec, exe) != (int)exe_sec) {
    free(exe);
    return 0;
  }
  if (memcmp(exe, "PS-X EXE", 8) != 0) {
    free(exe);
    return 0;
  }

  // PS-X EXE header (first 0x800 bytes). LE uint32 fields:
  //   +0x10 PC  +0x14 GP  +0x18 text_addr  +0x1C text_size
  //   +0x28 bss_addr  +0x2C bss_size  +0x30 sp_base  +0x34 sp_offset
  uint32_t pc        = rd_le32(exe + 0x10);
  uint32_t gp        = rd_le32(exe + 0x14);
  uint32_t text_addr = rd_le32(exe + 0x18);
  uint32_t text_size = rd_le32(exe + 0x1C);
  uint32_t bss_addr  = rd_le32(exe + 0x28);
  uint32_t bss_size  = rd_le32(exe + 0x2C);
  uint32_t sp_base   = rd_le32(exe + 0x30);
  uint32_t sp_offset = rd_le32(exe + 0x34);

  if ((uint64_t)0x800 + text_size > (uint64_t)exe_sec * 2048) {
    text_size = (uint32_t)((uint64_t)exe_sec * 2048 - 0x800);
  }
  memcpy(ram + (text_addr & 0x1FFFFF), exe + 0x800, text_size);
  free(exe);

  // Clear BSS (exec()'s clearBSS loop): zero [bss_addr, bss_addr+bss_size).
  if (bss_size) {
    uint32_t b = bss_addr & 0x1FFFFF;
    uint32_t n = bss_size;
    if ((uint64_t)b + n > 0x200000u) n = 0x200000u - b;
    memset(ram + b, 0, n);
  }

  if (out_pc) *out_pc = pc;
  if (out_gp) *out_gp = gp;
  // exec(): $sp = stack_start + stack_size, only if stack_start (header SP) set.
  if (out_stack_top) *out_stack_top = sp_base ? (sp_base + sp_offset) : 0;
  return 1;
}

// Boot: read SYSTEM.CNF, parse BOOT=cdrom:\NAME;1, load NAME's PS-X EXE into RAM
// and return its initial PC/SP/GP. Returns 1 on success.
int psxport_hle_boot(uint8_t* ram, uint32_t* out_pc, uint32_t* out_sp, uint32_t* out_gp) {
  // --- SYSTEM.CNF ---
  uint32_t cnf_lba = 0, cnf_size = 0;
  if (!psxport_hle_iso_find("SYSTEM.CNF", &cnf_lba, &cnf_size)) return 0;
  if (cnf_size == 0) return 0;

  uint32_t cnf_sec = sectors_for(cnf_size);
  uint8_t* cnf = (uint8_t*)malloc((size_t)cnf_sec * 2048);
  if (!cnf) return 0;
  if (psxport_cd_read_sectors((int32_t)cnf_lba, (int)cnf_sec, cnf) != (int)cnf_sec) {
    free(cnf);
    return 0;
  }

  // BOOT = cdrom:\SCUS_944.54;1  (also cdrom0:, with/without leading backslash)
  char boot_val[128];
  if (!cnf_get((const char*)cnf, cnf_size, "BOOT", boot_val, sizeof(boot_val))) {
    free(cnf);
    return 0;
  }
  free(cnf);

  // Strip a "cdrom:" / "cdrom0:" prefix, then any leading backslashes.
  char* name = boot_val;
  const char* colon = strchr(name, ':');
  if (colon) name = (char*)colon + 1;
  while (*name == '\\' || *name == '/') ++name;
  if (*name == '\0') return 0;

  // --- locate + read the EXE ---
  uint32_t exe_lba = 0, exe_size = 0;
  if (!psxport_hle_iso_find(name, &exe_lba, &exe_size)) return 0;
  if (exe_size < 0x800) return 0; // must hold a PS-X EXE header

  uint32_t exe_sec = sectors_for(exe_size);
  uint8_t* exe = (uint8_t*)malloc((size_t)exe_sec * 2048);
  if (!exe) return 0;
  if (psxport_cd_read_sectors((int32_t)exe_lba, (int)exe_sec, exe) != (int)exe_sec) {
    free(exe);
    return 0;
  }

  // PS-X EXE header (first 0x800 bytes). LE uint32 fields:
  //   +0x10 PC  +0x14 GP  +0x18 text_addr  +0x1C text_size
  //   +0x30 sp_base  +0x34 sp_offset
  if (memcmp(exe, "PS-X EXE", 8) != 0) {
    free(exe);
    return 0;
  }
  uint32_t pc        = rd_le32(exe + 0x10);
  uint32_t gp        = rd_le32(exe + 0x14);
  uint32_t text_addr = rd_le32(exe + 0x18);
  uint32_t text_size = rd_le32(exe + 0x1C);
  uint32_t sp_base   = rd_le32(exe + 0x30);
  uint32_t sp_offset = rd_le32(exe + 0x34);

  // Clamp text_size to what we actually read (header is at +0x800).
  if ((uint64_t)0x800 + text_size > (uint64_t)exe_sec * 2048) {
    text_size = (uint32_t)((uint64_t)exe_sec * 2048 - 0x800);
  }
  // Text segment loads at text_addr (mask to 2 MiB RAM offset).
  memcpy(ram + (text_addr & 0x1FFFFF), exe + 0x800, text_size);

  free(exe);

  if (out_pc) *out_pc = pc;
  if (out_gp) *out_gp = gp;
  // SP = sp_base + sp_offset; default to top of RAM when the EXE leaves it 0.
  if (out_sp) *out_sp = sp_base ? (sp_base + sp_offset) : 0x801FFF00u;
  return 1;
}

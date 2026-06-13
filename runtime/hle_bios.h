// psxport HLE BIOS — shared contract.
//
// Goal: a pure native (host C++) PlayStation BIOS — no MIPS BIOS ROM. The
// emulated CPU runs the GAME's EXE directly; every BIOS service (boot, A0/B0/C0
// syscalls, the event/thread kernel, cdrom: file I/O) is implemented here in
// native code. Built incrementally and scoped to psxport's target games
// (Tomba! 2, Crash Bash).
//
// Memory model: PSX main RAM is 2 MiB. A PSX virtual address A maps to host
// byte `ram[A & 0x1FFFFF]` (KUSEG/KSEG0/KSEG1 all alias the same RAM). All
// multi-byte values are little-endian (host is x86-64 LE, so plain memcpy /
// unaligned loads are fine; the helpers below keep it explicit and portable).

#ifndef PSXPORT_HLE_BIOS_H
#define PSXPORT_HLE_BIOS_H

#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- Main RAM access helpers (PSX virtual address in; & 0x1FFFFF applied) ----
static inline uint32_t hle_r32(const uint8_t* ram, uint32_t a) { uint32_t v; memcpy(&v, ram + (a & 0x1FFFFF), 4); return v; }
static inline uint16_t hle_r16(const uint8_t* ram, uint32_t a) { uint16_t v; memcpy(&v, ram + (a & 0x1FFFFF), 2); return v; }
static inline uint8_t  hle_r8 (const uint8_t* ram, uint32_t a) { return ram[a & 0x1FFFFF]; }
static inline void hle_w32(uint8_t* ram, uint32_t a, uint32_t v) { memcpy(ram + (a & 0x1FFFFF), &v, 4); }
static inline void hle_w16(uint8_t* ram, uint32_t a, uint16_t v) { memcpy(ram + (a & 0x1FFFFF), &v, 2); }
static inline void hle_w8 (uint8_t* ram, uint32_t a, uint8_t  v) { ram[a & 0x1FFFFF] = v; }

// GPR indices (psxport_cpu_gpr() / the gpr[] passed to handlers): MIPS o32.
enum { HLE_R_ZERO=0, HLE_R_AT=1, HLE_R_V0=2, HLE_R_V1=3, HLE_R_A0=4, HLE_R_A1=5,
       HLE_R_A2=6, HLE_R_A3=7, HLE_R_T1=9, HLE_R_GP=28, HLE_R_SP=29, HLE_R_RA=31 };

// Native CD read (implemented in the imported cdc.c): read `count` 2048-byte
// Mode2/Form1 user-data sectors from filesystem LBA `lba` into dst. Returns
// count on success, -1 on failure. This is the host-speed disc backend the HLE
// boot + cdrom: file I/O are built on.
int psxport_cd_read_sectors(int32_t lba, int count, uint8_t* dst);

// ---- Stage 1: boot ----------------------------------------------------------
// Parse the disc: read SYSTEM.CNF (root dir of the ISO9660 filesystem), extract
// BOOT = cdrom:\NAME;1, locate NAME, read + parse its PS-X EXE header, copy the
// text segment into `ram`, and return the initial CPU state. Returns true on
// success. (Implemented by the ISO/EXE module.)
//   out_pc/out_sp/out_gp = EXE entry PC, initial SP, GP (r28).
int psxport_hle_boot(uint8_t* ram, uint32_t* out_pc, uint32_t* out_sp, uint32_t* out_gp);

// ISO9660 lookup: find a file by name in the root directory. Returns true and
// fills *out_lba (filesystem LBA, pass straight to psxport_cd_read_sectors) and
// *out_size (bytes). Name match is case-insensitive and ignores a ";1" suffix.
int psxport_hle_iso_find(const char* name, uint32_t* out_lba, uint32_t* out_size);

// ---- Stage 2: A0/B0/C0 syscalls --------------------------------------------
// Dispatch a BIOS syscall. `table` is 'A','B','C'; `fnum` is the function number
// (from $t1). gpr is the 35-entry register file; ram is main RAM. Return 1 if
// handled (with gpr[HLE_R_V0] set to the return value), 0 if unimplemented (the
// caller logs it). a0..a3 are gpr[4..7]; further args are on the stack at
// gpr[29]+0x10, 0x14, ... (o32: first 4 args have reserved stack slots too).
int psxport_hle_syscall(char table, uint32_t fnum, uint32_t* gpr, uint8_t* ram);

#ifdef __cplusplus
}
#endif

#endif // PSXPORT_HLE_BIOS_H

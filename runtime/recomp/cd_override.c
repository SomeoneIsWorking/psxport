// Native CD overrides (S3): replace the game's libcd read path with direct native file
// I/O, completing synchronously. "No emulation" — we do NOT model the CD controller or
// deliver CD IRQs; instead the recomp-overrides layer swaps in native bodies for the two
// chokepoints, keeping the original recomp bodies alive (A/B + diffable) per the skill.
//
// Override points (entry addresses; decomp = scratch/decomp/ram_f1000_all.c):
//   * 0x8008B2D8  FUN_8008b2d8  low-level CdInit. The real body does the CD-controller
//     reset handshake then spins in CD_cw / CD timeout waiting on the IRQ-set status
//     DAT_800ac298, which nothing sets -> Init failed. Override: report drive ready
//     (v0=0). Its caller FUN_80089930 still runs FUN_8008b19c (harmless reg pokes) and
//     FUN_800898a0 still installs the libcd callbacks, so only the HW handshake is bypassed.
//   * 0x8008C1EC  FUN_8008c1ec(blocks, lba, buf) the single synchronous read primitive
//     (LBA->MSF, Setloc, ReadN, blocking wait via FUN_8008cafc). Override: read blocks*2048
//     bytes from the disc image at lba straight into buf, return 1 (its bool success value).
//     This bypasses the whole FUN_8008c960/c5d8/cafc/ac34 command+IRQ machinery for data.
#include "r3000.h"
#include <stdio.h>
#include <stdlib.h>

enum { V0 = 2, A0 = 4, A1 = 5, A2 = 6 };

int disc_read_sector(uint32_t lba, uint8_t* out);

// 0x8008B2D8 FUN_8008b2d8: low-level CdInit -> success (drive ready), no HW handshake.
static void ov_cdinit(R3000* c) { c->r[V0] = 0; }

// libcd command/sync primitives whose real bodies spin in a CD_cw / CD timeout loop on the
// IRQ-set status DAT_800ac298 (never set — no controller). Since every DATA read is served
// natively by ov_cd_read, the remaining CdControl/CdSync calls (Setmode, Pause, Nop, ...)
// only need to report success so their waits fall through. We replace, not super-call: the
// real bodies cannot return without the IRQ. Result bytes (drive status) the caller may copy
// are zeroed — callers on the boot path branch on the return value, not the status bytes.
static void zero_result(uint32_t p) { if (p) for (int i = 0; i < 8; i++) mem_w8(p + i, 0); }

// 0x8008AC34 FUN_8008ac34(cmd, param, result, mode) CdCommand -> 0 (success).
static void ov_cd_command(R3000* c) { zero_result(c->r[A2]); c->r[V0] = 0; }
// 0x8008A6EC FUN_8008a6ec(noblock, result) CdSync -> 2 (status: complete/ready).
static void ov_cd_sync(R3000* c) { zero_result(c->r[A1]); c->r[V0] = 2; }

// 0x8008C1EC FUN_8008c1ec(a0=blocks, a1=lba, a2=buf): native synchronous read.
static int  g_cd_verbose = 0;  // PSXPORT_CD_VERBOSE=1
static void ov_cd_read(R3000* c) {
  uint32_t blocks = c->r[A0], lba = c->r[A1], buf = c->r[A2];
  uint8_t sec[2048];
  for (uint32_t i = 0; i < blocks; i++) {
    if (!disc_read_sector(lba + i, sec)) { c->r[V0] = 0; return; }  // bool: 0 = failure
    for (uint32_t j = 0; j < 2048; j++) mem_w8(buf + i * 2048u + j, sec[j]);
  }
  if (g_cd_verbose)
    fprintf(stderr, "[cd] read %u blk @ LBA %u -> 0x%08X\n", blocks, lba, buf);
  c->r[V0] = 1;  // bool: success
}

void cd_overrides_init(void) {
  if (getenv("PSXPORT_CD_VERBOSE")) g_cd_verbose = 1;
  rec_set_override(0x8008B2D8u, ov_cdinit);
  rec_set_override(0x8008AC34u, ov_cd_command);
  rec_set_override(0x8008A6ECu, ov_cd_sync);
  rec_set_override(0x8008C1ECu, ov_cd_read);
}

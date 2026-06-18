// Native stall-free overrides: hardware "sync"/wait functions that busy-spin on a status bit
// an IRQ would clear are replaced by native bodies that complete immediately. In this port the
// operations they wait on are performed synchronously (the GTE/MDEC/CD work is done inline by
// the time the game polls), so the wait is always already satisfied — we return success instead
// of spinning the original 0x100000-iteration loop out to a timeout. (Standing rule: convert
// every stalling function to a PC-native non-stall; CD command/VSync are handled in their own
// modules — cd_override.c / timing.c — do not duplicate those here.)
//
// Each override is keyed by the original function's entry address; the generated func_<addr>
// wrapper runs the override instead of the recomp body, so ALL call paths (recompiled or
// interpreted) hit it. Decomp reference: scratch/decomp/ram_f1000_all.c.
#include "core.h"

enum { V0 = 2, A0 = 4, A1 = 5, A2 = 6 };

// 0x8009CAEC DecDCTinSync / 0x8009CB80 DecDCToutSync — the MDEC in/out sync waits. The real
// bodies spin on MDEC1 status bit 0x20000000 (data-in busy) / DMA1 busy bit 0x1000000 until an
// IRQ clears them. MDEC decode + its DMAs are synchronous here, so the sync is already done.
static void ov_sync_ok(Core* c) { c->r[V0] = 0; }   // 0 = sync complete, success

// Zero a libcd result buffer (the 8-byte status packet a caller may inspect). Mirrors
// cd_override.c: on the boot path callers branch on the return value; the status bytes that
// would be filled by a real IRQ are reported as "clear" so no stale-IRQ flag is seen.
static void zero_result(Core* c, uint32_t p) { if (p) for (int i = 0; i < 8; i++) c->mem_w8(p + i, 0); }

// 0x8008A96C FUN_8008a96c(mode, result) — CdReadSync. Blocking path spins until the CD
// data-ready IRQ sets DAT_800ac29a (and the per-cmd-complete DAT_800ac299); the timeout path
// (deadline DAT_80102748 / 0x3c0000 counter) prints "CD timeout" and returns 0xFFFFFFFF.
// Neither IRQ fires in the no-IRQ model. Native data reads (cd_override.c ov_cd_read @
// 0x8008C1EC) complete synchronously, so a read is always already done: report "nothing
// pending / complete" = 0. This matches the function's own poll-mode (param_1!=0) return
// (decomp 65760: `if (param_1 != 0) return 0;`), and its caller FUN_8008d110 (decomp 66938)
// only requires the value != 5 and reads the result buffer, which we clear.
static void ov_cdreadsync(Core* c) { zero_result(c, c->r[A1]); c->r[V0] = 0; }

// 0x8008B4B8 FUN_8008b4b8(mode) — CdDataSync (CD DMA-done wait). Blocking path spins while the
// CD DMA busy bit (*DAT_800ac2c4 & 0x1000000) is set; the same deadline/0x3c0000 timeout
// prints "CD timeout" and returns 0xFFFFFFFF. The blocking-success return is 0 (decomp 66067:
// `uVar3 = 0` once the DMA-busy bit is clear), i.e. DMA idle. The CD DMA is never started in
// this port (reads are served by native file I/O), so the DMA is always idle => return 0.
static void ov_cddatasync(Core* c) { c->r[V0] = 0; }   // 0 = DMA idle / transfer complete

void sync_overrides_init(void) {
  rec_set_override(0x8009CAECu, ov_sync_ok);      // DecDCTinSync  ("MDEC_in_sync timeout")
  rec_set_override(0x8009CB80u, ov_sync_ok);      // DecDCToutSync ("MDEC_out_sync timeout")
  rec_set_override(0x8008A96Cu, ov_cdreadsync);   // CdReadSync    ("CD timeout")
  rec_set_override(0x8008B4B8u, ov_cddatasync);   // CdDataSync    ("CD timeout")
}

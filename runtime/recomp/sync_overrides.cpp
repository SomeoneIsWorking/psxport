// PLATFORM HLE TABLE — hardware sync/wait primitives resolved as instant native calls.
//
// WHAT THIS IS (and is NOT). The game-override table removed 2026-06-22 was the RUNTIME table that
// agents abused to flip GAME-LOGIC FUN_xxxx (engine/content) functions into native C instead of
// porting them top-down. THIS table is categorically different and is NOT that: it HLE's PSX
// BIOS/LIBRARY HARDWARE-SYNC primitives — the SCEI libcd/libetc/libmdec sync/wait functions linked
// into MAIN.EXE — that busy-spin on a hardware IRQ or status bit our cooperative, no-IRQ runtime
// never satisfies. On a PC there is no PSX controller/GPU/MDEC to wait on; the work these functions
// gate is done synchronously (native reads / native VSync / synchronous MDEC), so the wait is always
// already satisfied and must resolve immediately rather than spin to a 0x100000-iteration "timeout".
// This is the SAME class as the BIOS A0/B0/C0 HLE in hle.cpp — hardware emulation, not a game-logic
// override.
//
// HARD RULE: every entry MUST be a PSX LIBRARY/BIOS function (libcd/libetc/libmdec/libgpu). NEVER a
// game or engine FUN_xxxx — those are owned top-down by PC calling them directly. The table is FIXED
// and small (the HW-sync surface); it does not grow as the engine port advances. The registrar
// asserts each address lies in the resident BIOS-library code window (see PLAT_LO/PLAT_HI) to keep
// anyone from sneaking a game address in here.
//
// WHY a table and not only native call-site replacement: these waits are reached from BOTH native
// callers (boot init prefix — handled directly there) AND from deep inside still-INTERPRETED library
// code (e.g. libgs FUN_8009c820/FUN_8009c9d0 call MDEC in-sync FUN_8009caec) that native code
// rec_dispatches wholesale. There is no native call site to redirect for those, so the interpreter
// (coro_native_call) and the dispatch entry (rec_dispatch_miss) consult this table by address. User
// directive 2026-06-22: "no busy-waits anywhere; everything must be PC-native sync calls that resolve
// as fast as the PC can" + "platform-HLE table" chosen as the mechanism.
#include "core.h"
#include <cstdio>
#include <cstdlib>

enum { V0 = 2, A0 = 4, A1 = 5, A2 = 6 };

// 0x8009CAEC DecDCTinSync / 0x8009CB80 DecDCToutSync — libmdec in/out sync. Real bodies spin (0x100000
// iters) on the MDEC1 status (DAT_800ad098 -> 0x1F801824) data-in busy bit 0x20000000 / DMA1 busy bit
// 0x1000000 until an IRQ clears them, else print "MDEC_in_sync/out_sync" + return -1. MDEC decode + its
// DMAs are synchronous here, so the sync is already done -> return 0 (complete).
static void ov_sync_ok(Core* c) { c->r[V0] = 0; }   // 0 = sync complete, success

// Zero a libcd result buffer (the 8-byte status packet a caller may inspect). On the boot path callers
// branch on the return value; the IRQ-filled status bytes are reported "clear" so no stale flag is seen.
static void zero_result(Core* c, uint32_t p) { if (p) for (int i = 0; i < 8; i++) c->mem_w8(p + i, 0); }

// 0x8008A96C FUN_8008a96c(mode, result) — CdReadSync. Blocking path spins until the CD data-ready IRQ
// sets DAT_800ac29a; the timeout path prints "CD timeout" + returns 0xFFFFFFFF. Native data reads
// complete synchronously, so a read is always already done: report "nothing pending / complete" = 0.
static void ov_cdreadsync(Core* c) { zero_result(c, c->r[A1]); c->r[V0] = 0; }

// 0x8008B4B8 FUN_8008b4b8(mode) — CdDataSync (CD DMA-done wait). Real body spins while the CD DMA busy
// bit is set, else times out. The CD DMA is never started here (reads are native file I/O) -> idle -> 0.
static void ov_cddatasync(Core* c) { c->r[V0] = 0; }   // 0 = DMA idle / transfer complete

// 0x8008B2D8 low-level CdInit reset handshake — pokes the (unmodelled) CD HW registers then spins in
// CD_cw on the controller-ready bit DAT_800ac298 nothing sets -> "CD timeout" -> "Init failed". We model
// no controller; report drive ready (v0=0). (The boot init prefix bypasses the whole CdInit caller via
// cd_hle_init; this entry covers any OTHER caller that reaches the handshake from interpreted code.)
static void ov_cdinit_hs(Core* c) { c->r[V0] = 0; }

// 0x800834A0 FUN_800834a0 / 0x800834D4 FUN_800834d4 — libgpu GPU-DMA-completion TIMEOUT (arm / check).
// The DMA-send primitives (FUN_80082424 etc.) kick the GPU OT-linked-list DMA, then arm a vblank-based
// deadline (FUN_800834a0 reads VSync(-1)+240) and poll the DMA-busy bit, calling FUN_800834d4 each spin
// to detect a hung controller ("GPU timeout que=..."). Our GPU is native (VK) and the OT-DMA runs
// SYNCHRONOUSLY on the channel-start write, so the transfer is already complete by the first poll — the
// timeout is never needed. Own the pair natively so they never touch VSync: arm a far-future deadline
// (no VSync read); report "not timed out" (the busy bit is already clear so the poll exits first anyway).
static void ov_gpu_timeout_arm(Core* c) { c->mem_w32(0x800a5adcu, 0x7fffffffu); c->mem_w32(0x800a5ae0u, 0); }
static void ov_gpu_timeout_chk(Core* c) { c->r[V0] = 0; }   // 0 = not timed out (DMA already done)

// VSync TRAP (user 2026-06-22): the PC-native frame loop (native_boot.cpp for-loop + gpu_pace_frame)
// OWNS all timing. NOTHING may reach libetc VSync 0x80085900 — not to WAIT for a vblank (pacing) and
// not to QUERY the vblank counter (PSX-time dependency). Any caller that does is PSX-timing code that
// must be PC-owned (ported top-down to read the native frame clock / pace via the native loop). There
// are NO exceptions: every mode, every caller, traps. (The prior instant-return / query carve-out /
// boot counter-bump were bandaids — removed.) The trap prints the caller and aborts so it can be owned.
extern volatile uint32_t g_interp_pc;

// Walk the guest stack (sp upward) printing plausible return addresses in resident-code range, so a
// trap shows the call chain that reached it (e.g. async-read issuer -> CD_cw -> VSync). Best-effort:
// the recomp ABI doesn't frame-link, so this scans sp..sp+512 for words that look like return PCs.
// Write the backtrace to an arbitrary stream (so the SBS divergence debugger can capture it into a
// string buffer and serve it over the debug server, not only abort-to-stderr).
extern "C" void guest_backtrace_to(Core* c, FILE* out) {
  uint32_t sp = c->r[29];
  fprintf(out, "  guest stack (sp=0x%08X), plausible return addrs:\n", sp);
  int shown = 0;
  for (uint32_t a = sp; a < sp + 512 && shown < 16; a += 4) {
    uint32_t w = c->mem_r32(a);
    uint32_t k = w & 0x1FFFFFFF;
    if (k >= 0x10000 && k < 0x120000 && (w & 3) == 0)   // resident MAIN/overlay code, word-aligned
      { fprintf(out, "    [sp+0x%03X] 0x%08X\n", a - sp, w); shown++; }
  }
}
static void guest_backtrace(Core* c) { guest_backtrace_to(c, stderr); }

static void trap_abort(Core* c, const char* what, uint32_t addr) {
  fprintf(stderr, "\n[%s-TRAP] reached 0x%08X  a0=%d ra=0x%08X interp_pc=0x%08X\n"
                  "  Everything must be PC-native + SYNCHRONOUS (no PSX vblank/IRQ waits, no async CD).\n"
                  "  This caller must be PC-owned (ported top-down) so it never reaches this primitive.\n",
          what, addr, (int)c->r[4], c->r[31], g_interp_pc);
  guest_backtrace(c);
  fflush(stderr);
  abort();
}

static void ov_vsync_trap(Core* c) { trap_abort(c, "VSYNC", 0x80085900u); }

// ---- the platform-HLE registry --------------------------------------------------------------------
// Resident BIOS-library code window (SCEI libs linked into MAIN.EXE sit high in the resident text). The
// registrar refuses any address outside it, so a game/engine FUN_xxxx (lower text / overlays) can never
// be registered here.
// Two platform windows, both I/O / hardware-service, NEVER game logic:
//   [0x8001C000,0x8001E000) — the engine's CD/SPU I/O GLUE (libcd-wrapper readers FUN_8001D940/
//     DB8C/DC40, SPU-mix FUN_8001CF00). This is the I/O boundary, not AI/physics/quest logic.
//   [0x80082000,0x8009E000) — the SCEI LIBRARY text (libgpu/libetc/libcd/libgs/libmdec).
// Game/engine LOGIC lives in [0x8001E000,0x80082000) (FUN_8004xxxx-FUN_8007xxxx, main) and the
// overlays (0x8010xxxx+) — both OUTSIDE these windows, so the guard keeps logic out of this table.
static int plat_in_window(uint32_t a) {
  // 0x8001Cxxx libcd glue; 0x80080000-0x8009E000 the PSX kernel/BIOS-library window (SCEI libs +
  // the kernel thread primitives at 0x80080xxx — ChangeThread/OpenThread, which the cooperative
  // scheduler funnels every task-switch through). All platform/BIOS, never game/engine FUN_xxxx.
  return (a >= 0x8001C000u && a < 0x8001E000u) || (a >= 0x80080000u && a < 0x8009E000u);
}
enum { PLAT_MAX = 32 };
static uint32_t  s_plat_addr[PLAT_MAX];
static OverrideFn s_plat_fn[PLAT_MAX];
static int       s_plat_n = 0;
static uint32_t  s_plat_lo = 0xFFFFFFFFu, s_plat_hi = 0;   // observed [min,max] for the fast gate

void platform_hle_register(uint32_t addr, OverrideFn fn);
static void plat_register(uint32_t addr, OverrideFn fn) { platform_hle_register(addr, fn); }
void platform_hle_register(uint32_t addr, OverrideFn fn) {
  if (!plat_in_window(addr)) {
    fprintf(stderr, "[plat-hle] REFUSED 0x%08X — not an I/O / BIOS-library address (game/engine logic "
                    "is owned top-down, never HLE'd here)\n", addr);
    return;
  }
  if (s_plat_n >= PLAT_MAX) { fprintf(stderr, "[plat-hle] table full\n"); return; }
  s_plat_addr[s_plat_n] = addr; s_plat_fn[s_plat_n] = fn; s_plat_n++;
  if (addr < s_plat_lo) s_plat_lo = addr;
  if (addr > s_plat_hi) s_plat_hi = addr;
}

// Fast lookup (called on every interpreted call target). The [min,max] gate makes it ~free for the
// overwhelming majority of calls (game/engine/overlay code, all outside the BIOS-library window).
OverrideFn platform_hle_lookup(uint32_t addr) {
  if (addr < s_plat_lo || addr > s_plat_hi) return 0;
  for (int i = 0; i < s_plat_n; i++) if (s_plat_addr[i] == addr) return s_plat_fn[i];
  return 0;
}

void sync_overrides_init(void) {
  // libmdec sync (reached from libgs FUN_8009c820/FUN_8009c9d0/FUN_8009ca60 etc. — interpreted).
  plat_register(0x8009CAECu, ov_sync_ok);      // DecDCTinSync
  plat_register(0x8009CB80u, ov_sync_ok);      // DecDCToutSync
  // libcd sync (reached from in-game/cutscene CD code — interpreted).
  plat_register(0x8008A96Cu, ov_cdreadsync);   // CdReadSync
  plat_register(0x8008B4B8u, ov_cddatasync);   // CdDataSync
  plat_register(0x8008B2D8u, ov_cdinit_hs);    // low-level CdInit reset handshake
  // libgpu GPU-DMA-completion timeout (arm/check) — native no-ops, never read VSync (GPU is synchronous).
  plat_register(0x800834A0u, ov_gpu_timeout_arm);   // FUN_800834a0 arm deadline
  plat_register(0x800834D4u, ov_gpu_timeout_chk);   // FUN_800834d4 check (not timed out)
  // libetc VSync — TRAP every caller, every mode (the native loop owns ALL timing). See ov_vsync_trap.
  plat_register(0x80085900u, ov_vsync_trap);   // VSync(mode)
  // Cooperative task-switch: ChangeThread (FUN_80080880) is the universal yield/task-end primitive
  // (FUN_80051f80 yield + FUN_80051fb4 task-end both funnel through it). Wire it to ov_switch so a
  // yield from an interpreted task coroutine saves the task's resume context and longjmps back to the
  // native scheduler (native_boot.cpp). Without this, every GAME-stage per-frame yield spins forever
  // (the override table that used to carry this was removed). ov_switch no-ops outside a task run.
  { void ov_switch(Core*); plat_register(0x80080880u, ov_switch); }   // ChangeThread -> scheduler yield
}

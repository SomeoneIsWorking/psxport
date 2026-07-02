// PLATFORM HLE TABLE — hardware sync/wait primitives resolved as instant native calls.
//
// See platform_hle.h for the class shape. This TU owns:
//   (a) the singleton `class PlatformHle` state + register/lookup impl,
//   (b) the hardware-service HANDLERS (sync_ok / cdreadsync / vsync_trap / etc.),
//   (c) the initBuiltins() list that wires the shipped HLE entries.
//   (d) the guest-backtrace utility used by the traps (also called by the SBS divergence debugger).
//
// HARD RULE: every entry MUST be a PSX LIBRARY/BIOS function (libcd/libetc/libmdec/libgpu). NEVER a
// game or engine FUN_xxxx — those are owned top-down by PC calling them directly. The registrar
// asserts each address lies in the resident BIOS-library code window.

#include "platform_hle.h"
#include "core.h"
#include "scheduler.h"
#include <cstdio>
#include <cstdlib>

enum { V0 = 2, A0 = 4, A1 = 5, A2 = 6 };

// ---- HANDLERS (stateless, take Core*, mutate Core state directly) ---------------------------------

// 0x8009CAEC DecDCTinSync / 0x8009CB80 DecDCToutSync — libmdec in/out sync. Real bodies spin (0x100000
// iters) on the MDEC1 status until an IRQ clears them. MDEC decode + its DMAs are synchronous here, so
// the sync is already done -> return 0 (complete).
static void sync_ok(Core* c) { c->r[V0] = 0; }

// Zero a libcd result buffer (the 8-byte status packet a caller may inspect). On the boot path callers
// branch on the return value; the IRQ-filled status bytes are reported "clear" so no stale flag is seen.
static void zero_result(Core* c, uint32_t p) { if (p) for (int i = 0; i < 8; i++) c->mem_w8(p + i, 0); }

// 0x8008A96C FUN_8008a96c(mode, result) — CdReadSync. Blocking path spins until the CD data-ready IRQ
// sets DAT_800ac29a. Native data reads complete synchronously → report "nothing pending / complete" = 0.
static void cdreadsync(Core* c) { zero_result(c, c->r[A1]); c->r[V0] = 0; }

// 0x8008B4B8 FUN_8008b4b8(mode) — CdDataSync (CD DMA-done wait). The CD DMA is never started here (reads
// are native file I/O) -> idle -> 0.
static void cddatasync(Core* c) { c->r[V0] = 0; }

// 0x8008B2D8 low-level CdInit reset handshake — spins in CD_cw on the controller-ready bit nothing sets.
// We model no controller; report drive ready (v0=0).
static void cdinit_hs(Core* c) { c->r[V0] = 0; }

// 0x800834A0 / 0x800834D4 — libgpu GPU-DMA-completion TIMEOUT (arm / check). Our GPU is native (VK) and
// the OT-DMA runs SYNCHRONOUSLY on the channel-start write, so the timeout is never needed. Arm a
// far-future deadline (no VSync read); report "not timed out".
static void gpu_timeout_arm(Core* c) { c->mem_w32(0x800a5adcu, 0x7fffffffu); c->mem_w32(0x800a5ae0u, 0); }
static void gpu_timeout_chk(Core* c) { c->r[V0] = 0; }

// Walk the guest stack (sp upward) printing plausible return addresses in resident-code range, so a
// trap shows the call chain that reached it (e.g. async-read issuer -> CD_cw -> VSync). Best-effort:
// the recomp ABI doesn't frame-link, so this scans sp..sp+512 for words that look like return PCs.
// Kept as `extern "C"` because the SBS divergence debugger captures it via a function-pointer.
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
  fprintf(stderr, "\n[%s-TRAP] reached 0x%08X  a0=%d ra=0x%08X pc=0x%08X\n"
                  "  Everything must be PC-native + SYNCHRONOUS (no PSX vblank/IRQ waits, no async CD).\n"
                  "  This caller must be PC-owned (ported top-down) so it never reaches this primitive.\n",
          what, addr, (int)c->r[4], c->r[31], c->pc);
  guest_backtrace(c);
  fflush(stderr);
  abort();
}

// VSync TRAP (user 2026-06-22): the PC-native frame loop OWNS all timing. NOTHING may reach libetc
// VSync 0x80085900 — not to WAIT for a vblank and not to QUERY the vblank counter. Every mode traps.
static void vsync_trap(Core* c) { trap_abort(c, "VSYNC", 0x80085900u); }

// ---- class PlatformHle ---------------------------------------------------------------------------

PlatformHle& PlatformHle::instance() {
  static PlatformHle s;
  return s;
}

// Two platform windows, both I/O / hardware-service, NEVER game logic:
//   [0x8001C000,0x8001E000) — the engine's CD/SPU I/O GLUE (libcd-wrapper readers, SPU-mix).
//   [0x80080000,0x8009E000) — the SCEI LIBRARY text (libgpu/libetc/libcd/libgs/libmdec) + the kernel
//     thread primitives at 0x80080xxx (ChangeThread/OpenThread — the scheduler funnel).
// Game/engine LOGIC lives in [0x8001E000,0x80082000) (main) and the overlays (0x8010xxxx+) — both
// OUTSIDE these windows, so the guard keeps logic out of this table.
bool PlatformHle::inBiosWindow(uint32_t a) {
  return (a >= 0x8001C000u && a < 0x8001E000u) || (a >= 0x80080000u && a < 0x8009E000u);
}

void PlatformHle::register_(uint32_t addr, OverrideFn fn) {
  if (!inBiosWindow(addr)) {
    fprintf(stderr, "[plat-hle] REFUSED 0x%08X — not an I/O / BIOS-library address (game/engine logic "
                    "is owned top-down, never HLE'd here)\n", addr);
    return;
  }
  if (mN >= kMax) { fprintf(stderr, "[plat-hle] table full\n"); return; }
  mAddr[mN] = addr; mFn[mN] = fn; mN++;
  if (addr < mLo) mLo = addr;
  if (addr > mHi) mHi = addr;
  // CRITICAL (later-257, substrate): these HW-sync primitives are RECOMPILED MAIN functions, so a call
  // to one routes rec_dispatch -> main_dispatch -> func_<addr> -> the recompiled BUSY-WAIT body, which
  // never reaches rec_dispatch_miss (where lookup used to intercept). That spins on an IRQ/status bit
  // our no-IRQ runtime never sets -> "CD timeout" / "VSync: timeout". Wire the HLE into the recomp
  // OVERRIDE table too (func_<addr>'s wrapper checks g_override[idx] FIRST), so the native sync
  // resolves it before the recompiled wait ever runs. No-op if `addr` isn't recompiled.
  extern void shard_set_override(uint32_t, OverrideFn);
  shard_set_override(addr, fn);
}

OverrideFn PlatformHle::lookup(uint32_t addr) const {
  if (addr < mLo || addr > mHi) return nullptr;
  for (int i = 0; i < mN; i++) if (mAddr[i] == addr) return mFn[i];
  return nullptr;
}

void PlatformHle::initBuiltins() {
  // libmdec sync (reached from libgs FUN_8009c820/FUN_8009c9d0/FUN_8009ca60 etc. — interpreted).
  register_(0x8009CAECu, sync_ok);          // DecDCTinSync
  register_(0x8009CB80u, sync_ok);          // DecDCToutSync
  // libcd sync (reached from in-game/cutscene CD code — interpreted).
  register_(0x8008A96Cu, cdreadsync);       // CdReadSync
  register_(0x8008B4B8u, cddatasync);       // CdDataSync
  register_(0x8008B2D8u, cdinit_hs);        // low-level CdInit reset handshake
  // libgpu GPU-DMA-completion timeout — native no-ops, never read VSync (GPU is synchronous).
  register_(0x800834A0u, gpu_timeout_arm);  // FUN_800834a0 arm deadline
  register_(0x800834D4u, gpu_timeout_chk);  // FUN_800834d4 check (not timed out)
  // libetc VSync — TRAP every caller, every mode.
  register_(0x80085900u, vsync_trap);
  // Cooperative task-switch: ChangeThread (FUN_80080880) is the universal yield/task-end primitive.
  // Wire it to scheduler_yield so a yield from an interpreted task coroutine saves the task's resume
  // context and longjmps back to the native scheduler. switch no-ops outside a task run.
  register_(0x80080880u, scheduler_yield);
}

// ---- Legacy free-function shims (thin bridges to the singleton) ------------------------------------
// Kept so callers that still take an address-only free-function pointer (e.g. shard init tables) work
// during the migration. New code should reach PlatformHle::instance().register_(...).

void platform_hle_register(uint32_t addr, OverrideFn fn) { PlatformHle::instance().register_(addr, fn); }
OverrideFn platform_hle_lookup(uint32_t addr)            { return PlatformHle::instance().lookup(addr); }
void sync_overrides_init(void)                            { PlatformHle::instance().initBuiltins(); }

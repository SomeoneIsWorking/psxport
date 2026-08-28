// PLATFORM HLE TABLE — hardware sync/wait primitives resolved as instant native calls.
//
// See platform_hle.h for the class shape. This TU owns:
//   (a) the singleton `class PlatformHle` state + register/lookup impl,
//   (b) the hardware-service HANDLERS (sync_ok / cdreadsync / vsync_trap / etc.),
//   (c) the initBuiltins() list that wires the shipped HLE entries.
//   (d) the guest-backtrace utility used by the traps (also called by the SBS divergence debugger).
//
// HARD RULE: every entry MUST be a PSX LIBRARY/BIOS function (libcd/libetc/libmdec/libgpu). Game and
// engine FUN_xxxx addresses are wired through the override registry instead (override_registry.h,
// overrides::install/overrides::dispatch, consulted at rec_dispatch top for every caller — user
// 2026-07-07). The registrar asserts each address here lies in the resident BIOS-library code window.

#include "cd_control.h"
#include "core.h"
#include "game.h"         // Game::core — register_/initBuiltins read the game's config off the Core
#include "game_runtime.h" // psxport_game_runtime — the direct-runtime PlatformHlePlan source
#include "platform_hle.h"
#include "proj_params.h"  // libgte_set_geom_offset / _screen — the camera projection setters
#include "recomp_iface.h" // seam: psxport_recomp()->shard_set_override (generated MAIN override setter)
#include "scheduler.h"
#include <cstdio>
#include <cstdlib>
#include <lucent/log.h>

enum { V0 = 2, A0 = 4, A1 = 5, A2 = 6 };

// ---- HANDLERS (stateless, take Core*, mutate Core state directly) ---------------------------------

// libgte SetGeomOffset(a0 = ofx, a1 = ofy) / SetGeomScreen(a0 = h) — the camera projection.
//
// UNLIKE EVERY OTHER ENTRY HERE these do not stand in for a spin loop: they are owned so the port
// RECORDS the projection where the game states it, instead of the native camera reading CR24/25/26
// back out of the GTE at draw time (see proj_params.h). They belong in this table rather than the
// override registry because they touch no guest RAM, so running natively on the oracle core too is
// correct — and because their behaviour is identical in every game that links libgte, with only the
// address being per-game, which is exactly what this table models.
//
// The recompiled body of SetGeomOffset shifts a0/a1 left 16 IN PLACE before writing them, so a caller
// reading them back afterwards sees the shifted values. That mutation is observable state; reproduce
// it. SetGeomScreen's body mutates no register.
static void set_geom_offset(Core *c) {
  const int32_t ofx = (int32_t)c->r[A0];
  const int32_t ofy = (int32_t)c->r[A1];
  c->r[A0] = (uint32_t)ofx << 16;
  c->r[A1] = (uint32_t)ofy << 16;
  libgte_set_geom_offset(c, ofx, ofy);
}

static void set_geom_screen(Core *c) {
  libgte_set_geom_screen(c, (int32_t)c->r[A0]);
}

// 0x8009CAEC DecDCTinSync / 0x8009CB80 DecDCToutSync — libmdec in/out sync. Real bodies spin (0x100000
// iters) on the MDEC1 status until an IRQ clears them. MDEC decode + its DMAs are synchronous here, so
// the sync is already done -> return 0 (complete).
static void sync_ok(Core *c) {
  c->r[V0] = 0;
}

// Zero a libcd result buffer (the 8-byte status packet a caller may inspect). On the boot path callers
// branch on the return value; the IRQ-filled status bytes are reported "clear" so no stale flag is seen.
static void zero_result(Core *c, uint32_t p) {
  if (p) {
    for (int i = 0; i < 8; i++) {
      c->mem_w8(p + i, 0);
    }
  }
}

// 0x8008A96C FUN_8008a96c(mode, result) — CdReadSync. Blocking path spins until the CD data-ready IRQ
// sets DAT_800ac29a. Native data reads complete synchronously → report "nothing pending / complete" = 0.
static void cdreadsync(Core *c) {
  zero_result(c, c->r[A1]);
  c->r[V0] = 0;
}

// 0x8008B4B8 FUN_8008b4b8(mode) — CdDataSync (CD DMA-done wait). The CD DMA is never started here (reads
// are native file I/O) -> idle -> 0.
static void cddatasync(Core *c) {
  c->r[V0] = 0;
}

// 0x8008B2D8 low-level CdInit reset handshake — spins in CD_cw on the controller-ready bit nothing sets.
// We model no controller; report drive ready (v0=0).
static void cdinit_hs(Core *c) {
  c->r[V0] = 0;
}

// libgpu GPU-DMA-completion TIMEOUT (arm / check). Our GPU is native (VK) and the OT-DMA runs
// SYNCHRONOUSLY on the channel-start write, so the timeout is never needed. Arm a far-future deadline
// (no VSync read); report "not timed out". The two guest globals the arm writes are GAME data and
// come from the config alongside the entry point itself.
static void gpu_timeout_arm(Core *c) {
  const GameConfig *cfg = c->cfg;
  if (cfg->hle.gpuTimeoutDeadlineVar) {
    c->mem_w32(cfg->hle.gpuTimeoutDeadlineVar, 0x7fffffffu);
  }
  if (cfg->hle.gpuTimeoutFlagVar) {
    c->mem_w32(cfg->hle.gpuTimeoutFlagVar, 0);
  }
}
static void gpu_timeout_chk(Core *c) {
  c->r[V0] = 0;
}

// Walk the guest stack (sp upward) printing plausible return addresses in resident-code range, so a
// trap shows the call chain that reached it (e.g. async-read issuer -> CD_cw -> VSync). Best-effort:
// the recomp ABI doesn't frame-link, so this scans sp..sp+512 for words that look like return PCs.
// Kept as `extern "C"` because the SBS divergence debugger captures it via a function-pointer.
extern "C" void guest_backtrace_to(Core *c, FILE *out) {
  // The "does this word look like a return address" test needs the game's resident-code range.
  // The optional diagnostic range wins; otherwise GuestProgramImage falls back to resident MAIN.
  // (Was hardcoded to Tomba!2's 0x10000..0x120000 — for another game that silently prints nothing,
  // or prints noise, exactly when a trap most needs to show its call chain.)
  if (!c->guestProgramImage) {
    fprintf(out, "  guest stack unavailable: GameRuntime supplies no GuestProgramImage\n");
    return;
  }
  const GuestAddressRange code = c->guestProgramImage->effectiveBacktraceText();
  if (code.empty()) {
    fprintf(out, "  guest stack unavailable: GuestProgramImage declares no resident code range\n");
    return;
  }
  if (!code.valid()) {
    fprintf(out,
            "  guest stack unavailable: GuestProgramImage declares invalid code range "
            "[0x%08X,0x%08X)\n",
            code.begin,
            code.end);
    return;
  }

  uint32_t sp = c->r[29];
  fprintf(out, "  guest stack (sp=0x%08X), plausible return addrs:\n", sp);
  int shown = 0;
  for (uint32_t a = sp; a < sp + 512 && shown < 16; a += 4) {
    uint32_t w = c->mem_r32(a);
    uint32_t k = w & 0x1FFFFFFF;
    if (code.containsPhysical(k) && (w & 3) == 0) // resident MAIN/overlay code, word-aligned
    {
      fprintf(out, "    [sp+0x%03X] 0x%08X\n", a - sp, w);
      shown++;
    }
  }
}
// WHERE DID THIS ADDRESS COME FROM? On a dispatch miss the address is often not referenced anywhere in
// the executable — it arrived as DATA (a function-pointer table loaded off the disc, a value the game
// computed and stored). Then the static question "who jumps here" has no answer and the only way
// forward is to find where the pointer LIVES. Scanning main RAM for the value gives that directly, and
// its address usually identifies the structure — and therefore the load — that produced it.
//
// Bounded and best-effort: 2MB of word compares is cheap on a path that is about to abort anyway.
extern "C" void guest_find_word_to(Core *c, FILE *out, uint32_t val) {
  fprintf(out, "  guest RAM locations holding 0x%08X:\n", val);
  int shown = 0;
  for (uint32_t a = 0x10000; a < 0x200000 && shown < 12; a += 4) {
    if (c->mem_r32(a | 0x80000000u) == val) {
      fprintf(out, "    [0x%08X]\n", a | 0x80000000u);
      shown++;
    }
  }
  if (!shown) {
    fprintf(out, "    (none — not stored in RAM as a word, so it is computed at the jump site)\n");
  }
}

static void guest_backtrace(Core *c) {
  guest_backtrace_to(c, stderr);
}

static void trap_abort(Core *c, const char *what, uint32_t addr) {
  lucent::error("sync-trap",
                "\n{}: reached 0x{:08X}  a0={} ra=0x{:08X} pc=0x{:08X}\n  Everything must be PC-native + SYNCHRONOUS "
                "(no PSX vblank/IRQ waits, no async CD).\n  This caller must be PC-owned (ported top-down) so it never "
                "reaches this primitive.",
                what,
                addr,
                (int)c->r[4],
                c->r[31],
                c->pc);
  guest_backtrace(c);
  fflush(stderr);
  abort();
}

// VSync TRAP: every product has a PC-native frame loop, so NOTHING may reach libetc VSync — not to
// wait for a vblank and not to query the counter. The title supplies only the measured address; this
// framework handler is universal and direct-runtime safe (Core::cfg is legitimately null there).
static void vsync_trap(Core *c) {
  trap_abort(c, "GUEST VSYNC VIOLATION", c->game->platform_hle.vsyncAddress());
}

// ---- class PlatformHle ---------------------------------------------------------------------------

// The platform windows are I/O / hardware-service address ranges, NEVER game logic — the guard is
// what keeps engine FUN_xxxx out of this table (those are owned top-down via the override registry).
// WHICH ranges those are is a fact about the game's memory map, so it comes from GameConfig
// (hle.windowLo/windowHi). Each half-open range may be one exact library body; another game's
// measured layout will differ.
//
// A game that configures NO window gets everything refused, with a diagnostic saying so. That is
// deliberate: silently accepting any address would turn the one guard protecting this table into a
// no-op for exactly the games that forgot to state their map.
bool PlatformHle::inBiosWindow(const GameConfig *cfg, uint32_t a) {
  // DIRECT runtime: the windows come from the runtime's own PlatformHlePlan (same guard, different
  // source — which windows admit registrations is a game memory-map fact either way).
  if (!cfg) {
    const GameRuntime *const runtime = psxport_game_runtime();
    const PlatformHlePlan *const plan = runtime ? runtime->platformHlePlan() : nullptr;
    bool anyDirect = false;
    if (plan) {
      for (int i = 0; i < kPlatformHleWindowCapacity; i++) {
        if (!plan->windowHi[i]) {
          continue;
        }
        anyDirect = true;
        if (a >= plan->windowLo[i] && a < plan->windowHi[i]) {
          return true;
        }
      }
    }
    if (!anyDirect) {
      lucent::error("plat-hle",
                    "no BIOS-library address window declared "
                    "(GameRuntime::platformHlePlan windows) — refusing every registration");
    }
    return false;
  }
  bool any = false;
  for (int i = 0; i < kPlatformHleWindowCapacity; i++) {
    if (!cfg->hle.windowHi[i]) {
      continue;
    }
    any = true;
    if (a >= cfg->hle.windowLo[i] && a < cfg->hle.windowHi[i]) {
      return true;
    }
  }
  if (!any) {
    lucent::error("plat-hle",
                  "no BIOS-library address window configured "
                  "(GameConfig::hle.windowLo/windowHi) — refusing every registration");
  }
  return false;
}

bool PlatformHle::register_(uint32_t addr, OverrideFn fn) {
  if (mVSyncAddress != 0 && (addr & 0x1FFFFFFFu) == (mVSyncAddress & 0x1FFFFFFFu) && fn != vsync_trap) {
    lucent::error("plat-hle",
                  "REFUSED replacement of mandatory VSync trap at 0x{:08X}; guest VSync has no "
                  "successful shipping handler",
                  mVSyncAddress);
    std::abort();
  }
  if (!inBiosWindow(game->core.cfg, addr)) {
    lucent::error(
        "plat-hle",
        "REFUSED 0x{:08X} — not an I/O / BIOS-library address (game/engine logic is owned top-down, never HLE'd here)",
        addr);
    return false;
  }
  // Boot setup is deliberately repeatable: some ports initialise the primary Game before entering
  // the shared boot path, while SBS constructs two fresh Games and initialises each there. Reusing an
  // address must replace that Game's local handler without consuming another table slot. Still write
  // the generated override below: another registrar may have displaced the process-global wrapper
  // between calls, so a local-table hit alone would not restore the recompiled dispatch path.
  for (int i = 0; i < mN; ++i) {
    if (mAddr[i] == addr) {
      mFn[i] = fn;
      if (const RecompRegistry *const rec = psxport_recomp()) {
        rec->shard_set_override(addr, fn);
      }
      return true;
    }
  }
  if (mN >= kMax) {
    lucent::info("plat-hle", "table full");
    return false;
  }
  mAddr[mN] = addr;
  mFn[mN] = fn;
  mN++;
  if (addr < mLo) {
    mLo = addr;
  }
  if (addr > mHi) {
    mHi = addr;
  }
  // CRITICAL (later-257, substrate): these HW-sync primitives are RECOMPILED MAIN functions, so a call
  // to one routes rec_dispatch -> main_dispatch -> func_<addr> -> the recompiled BUSY-WAIT body, which
  // never reaches rec_dispatch_miss (where lookup used to intercept). That spins on an IRQ/status bit
  // our no-IRQ runtime never sets -> "CD timeout" / "VSync: timeout". Wire the HLE into the recomp
  // OVERRIDE table too (func_<addr>'s wrapper checks g_override[idx] FIRST), so the native sync
  // resolves it before the recompiled wait ever runs. No-op if `addr` isn't recompiled. The MAIN
  // module setter is a generated symbol reached through the RecompRegistry seam (recomp_iface.h).
  // A missing registry means NO SUBSTRATE was installed (hermetic tests, tools/smoke) — nothing
  // exists to override, so only the wiring is skipped, never the table entry above.
  if (const RecompRegistry *const rec = psxport_recomp()) {
    rec->shard_set_override(addr, fn);
  }
  return true;
}

void PlatformHle::bindVSyncTrap(uint32_t addr) {
  if (!addr) {
    return;
  }
  if (mVSyncAddress != 0 && (addr & 0x1FFFFFFFu) != (mVSyncAddress & 0x1FFFFFFFu)) {
    lucent::error("plat-hle",
                  "conflicting VSync addresses 0x{:08X} and 0x{:08X}; one title has one measured "
                  "libetc entry",
                  mVSyncAddress,
                  addr);
    std::abort();
  }
  mVSyncAddress = addr;
  if (!register_(addr, vsync_trap)) {
    mVSyncAddress = 0;
    lucent::error("plat-hle", "failed to install mandatory VSync trap at 0x{:08X}", addr);
    std::abort();
  }
}

void PlatformHle::requireNativeFrameLoopContract() const {
  if (mVSyncAddress != 0) {
    return;
  }
  lucent::error("plat-hle",
                "GameRuntime declares no measured libetc VSync address; refusing product boot "
                "before guest code can enter a VSync wait/query");
  std::abort();
}

OverrideFn PlatformHle::lookup(uint32_t addr) const {
  if (addr < mLo || addr > mHi) {
    return nullptr;
  }
  for (int i = 0; i < mN; i++) {
    if (mAddr[i] == addr) {
      return mFn[i];
    }
  }
  return nullptr;
}

void PlatformHle::initBuiltins() {
  auto reg = [&](uint32_t addr, OverrideFn fn) {
    if (addr) {
      (void)register_(addr, fn);
    }
  };
  auto regProjectionLeaves = [&](uint32_t setGeomOffset, uint32_t setGeomScreen) {
    reg(setGeomOffset, set_geom_offset);
    reg(setGeomScreen, set_geom_screen);
  };

  // DIRECT runtimes (core.cfg == nullptr) declare their own hardware-sync primitives through
  // GameRuntime::platformHlePlan() — the consumer-owned fact slice from
  // docs/plans/game-seam-redesign.md. register_ applies the same inBiosWindow guard, sourcing the
  // windows from the plan (see inBiosWindow). A null plan is the honest "declares nothing": install
  // nothing, announce it, and let the guest spin in any real sync loop it reaches — the visible
  // signal that RE is outstanding.
  if (!game->core.cfg) {
    const GameRuntime *const runtime = psxport_game_runtime();
    const PlatformHlePlan *const plan = runtime ? runtime->platformHlePlan() : nullptr;
    if (plan) {
      regProjectionLeaves(plan->setGeomOffset, plan->setGeomScreen);
      reg(plan->cdReadAddress, cd_read_stock_sync);
      reg(plan->cdReadSyncAddress, cd_readsync_stock_sync);
      reg(plan->drawSyncAddress, sync_ok);
      bindVSyncTrap(plan->vsyncAddress);
      for (int i = 0; i < plan->bindingCount && i < PlatformHlePlan::kMaxBindings; i++) {
        if (plan->bindings[i].addr && plan->bindings[i].fn) {
          reg(plan->bindings[i].addr, plan->bindings[i].fn);
        }
      }
    }
    lucent::info("plat-hle",
                 "{} hardware-sync primitive(s) installed from GameRuntime::platformHlePlan{}",
                 mN,
                 mN ? "" : " — NONE declared; the guest will spin in any real sync loop it reaches");
    return;
  }

  // Adapter path: every address here is GAME data (GameConfig::hle) — the framework ships none. A
  // zero entry means "this game has no such primitive, or it has not been RE'd yet" and is skipped;
  // the game then hangs in the real spin loop if it needs it, which is the honest signal that RE is
  // outstanding.
  const GameConfig::PlatformHleCfg &h = game->core.cfg->hle;

  // libmdec sync — MDEC decode + its DMAs are synchronous here, so the sync is already done.
  reg(h.decDctInSync, sync_ok);
  reg(h.decDctOutSync, sync_ok);
  // libcd sync — native reads complete synchronously; the drive is modelled as always ready.
  reg(h.cdReadSync, cdreadsync);
  reg(h.cdDataSync, cddatasync);
  reg(h.cdInitHandshake, cdinit_hs);
  // libgpu GPU-DMA-completion timeout — native no-ops, never read VSync (the GPU is synchronous).
  reg(h.gpuTimeoutArm, gpu_timeout_arm);
  reg(h.gpuTimeoutCheck, gpu_timeout_chk);
  // libgpu DrawSync — the host GPU consumes GP0/DMA work synchronously, so there is no pending
  // hardware queue to wait for. Returning success is the same hardware event as a completed DMA,
  // and prevents the retained library body from falling into its VSync-based timeout loop.
  reg(h.drawSync, sync_ok);
  // Cooperative task-switch (ChangeThread): the universal yield/task-end primitive. Wired to
  // scheduler_yield so a yield from an interpreted task coroutine saves the task's resume context and
  // longjmps back to the native scheduler. No-ops outside a task run.
  reg(h.changeThread, scheduler_yield);
  // libetc VSync — every title supplies only its measured address; all modes hit the one fatal
  // native-frame-loop ownership trap above.
  bindVSyncTrap(h.vsyncTrap);
  // libgte SetGeomOffset / SetGeomScreen — the camera projection, recorded where the game STATES it.
  regProjectionLeaves(h.setGeomOffset, h.setGeomScreen);

  // State the wiring's own reach. Now that the addresses come from the game, "the table is empty
  // because the game configured nothing" and "the table is full" fail IDENTICALLY at a glance — the
  // run just hangs somewhere later. Reporting the count turns a silent misconfiguration into a
  // visible one, and gives any port a one-line check that its HLE actually installed.
  lucent::info("plat-hle",
               "{} hardware-sync primitive(s) installed from GameConfig::hle{}",
               mN,
               mN ? "" : " — NONE configured; the guest will spin in any real sync loop it reaches");
}

// Instance method — PlatformHle is a Game member (see game.h). Callers use `c->game->platform_hle`.

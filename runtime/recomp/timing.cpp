// Native VBlank / VSync source (S3). No CD/GPU IRQ is emulated, so the libetc VSync count
// DAT_800abde0 — normally bumped by the BIOS VBlank IRQ — never advances and the game's
// VSync(0) wrapper FUN_80085900 spins in FUN_80085a78 -> "VSync: timeout". Class Timing
// (game.h) owns the frame clock: each VSync(0)/VSync(n) advances a native frame clock and
// returns it; VSync(-1) queries it. This is the standard static-recomp frame model: one logic
// frame per VSync(0). Each tick also DeliverEvents the VBlank event class for any TestEvent-
// based waiter. Reached via c->game->timing.method().
#include "c_subsys.h" // watchdog_spin_fault — the spin fatal path
#include "cdc_state.h"
#include "config_vars.h" // cv_spin_ticks / cv_spin_runs — the spin-detector thresholds
#include "core.h"
#include "field_rate.h"
#include "game.h"
#include <algorithm> // std::min
#include <lucent/log.h>
#include <stdio.h>
#include <stdlib.h>

enum { A0 = 4, V0 = 2 };
#define VBLANK_COUNT 0x800ABDE0u // DAT_800abde0: libetc VSync counter (FUN_80085900 returns it)

uint64_t Timing::readEmulatedCpuTicks(void *context) {
  return static_cast<Timing *>(context)->mEmulatedTime.nowTicks();
}

void Timing::bindCdcClock(CdcState *cdc) {
  cdc_bind_tick_source(cdc, this, readEmulatedCpuTicks);
}

void Timing::advanceGuestInstructionTicks(uint32_t ticks) {
  guestInstructionTicks += ticks;
  mEmulatedTime.advanceInstructions(ticks);
  serviceCdc();
}

bool Timing::advanceDisplayFields(int fields, int parts, uint32_t fieldRateMilliHz) {
  if (fields <= 0 || parts <= 0 || fieldRateMilliHz == 0) {
    return false;
  }
  if (!mEmulatedTime.advanceDisplayFields(
          static_cast<uint32_t>(fields), static_cast<uint32_t>(parts), fieldRateMilliHz)) {
    return false;
  }
  serviceCdc();
  return true;
}

uint64_t Timing::emulatedCpuTicks() const {
  return mEmulatedTime.nowTicks();
}

uint16_t Timing::hSyncCounter() const {
  const bool pal = game && game->gpu.s_disp_pal != 0;
  return static_cast<uint16_t>(mEmulatedTime.hSyncCount(field_rate_millihz(pal), display_lines_per_field(pal)));
}

void Timing::serviceCdc() {
  if (cdc_drive_service(&game->cdc)) {
    game->core.irqStatLatch();
  }
}

bool spin_detector_sample(
    SpinDetectorState &st, uint32_t pc, bool host_starved, uint32_t ticks, uint64_t window_ticks, int max_run);
static bool spin_detector_sample_core(Core *core, uint32_t ticks, uint64_t window_ticks, int max_run) {
  const bool spun = spin_detector_sample(
      core->spin, core->pc, (core->pending_work & Core::PW_HOST) != 0, ticks, window_ticks, max_run);
  if (spun) {
    // Report here, in real context, then die loudly — never return to the spinning guest.
    watchdog_spin_fault(core->spin.anchor, core->pc, (unsigned long long)window_ticks * (unsigned long long)max_run);
  }
  return spun;
}
extern "C" void rec_guest_instruction_ticks(Core *core, uint32_t ticks) {
  core->game->timing.advanceGuestInstructionTicks(ticks);
  // Thresholds are read once per process: they are configuration, not something a run changes.
  static const uint64_t kWindow = [] {
    const long v = psx::config::cv_spin_ticks.get();
    return v > 0 ? (uint64_t)v : 0;
  }();
  static const int kMaxRun = (int)psx::config::cv_spin_runs.get();
  if (spin_detector_sample_core(core, ticks, kWindow, kMaxRun)) {
    // spin_detector_sample already reported through watchdog_spin_fault and aborted; this line
    // exists only so the compiler knows control does not continue.
    return;
  }
}

// ---- SPIN DETECTOR (see Core::spin_* and tests/test_spin_detector.cpp) -----------------------
// One decision per `window_ticks` guest instructions. A decision counts toward a spin only when
// BOTH hold: the host is still owed turns it never took (PW_HOST set — the guest has not reached
// any call boundary for the whole window), and the pc stayed within one ±32KB region of the
// anchor. Anything else resets: host serviced, or execution moved on. When the run reaches
// `max_run` consecutive starved in-region decisions the process fail-fasts NAMING the region —
// measured live as Vagrant's movie-wait spinning inside resident 0x80022484 while CD sectors
// flowed (issue #25).
bool spin_detector_sample(
    SpinDetectorState &st, uint32_t pc, bool host_starved, uint32_t ticks, uint64_t window_ticks, int max_run) {
  if (window_ticks == 0 || max_run <= 0) {
    return false; // detection disabled by configuration
  }
  st.window_ticks += ticks;
  if (st.window_ticks < window_ticks) {
    return false;
  }
  st.window_ticks = 0;

  constexpr uint32_t kRegionMask =
      ~0x7FFFFu; // same ±512KB region (measured: Vagrant's movie-wait chain spans ~100KB across dispatcher blocks)
  const bool same_region = st.anchor != 0 && (pc & kRegionMask) == (st.anchor & kRegionMask);
  if (!host_starved) {
    // Host got its turn: healthy frame loop, whatever the code is doing. Full reset.
    st.anchor = pc;
    st.run = 0;
    return false;
  }
  if (!same_region) {
    // Starved but MOVED: execution is walking other functions — forward progress of a kind, but
    // also the first decision of a fresh candidate run anchored HERE (a migrating spin must not
    // get a free ride by hopping regions every window).
    st.anchor = pc;
    st.run = 1;
    return st.run >= max_run;
  }
  st.run = std::min(st.run + 1, max_run + 1); // saturate; never overflow
  return st.run >= max_run;
}

// 0x80085BB0 FUN_80085bb0 VSyncCallback(func): no-op. The original routes the per-vblank
// callback through the libapi interrupt vector we don't model; we don't deliver preemptive
// VBlank IRQs at all — the game's vblank busy-waits are ported to PC behavior natively
// (see games_tomba2.c), so registering the callback is unnecessary and its unmodeled-vector
// deref is skipped. Was ov_vsync_callback (taxi-in via c->r[4]; the callback ptr arg is
// unused here, so no arg on the method).
void Timing::vsyncCallback() {
  game->core.r[V0] = 0;
}

// Deliver the VBlank event to whichever class the game opened it under (RCnt3 vblank, or the
// libapi vblank class); broad spec so any opened+enabled vblank EvCB matches.
static void deliver_vblank_events(Core *c) {
  c->game->hle.deliverEvent(0xF2000003u, 0xFFFFFFFFu);
  c->game->hle.deliverEvent(0xF0000001u, 0xFFFFFFFFu);
}

// 0x80085900 FUN_80085900 = libetc VSync(mode) reached via c->r[A0]:
//   mode < 0  -> return current vblank count (query, no wait)
//   mode == 1 -> return HBlank-clocked root-counter delta since the last wait (query, no wait)
//   mode == 0 -> wait one vblank; mode > 1 -> wait `mode` vblanks. Advance the frame clock.
// Currently unreachable — sync_overrides traps VSync (all pacing is PC-native). Kept for RE.
void Timing::vsync() {
  Core *c = &game->core;
  int32_t mode = (int32_t)c->r[A0];
  if (mode < 0) {
    c->r[V0] = vblank;
  } else if (mode == 1) {
    c->r[V0] = static_cast<uint16_t>(hSyncCounter() - mVSyncHSyncBaseline);
  } else {
    vblank += (mode == 0) ? 1u : (uint32_t)mode;
    c->r[V0] = vblank;
    deliver_vblank_events(c);
    mVSyncHSyncBaseline = hSyncCounter();
  }
  c->mem_w32(VBLANK_COUNT, vblank);
}

// Advance the canonical libetc VSync counter once per native frame. The PC-native frame loop owns
// timing (one logic frame == one vblank), and VSync(0) is trapped (sync_overrides) so Timing::vsync
// never runs — meaning DAT_800abde0 would otherwise stay 0 forever. Native code reimplements its
// own paced logic and ignores this counter, but RECOMP code (full-PSX core in SBS, and any still-
// recomp leaf) reads DAT_800abde0 to pace animations/idle timers; if it never ticks, those tasks
// freeze in place (SBS core-B title-menu freeze). Bump it every frame for ALL cores so the recomp
// timebase advances.
void Timing::frameTick() {
  vblank += 1u;
  game->core.mem_w32(VBLANK_COUNT, vblank);
}

// ---- vsyncHle: libetc VSync(mode) for DIRECT whole-program runtimes ------------------------------
//
// A port whose PC-native frame loop owns all timing TRAPS VSync (vsync_trap above; Tomba!2). A
// direct runtime running the WHOLE guest program cannot: its retail code CALLS VSync and consumes
// the result — the query form feeds deadline arithmetic (Tekken 3's CdSync spins on VSync(-1)
// against a count + 0x3C0 deadline), so the counter must move with emulated display fields or the
// wait can never end. Unlike the trap policy this is the FAITHFUL leaf, derived from the same RE as
// Timing::vsync() but sourced from EmulatedTime (which advances with guest instruction ticks) so it
// works before any presenter exists:
//   mode < 0   -> $v0 = display fields elapsed (query, no wait); mirror written to DAT_…abde0.
//   mode >= 1  -> consume `mode` (or one, for 0) display field(s) of emulated time, then answer as
//                 the query form. Waiting IS consuming the field interval on real hardware.
// The mode == 1 hblank-delta form stays answered by the caller-facing vsync() semantics via the
// hsync counter; both share mEmulatedTime so they cannot disagree.

uint32_t Timing::emulatedDisplayFields(bool pal) const {
  const uint64_t hsyncs = mEmulatedTime.hSyncCount(field_rate_millihz(pal), display_lines_per_field(pal));
  return static_cast<uint32_t>(hsyncs / display_lines_per_field(pal));
}

void Timing::vsyncHle(Core *c) {
  Timing &t = c->game->timing;
  const bool pal = c->game->gpu.s_disp_pal != 0;
  int32_t mode = static_cast<int32_t>(c->r[A0]);
  if (mode < 0) {
    t.vblank = t.emulatedDisplayFields(pal);
    c->r[V0] = t.vblank;
    c->mem_w32(VBLANK_COUNT, t.vblank);
    lucent::debug("vsync", "query -> {} fields (ticks {})", t.vblank, t.emulatedCpuTicks());
    return;
  }
  const uint32_t fields = (mode == 0) ? 1u : static_cast<uint32_t>(mode);
  if (!t.advanceDisplayFields(static_cast<int>(fields), 1, field_rate_millihz(pal))) {
    lucent::warn("plat-hle", "VSync({}) field advance refused (invalid cadence)", mode);
  }
  if (mode != 1) { // the delta form reports hsyncs, not the shared counter
    t.vblank += fields;
    deliver_vblank_events(c);
  }
  c->r[V0] = (mode == 1) ? static_cast<uint16_t>(t.hSyncCounter() - t.mVSyncHSyncBaseline) : t.vblank;
  t.mVSyncHSyncBaseline = t.hSyncCounter();
  c->mem_w32(VBLANK_COUNT, t.vblank);
}

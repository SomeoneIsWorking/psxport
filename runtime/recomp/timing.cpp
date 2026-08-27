// Native display-time source. The host frame loop advances fields through frame_pacer.cpp and mirrors
// one canonical counter tick through frameTick(). libetc VSync is not implemented here: every guest
// wait/query is trapped by PlatformHle because only the native loop may own field advancement.
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

enum { V0 = 2 };
#define VBLANK_COUNT 0x800ABDE0u // DAT_800abde0: libetc VSync counter (FUN_80085900 returns it)

uint64_t Timing::readEmulatedCpuTicks(void *context) {
  return static_cast<Timing *>(context)->mEmulatedTime.nowTicks();
}

// CDC drive clock: wall-locked (see timing.h). Nominal-rate ticks since bind; no state cached.
uint64_t Timing::readWallLockedCdcTicks(void *context) {
  const auto *t = static_cast<const Timing *>(context);
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  const uint64_t ns =
      static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec) - t->wallClockOriginNs;
  // The 128-bit intermediate overflows only after ~570 years of uptime.
  return static_cast<uint64_t>((static_cast<unsigned __int128>(ns) * kNominalPsxCpuHz) / 1'000'000'000ull);
}

void Timing::bindCdcClock(CdcState *cdc) {
  timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  wallClockOriginNs = static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
  cdc_bind_tick_source(cdc, this, readWallLockedCdcTicks);
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
// measured live as Vagrant's movie-wait spinning inside a single resident libcd poll body while
// CD sectors flowed (issue #25; the concrete pc lives in that issue's record, not here).
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

// Advance the canonical libetc VSync counter once per native frame. The PC-native frame loop owns
// timing (one logic frame == one vblank). Native code ignores this compatibility mirror, but finite
// recomp leaves may read it for animations/idle timers. They may not call VSync to advance or query it.
void Timing::frameTick() {
  vblank += 1u;
  game->core.mem_w32(VBLANK_COUNT, vblank);
}

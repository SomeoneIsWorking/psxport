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
  game->sio.service(mEmulatedTime.nowTicks());
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
  game->sio.service(mEmulatedTime.nowTicks());
  raiseVBlank(consumeCompletedDisplayFields(static_cast<uint32_t>(fields), static_cast<uint32_t>(parts)));
  serviceCdc();
  return true;
}

namespace {

unsigned __int128 gcd128(unsigned __int128 a, unsigned __int128 b) {
  while (b != 0) {
    const unsigned __int128 remainder = a % b;
    a = b;
    b = remainder;
  }
  return a;
}

} // namespace

uint32_t Timing::consumeCompletedDisplayFields(uint32_t fields, uint32_t parts) {
  const unsigned __int128 common = gcd128(mDisplayFieldPhaseDenominator, parts);
  const unsigned __int128 leftScale = parts / common;
  const unsigned __int128 rightScale = mDisplayFieldPhaseDenominator / common;
  const unsigned __int128 denominator = mDisplayFieldPhaseDenominator * leftScale;
  const unsigned __int128 numerator = mDisplayFieldPhaseNumerator * leftScale + fields * rightScale;
  const unsigned __int128 completed = numerator / denominator;
  mDisplayFieldPhaseNumerator = numerator % denominator;
  mDisplayFieldPhaseDenominator = denominator;
  if (mDisplayFieldPhaseNumerator == 0) {
    mDisplayFieldPhaseDenominator = 1;
  } else {
    const unsigned __int128 reduction = gcd128(mDisplayFieldPhaseNumerator, mDisplayFieldPhaseDenominator);
    mDisplayFieldPhaseNumerator /= reduction;
    mDisplayFieldPhaseDenominator /= reduction;
  }
  return static_cast<uint32_t>(completed);
}

// Every display field ends in a VBlank, and the display controller raises I_STAT bit 0 for it
// whether or not anything is listening. The port owns FRAME PACING natively and traps every guest
// VSync wait (see sync_overrides.cpp) — that is unchanged. What this asserts is only the INTERRUPT
// EDGE, which is a separate thing a guest can own and which several drivers do: Crash Bash patches
// its own pad engine into the kernel C0 table and does the whole controller handshake inside the
// interrupt element it registers, so with no VBlank edge its verifier never ran, no SIO transfer
// ever started, and no button state reached guest RAM (crashbash issue 0019).
//
// It costs nothing where nobody is listening: Hle::irqPoll delivers only when the guest has both
// unmasked bit 0 in I_MASK and registered a chain element, and the bit stays latched until the
// guest acknowledges it exactly as hardware does. A title that leaves VBlank masked — every one
// whose vblank work the port already owns natively — sees no behavior change at all.
void Timing::raiseVBlank(uint32_t fields) {
  if (fields == 0) {
    return;
  }
  // A latch, not a count: a guest that has not acknowledged the previous edge sees one pending
  // VBlank, which is what the hardware bit does. Missed edges are the guest's own problem.
  game->hle.i_stat |= 1u;
  game->core.pending_work |= Core::PW_IRQ; // arm the per-function-entry delivery gate
}

uint64_t Timing::emulatedCpuTicks() const {
  return mEmulatedTime.nowTicks();
}

// ---- root counter 2 ------------------------------------------------------------------------
// Counts the same emulated CPU time everything else here is measured in, so it advances exactly
// when the guest executes instructions or waits out a display field, and it wraps at 16 bits like
// the hardware register. Mode bit 9 selects system-clock/8. With sync enabled (bit 0), sync modes
// 0 and 3 stop timer 2 while modes 1 and 2 free-run; without sync it always runs.
//
// Crash Bash's pad driver is why this exists (crashbash issue 0019): its inter-byte delays and its
// per-byte /ACK timeout are both `latch counter 2, spin until the delta exceeds N` (guest
// 0x8003C688 / 0x8003C6A8), so with the register unmapped and reading 0 the delta was always 0,
// the budget was never reached, and the SIO transfer hung in its first delay forever.
//
// NOT modelled, deliberately, because nothing has yet demanded it: the target/wrap IRQ (mode bits
// 4-5 and 10, I_STAT bit 6) and the reached-target/reached-max status bits 11-12. A guest that
// waits on a timer INTERRUPT still gets nothing, and will hang visibly rather than being handed a
// fabricated event.
uint16_t Timing::rootCounter2() const {
  const uint32_t syncMode = (rootCounter2Mode >> 1u) & 0x3u;
  const bool stopped = (rootCounter2Mode & 0x1u) != 0 && (syncMode == 0u || syncMode == 3u);
  if (stopped) {
    return rootCounter2BaseValue;
  }
  const unsigned shift = (rootCounter2Mode & 0x200u) ? 3u : 0u; // bit 9: system clock / 8
  const uint64_t count = rootCounter2BaseValue + ((mEmulatedTime.nowTicks() - rootCounter2OriginTicks) >> shift);
  if (rootCounter2Mode & 0x008u) { // reset on target: the programmed target is the wrap period
    const uint32_t period = rootCounter2Target > 1u ? rootCounter2Target : 1u;
    return static_cast<uint16_t>(count % period);
  }
  return static_cast<uint16_t>(count);
}

void Timing::rootCounter2Write(uint32_t reg, uint32_t v) {
  const uint64_t now = mEmulatedTime.nowTicks();
  switch (reg & 0xCu) {
  case 0x0: { // counter value: writing it restarts counting from that value
    rootCounter2BaseValue = static_cast<uint16_t>(v);
    rootCounter2OriginTicks = now;
    return;
  }
  case 0x4: // mode: a write resets the counter to zero, as on hardware
    rootCounter2Mode = v & 0x3FFu;
    rootCounter2BaseValue = 0;
    rootCounter2OriginTicks = now;
    return;
  case 0x8:
    rootCounter2Target = v & 0xFFFFu;
    return;
  default:
    return;
  }
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

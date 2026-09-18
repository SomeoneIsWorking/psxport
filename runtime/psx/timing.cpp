// Native display-time source. The host frame loop advances fields through frame_pacer.cpp and
// advances one title-neutral counter through frameTick(). A title that mirrors it into guest RAM
// owns the guest address and write order at its frame boundary.
#include "cdc_state.h"
#include "core.h"
#include "field_rate.h"
#include "game.h"
#include "host_turn.h"
#include <algorithm> // std::min
#include <lucent/log.h>
#include <stdio.h>
#include <stdlib.h>

enum { V0 = 2 };

uint64_t Timing::readEmulatedCpuTicks(void *context) {
  return static_cast<Timing *>(context)->mEmulatedTime.nowTicks();
}

void Timing::bindCdcClock(CdcState *cdc) {
  cdc_bind_tick_source(cdc, this, readEmulatedCpuTicks);
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
  // A delivered display field is the host field clock's boundary. Re-anchor it here, before the
  // field's guest callbacks run: the advance above lands exactly on the deadline this field was
  // owed at, and a callback that accounted guest time against the stale deadline would request
  // the very field it is inside.
  psx::cpu::notifyDisplayField(game->core);
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

// Advance the host field count once per title-owned native frame. Guest memory layout is title
// policy: a frame driver that needs a libetc compatibility mirror writes its measured address.
void Timing::frameTick() {
  vblank += 1u;
}

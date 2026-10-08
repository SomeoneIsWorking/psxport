// pad_input.cpp — the PSX controller the guest reads: the effective active-low mask, the per-VBlank
// digital packet, the REPL drive, and the deterministic record/replay session.
//
// THE GUEST GETS A NATIVE PACKET, not its own SIO pad read. Titles that bit-bang the port (Tomba! 2's
// libpad driver is the measured case) depend on the SIO IRQ, which this runtime never raises, so the
// guest's own per-VBlank read spins on the status poll and reports no pad. This service does what that
// read would have done: once per frame, before the guest reads input, write the standard digital packet
// into every registered slot buffer.
//
// LIMITATION (measured 2026-06-14): that guest read lives at 0x80003A4C, BELOW Tomba! 2's MAIN.EXE text
// range, in the boot-stub / resident low-text module. Until the image catalog identifies that resident
// module no image-scoped override can own the address, so no address-only registration is attempted and
// no missing registration is silently accepted — the native per-frame service supplies the buffers.
#include "pad_input.h"
#include "cfg.h"
#include "config_vars.h"
#include "core.h"
#include "game.h"                // class Pad lives on Game (c->game->pad)
#include "game_runtime.h"        // GameRuntime::inputPhase / guestPadBufferLayout
#include "gpu_native_internal.h" // gpu_scene_dump_now / gpu_disp_dump_now / gpu_otattr_dump_now
#include "gpu_vk.h"              // gpu_vk_shot, gpu_vk_windowed — the windowed/headless discriminator
#include "guest_pad_buffer_layout.h"
#include <lucent/log.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The title's input phase for this pad frame. A title that declares none (the GameRuntime default,
// and every legacy adapter) answers kUnkeyedPhase, and its recordings are absolute from boot.
uint64_t Pad::currentPhase(Core &core) const {
  return core.runtime ? core.runtime->inputPhase(core) : psx::input::kUnkeyedPhase;
}

void Pad::init() {
  buttons = psx::input::kNoButtons;
}

void Pad::setButtons(uint16_t mask) {
  buttons = mask;
}

// Write the standard digital auto-pad packet into a buffer the game polls: status, pad id, and the
// button mask low/high, active-low. `buf` must have room for at least 4 bytes.
void Pad::fillBuffer(uint8_t *buf) {
  if (!buf) {
    return;
  }
  buf[0] = 0x00;                             // status: pad present / read ok
  buf[1] = 0x41;                             // id: digital controller (1 halfword of data)
  buf[2] = (uint8_t)(buttons & 0xFF);        // button mask low  (active-low)
  buf[3] = (uint8_t)((buttons >> 8) & 0xFF); // button mask high (active-low)
}

// The host mask, plus the P / '.' / B debug keys, whose ACTION belongs to the debug channel. Nothing
// frame-indexed is touched here (no record/replay tick, no frame counter, no tap countdown), which is
// what lets a paused or a movie-blocked turn keep reading the host without disturbing a recording.
//
// A leg with NO window has no host input, and this pump must not overwrite what the pad already
// resolves: in such a leg a forced, replayed, restored or host-driven mask is the only input there is,
// and clobbering it with "nothing pressed" would discard exactly what the leg depends on. The drain
// still runs, because a window close has to end the run from any leg.
void Pad::pollHostInput() {
  const bool windowed = gpu_vk_windowed() != 0;
  const uint16_t hostMask = mHost.poll(windowed);
  if (!windowed) {
    return;
  }
  buttons = hostMask;
  if (mHost.takePauseRequest()) {
    game->dbg_server.togglePause();
  }
  if (mHost.takeFrameStepRequest()) {
    game->dbg_server.addStep(1);
  }
  if (mHost.takeBugReportRequest()) {
    game->bugReport.request();
  }
}

void Pad::overridesInit() {
  init();
}

// REPL pad control (native-port -repl): a held active-low mask + a tap countdown, applied by
// serviceFrame() so the interactive driver can press/hold/tap buttons.
void Pad::driveHold(uint16_t activeLowMask) {
  repl_on = 1;
  repl_hold = activeLowMask;
}

void Pad::driveTap(uint16_t activeLowMask, int nframes) {
  repl_on = 1;
  repl_tap = activeLowMask;
  repl_tap_n = nframes;
}

// Relinquish REPL pad control entirely, so neither a held mask nor a stale no-buttons mask keeps
// overriding host/FORCE input.
void Pad::driveRelease() {
  repl_on = 0;
  repl_hold = psx::input::kNoButtons;
  repl_tap = psx::input::kNoButtons;
  repl_tap_n = 0;
}

// PSXPORT_GUEST_POKE=<addr>:<val>[:<width>],... — rewrite these guest locations EVERY frame, on
// whatever leg is running. Width is 1 (default), 2 or 4 bytes; addr and val are hex. A malformed spec
// refuses loudly rather than poking nothing.
//
// It writes GUEST state at the platform frame tick, not at a render point, so a comparison's oracle
// leg (which draws from the guest OT) sees it too. That makes it canon-changing: on in BOTH legs of a
// comparison or in NEITHER, and never in a byte-compare.
void Pad::applyGuestPoke(Core *c) {
  if (!mPokeInit) {
    mPokeInit = 1;
    const char *spec = cfg_str("PSXPORT_GUEST_POKE");
    if (spec) {
      char buf[512];
      snprintf(buf, sizeof buf, "%s", spec);
      int bad = 0;
      for (char *t = strtok(buf, ","); t; t = strtok(nullptr, ",")) {
        unsigned addr = 0, val = 0, width = 1;
        const int got = sscanf(t, "%x:%x:%u", &addr, &val, &width);
        if (got < 2 || (width != 1 && width != 2 && width != 4) || mPokeN >= kPokeMax) {
          bad++;
          continue;
        }
        mPoke[mPokeN].addr = addr;
        mPoke[mPokeN].val = val;
        mPoke[mPokeN].width = width;
        mPokeN++;
      }
      if (bad || !mPokeN) {
        lucent::error("poke",
                      "PSXPORT_GUEST_POKE={}: REFUSED {} malformed entr(y/ies), accepted {} — "
                      "expected <hexaddr>:<hexval>[:1|2|4] separated by commas. Nothing is being "
                      "poked for the rejected ones, so do not read this run as forcing them.",
                      spec,
                      bad,
                      mPokeN);
      }
      for (int i = 0; i < mPokeN; i++) {
        lucent::info("poke",
                     "every frame: [{:08X}] = {:X} ({} byte{})",
                     mPoke[i].addr,
                     mPoke[i].val,
                     mPoke[i].width,
                     mPoke[i].width == 1 ? "" : "s");
      }
    }
  }
  for (int i = 0; i < mPokeN; i++) {
    if (mPoke[i].width == 1) {
      c->mem_w8(mPoke[i].addr, (uint8_t)mPoke[i].val);
    } else if (mPoke[i].width == 2) {
      c->mem_w16(mPoke[i].addr, (uint16_t)mPoke[i].val);
    } else {
      c->mem_w32(mPoke[i].addr, mPoke[i].val);
    }
  }
}

namespace {

// Where this title keeps the packet the guest polls. A title may expose fixed buffers, a driver-owned
// pointer table, or both; a non-null driver pointer wins and the fixed address is the fallback.
GuestPadBufferLayout resolveGuestPadBufferLayout(const Core &core) {
  if (core.cfg) {
    return {
        .slot0Buffer = core.cfg->padSlot0Buf,
        .slot1Buffer = core.cfg->padSlot1Buf,
        .slotPointerTable = core.cfg->padSlotPtrTable,
        .slotPointerStride = core.cfg->padSlotPtrStride ? core.cfg->padSlotPtrStride : 4u,
    };
  }
  if (core.runtime) {
    if (const GuestPadBufferLayout *layout = core.runtime->guestPadBufferLayout()) {
      GuestPadBufferLayout resolved = *layout;
      if (!resolved.slotPointerStride) {
        resolved.slotPointerStride = 4u;
      }
      return resolved;
    }
  }
  return {};
}

} // namespace

void Pad::serviceFrame() {
  Core *c = &game->core;
  const bool windowed = gpu_vk_windowed() != 0;
  pollHostInput(); // host keyboard/controllers, plus the P / '.' debug keys

  if (!mForceInit) { // headless test hook: pulse an active-low mask
    const char *force = cfg_str("PSXPORT_FORCE_BUTTONS");
    if (force) {
      mForceOn = 1;
      mForceMask = (uint16_t)strtoul(force, 0, 16);
    }
    // Second phase: HOLD a mask continuously from PSXPORT_FORCE_HOLD_AT onward (overrides the
    // pulse). Lets a headless run reach a state via pulsed Start, then hold a direction in-level
    // (a held direction is what the game reads for movement) — for interactivity testing.
    const char *hold = cfg_str("PSXPORT_FORCE_HOLD");
    if (hold) {
      mForceOn = 1;
      mHoldMask = (uint16_t)strtoul(hold, 0, 16);
      const char *at = cfg_str("PSXPORT_FORCE_HOLD_AT");
      mHoldAt = at ? strtoul(at, 0, 0) : 0;
    }
    mForceInit = 1;
  }
  // Pulse the forced buttons (pressed 8 frames, released 24) so each press is a fresh EDGE the game's
  // current&~prev input logic sees; a continuous hold would edge only once. Once past FORCE_HOLD_AT,
  // hold FORCE_HOLD continuously instead (a held direction is what a game reads for movement).
  // PSXPORT_FORCE_STOP_AT=N ceases ALL forced input at frame N, so a run can pulse Start through
  // attract/menu/intro to a target scene and then go hands-off.
  if (mStopAt == -2) {
    const char *e = cfg_str("PSXPORT_FORCE_STOP_AT");
    mStopAt = e ? atol(e) : -1;
  }
  if (mForceOn && !(mStopAt >= 0 && (long)mFc >= mStopAt)) {
    if (mHoldMask != psx::input::kNoButtons && mFc >= mHoldAt) {
      setButtons(mHoldMask);
    } else {
      setButtons((mFc % 32u) < 8u ? mForceMask : psx::input::kNoButtons);
    }
  }
  // REPL pad control: a tap (countdown) overrides the held mask while active. The effective REPL mask is
  // kept aside so a replay in progress MERGES it (below) instead of swallowing it — an explicit press is
  // user intent NOW. Determinism is unaffected when no REPL command is issued.
  uint16_t repl_mask = psx::input::kNoButtons;
  if (repl_on) {
    if (repl_tap_n > 0) {
      repl_mask = repl_tap;
      repl_tap_n--;
    } else {
      repl_mask = repl_hold;
    }
    setButtons(repl_mask);
  }
  mFc++;

  // The HOST's claim on the player's input, applied BEFORE the record/replay service so the guest, the
  // recording and a replay all see the same mask: a recording taken under the claim records what the
  // guest was actually given, not the press the host consumed.
  if (mPlayerInputSuppressed) {
    buttons = psx::input::kNoButtons;
  }

  // ---- INPUT RECORD / REPLAY (deterministic pad capture) ---------------------------------------
  // The engine has no wall-clock/RNG, so the per-frame final pad mask fully determines a run from a
  // given card image. The session (pad_record_replay.h) records the finalized mask with the title's
  // input phase every frame, and a replay/resume overrides the mask AFTER every other input source so
  // it wins. Recordings are phase-keyed (pad_phase_replay.h): a recorded press is replayed at its
  // offset from the entry of the phase it was recorded in, not at an absolute frame from boot.
  {
    if (!mSession.configured()) {
      mSession.configure(psx::input::PadSessionConfig{
          .recordPath = psx::config::cv_pad_record.get(),
          .replayPath = psx::config::cv_pad_replay.get(),
          .resumePath = psx::config::cv_pad_resume.get(),
          .windowed = windowed,
          .liveInputOnly = mLiveInputOnly,
          .card =
              [this] {
                return game->memcard.snapshot();
              },
      });
    }
    buttons = mSession.service(currentPhase(*c), buttons, repl_mask);
    // Latch edges only after every input source has resolved to the mask the guest receives. Sampling
    // host input earlier would miss replay/REPL presses or expose an edge for a mask later replaced.
    sampleButtonEdges();
    const uint32_t rec_fc = mSession.frameIndex(); // the pad-frame axis the schedules below index
    // PSXPORT_PAD_SHOT_AT=f0,f1,... : during replay, screenshot at these EXACT replay (pad) frame
    // indices to scratch/screenshots/padshot_<frame>.ppm. The pad-frame axis (rec_fc) is the faithful
    // one (gpu_frame_no drifts because boot/FMV presents extra frames), so this captures a
    // deterministic visual timeline of a replayed session. Taken here, after the mask is applied for
    // THIS frame.
    uint32_t *shot_at = mShotAt;
    int &shot_n = mShotN;
    if (!mShotInit) {
      mShotInit = 1;
      const char *s = cfg_str("PSXPORT_PAD_SHOT_AT");
      if (s) {
        char buf[512];
        snprintf(buf, sizeof buf, "%s", s);
        for (char *t = strtok(buf, ","); t && shot_n < 64; t = strtok(nullptr, ",")) {
          shot_at[shot_n++] = (uint32_t)strtoul(t, 0, 0);
        }
      }
    }
    for (int i = 0; i < shot_n; i++) {
      if (shot_at[i] == rec_fc) {
        char p[96];
        snprintf(p, sizeof p, "scratch/screenshots/padshot_%u.ppm", rec_fc);
        gpu_vk_shot(c, p);
        lucent::info("padrec", "shot at replay-frame {} -> {}", rec_fc, p);
      }
    }
    // PSXPORT_PAD_DUMP_AT=f0,f1,... : dump 2MB guest RAM (+.spad) at these REPLAY frames to
    // scratch/bin/padram_<f>.bin — for A/B diffing scene state (e.g. village vs hut interior).
    uint32_t *dump_at = mDumpAt;
    int &dump_n = mDumpN;
    if (!mDumpInit) {
      mDumpInit = 1;
      const char *s = cfg_str("PSXPORT_PAD_DUMP_AT");
      if (s) {
        char buf[256];
        snprintf(buf, sizeof buf, "%s", s);
        for (char *t = strtok(buf, ","); t && dump_n < 32; t = strtok(nullptr, ",")) {
          dump_at[dump_n++] = (uint32_t)strtoul(t, 0, 0);
        }
      }
    }
    for (int i = 0; i < dump_n; i++) {
      if (dump_at[i] == rec_fc) {
        char p[96];
        snprintf(p, sizeof p, "scratch/bin/padram_%u.bin", rec_fc);
        FILE *fp = fopen(p, "wb");
        if (fp) {
          fwrite(c->ram, 1, 0x200000, fp);
          fclose(fp);
        }
        char sp[112];
        snprintf(sp, sizeof sp, "%s.spad", p);
        FILE *spf = fopen(sp, "wb");
        if (spf) {
          fwrite(c->scratch, 1, sizeof c->scratch, spf);
          fclose(spf);
        }
        // also dump the classified display list (which prims/passes drew this frame) — pins the culprit pass.
        char sc[100];
        snprintf(sc, sizeof sc, "scratch/bin/padscene_%u.txt", rec_fc);
        FILE *scf = fopen(sc, "w");
        if (scf) {
          gpu_scene_dump_now(c, scf);
          fclose(scf);
        }
        lucent::info("padrec", "dumpram+scene at replay-frame {} -> {}", rec_fc, p);
      }
    }
    // PSXPORT_PAD_TRACE=lo-hi : log the transition/scene markers EVERY replay frame in [lo,hi]
    // (pad-frame indexed — the faithful axis). Finds which field moves when the player walks into the
    // hut (the seamless sub-scene transition). All fixed-address globals; sm = *0x1f800138.
    uint32_t &trace_lo = mTraceLo;
    uint32_t &trace_hi = mTraceHi;
    if (!mTraceInit) {
      mTraceInit = 1;
      const char *s = cfg_str("PSXPORT_PAD_TRACE");
      if (s) {
        unsigned a = 0, b = 0;
        if (sscanf(s, "%u-%u", &a, &b) == 2) {
          trace_lo = a;
          trace_hi = b;
        } else if (sscanf(s, "%u", &a) == 1) {
          trace_lo = 0;
          trace_hi = a;
        }
      }
    }
    if (rec_fc >= trace_lo && rec_fc <= trace_hi) {
      uint32_t sm = c->mem_r32(0x1f800138u);
      lucent::info("padtr",
                   "f{:<4} bf839={:02X} bf80f={:02X} i236={:02X} sm4a={} sm4c={} sm4e={} scene={} bf870={:02X} "
                   "bf809={:02X} bf89c={:02X} e7e68={:08X} tX={} tZ={}",
                   rec_fc,
                   c->mem_r8(0x800bf839u),
                   c->mem_r8(0x800bf80fu),
                   c->mem_r8(0x1f800236u),
                   c->mem_r16(sm + 0x4a),
                   c->mem_r16(sm + 0x4c),
                   c->mem_r16(sm + 0x4e),
                   c->mem_r32(0x800be258u),
                   c->mem_r8(0x800bf870u),
                   c->mem_r8(0x800bf809u),
                   c->mem_r8(0x800bf89cu),
                   c->mem_r32(0x800e7e68u),
                   c->mem_r16s(0x800fe916u),
                   c->mem_r16s(0x800fe91eu));
    }
  }

  applyGuestPoke(c);

  // A BIOS InitPAD2/StartPAD2 user receives packets only while its PadCardIrq handler is enqueued and
  // the work-area pad-enable flag is set. Title-native SIO drivers never enter this BIOS lifecycle and
  // retain their existing host packet path.
  if (!game->hle.biosPadShouldService()) {
    return;
  }

  uint8_t pk[4];
  fillBuffer(pk);
  const GuestPadBufferLayout layout = resolveGuestPadBufferLayout(*c);
  const uint32_t bufs[2] = {layout.slot0Buffer, layout.slot1Buffer};
  for (int slot = 0; slot < 2; slot++) {
    // Only consult the driver's table when the port HAS one. Reading it unconditionally means a game
    // with no known table reads guest address 0 (and 0+stride) and calls whatever garbage is there a
    // buffer pointer — writing the pad packet into an arbitrary address.
    uint32_t b =
        layout.slotPointerTable ? c->mem_r32(layout.slotPointerTable + (uint32_t)slot * layout.slotPointerStride) : 0u;
    if (!b) {
      b = bufs[slot]; // fall back to the fixed buffer
    }
    if (!b) {
      continue; // neither known: nothing to fill
    }
    if (slot == 1 && !mSlot1Connected) {
      c->mem_w8(b, 0xFF);
    } else {
      for (int i = 0; i < 4; i++) {
        c->mem_w8(b + i, pk[i]);
      }
    }
  }
}

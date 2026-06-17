// Native controller / PAD input for the Tomba!2 PC port.
// -----------------------------------------------------------------------------
// RE summary (see report / docs): Tomba!2 does NOT use the BIOS auto-pad
// services (B0:0x12 InitPAD .. 0x16). MAIN.EXE statically links Sony's libpad/_pad
// SIO driver into its own low text (0x80003748..0x80004xxx) and bit-bangs the
// controller port directly:
//   * FUN_800040c4(buf1,len1,buf2,len2)  = InitPAD-equiv: stores the per-slot pad
//     buffer pointers at the driver globals _DAT_0000aec8 (slot 0) and
//     _DAT_0000aecc (slot 1) [base 0x0000aec8, stride 4 bytes per slot], zeroes them.
//   * FUN_800041b8 / FUN_8000435c        = StartPAD-equiv: hook the VBlank/root-counter
//     IRQ so that, each VBlank, the SIO read fills those buffers.
//   * FUN_80003a4c(slot)                 = the per-VBlank read: bit-bangs SIO
//     (0x1F801040 data / 0x1F801044 status / 0x1F80104A control), runs the
//     0x01,0x42,... controller handshake, and writes the standard pad packet into
//     the slot's buffer:
//         buf[0] = status   (0x00 = pad present/ok; 0xFF = no pad / error)
//         buf[1] = pad id    (0x41 = digital; high nibble = #halfwords of data,
//                             low nibble counts; 0x41 => 1 halfword => 2 data bytes)
//         buf[2] = button mask low  byte   (active-low: 0 = pressed)
//         buf[3] = button mask high byte
//         buf[4..] = analog (only for analog/2-halfword ids; unused for 0x41)
//
// Because that SIO read depends on real hardware + the SIO IRQ (which the no-IRQ
// HLE runtime never raises), its recomp body would spin on the _DAT_1f801044
// status poll and bail via its timeout (LAB_80003da4 -> mark "no pad"). The clean
// PC-native fix (recomp-overrides pattern): override FUN_80003a4c to write our
// native packet straight into the registered slot buffer instead of emulating SIO.
//
// This module is self-contained (no recomp/runtime headers required) so it can be
// unit-tested standalone. The PM wires it into hle.c/mem.c (see report).

#include <stdint.h>

// PSX digital button bits, active-low (0 = pressed). Default = nothing pressed.
#define PAD_NONE 0xFFFFu

static uint16_t s_buttons = PAD_NONE;  // current host button state (active-low)

void pad_init(void) {
  s_buttons = PAD_NONE;
}

// Host/PM feeds button state in the PSX active-low layout (0 bit = pressed).
void pad_set_buttons(uint16_t mask) {
  s_buttons = mask;
}

uint16_t pad_buttons(void) {
  return s_buttons;
}

// Write the standard digital auto-pad packet into a buffer the game polls.
// `buf` must have room for at least 4 bytes. Layout matches FUN_80003a4c's output
// for a connected digital pad (id 0x41).
void pad_fill_buffer(uint8_t* buf) {
  if (!buf) return;
  buf[0] = 0x00;                       // status: pad present / read ok
  buf[1] = 0x41;                       // id: digital controller (1 halfword of data)
  buf[2] = (uint8_t)(s_buttons & 0xFF);        // button mask low  (active-low)
  buf[3] = (uint8_t)((s_buttons >> 8) & 0xFF); // button mask high (active-low)
}

// --- Optional SDL host input ------------------------------------------------
// DEFAULT (no host input) works headlessly: s_buttons stays 0xFFFF (no presses)
// unless pad_set_buttons() / pad_poll_sdl() updates it. SDL is compiled in only
// under PSXPORT_SDL, matching gpu_native.c's optional-SDL style.
#ifdef PSXPORT_SDL
#include <SDL.h>

// Map keyboard + first connected gamepad to the PSX button mask. Call once per
// frame (after SDL_PumpEvents elsewhere, or it pumps here). Builds an ACTIVE-LOW
// mask: a bit is cleared when its control is pressed.
void pad_poll_sdl(void) {
  SDL_PumpEvents();
  uint16_t mask = PAD_NONE;

  const Uint8* ks = SDL_GetKeyboardState(NULL);
  if (ks) {
    #define KEYDOWN(sc) (ks[(sc)] != 0)
    if (KEYDOWN(SDL_SCANCODE_UP)     || KEYDOWN(SDL_SCANCODE_W)) mask &= ~0x0010u; // Up
    if (KEYDOWN(SDL_SCANCODE_RIGHT)  || KEYDOWN(SDL_SCANCODE_D)) mask &= ~0x0020u; // Right
    if (KEYDOWN(SDL_SCANCODE_DOWN)   || KEYDOWN(SDL_SCANCODE_S)) mask &= ~0x0040u; // Down
    if (KEYDOWN(SDL_SCANCODE_LEFT)   || KEYDOWN(SDL_SCANCODE_A)) mask &= ~0x0080u; // Left
    if (KEYDOWN(SDL_SCANCODE_RETURN))                           mask &= ~0x0008u; // Start
    if (KEYDOWN(SDL_SCANCODE_RSHIFT) || KEYDOWN(SDL_SCANCODE_TAB)) mask &= ~0x0001u; // Select
    if (KEYDOWN(SDL_SCANCODE_K))     mask &= ~0x4000u; // Cross
    if (KEYDOWN(SDL_SCANCODE_L))     mask &= ~0x2000u; // Circle
    if (KEYDOWN(SDL_SCANCODE_I))     mask &= ~0x1000u; // Triangle
    if (KEYDOWN(SDL_SCANCODE_J))     mask &= ~0x8000u; // Square
    if (KEYDOWN(SDL_SCANCODE_Q))     mask &= ~0x0400u; // L1
    if (KEYDOWN(SDL_SCANCODE_E))     mask &= ~0x0800u; // R1
    if (KEYDOWN(SDL_SCANCODE_1))     mask &= ~0x0100u; // L2
    if (KEYDOWN(SDL_SCANCODE_3))     mask &= ~0x0200u; // R2
    // Debug pause / frame-step keys (edge-detected so one keypress = one action). P toggles
    // pause/play; '.' (period) freezes and advances exactly one frame. Handled here because
    // pad_poll_sdl runs every frame in BOTH the running loop and the paused wait. (dbg_server.c)
    { void dbg_toggle_pause(void); void dbg_add_step(int);
      static int prev_p = 0, prev_step = 0;
      int p = KEYDOWN(SDL_SCANCODE_P), st = KEYDOWN(SDL_SCANCODE_PERIOD);
      if (p && !prev_p)   dbg_toggle_pause();
      if (st && !prev_step) dbg_add_step(1);
      prev_p = p; prev_step = st; }
    #undef KEYDOWN
  }

  // First connected game controller (if any), additive with the keyboard.
  for (int i = 0; i < SDL_NumJoysticks(); i++) {
    if (!SDL_IsGameController(i)) continue;
    SDL_GameController* gc = SDL_GameControllerOpen(i);
    if (!gc) continue;
    #define BTN(b) SDL_GameControllerGetButton(gc, (b))
    if (BTN(SDL_CONTROLLER_BUTTON_DPAD_UP))        mask &= ~0x0010u;
    if (BTN(SDL_CONTROLLER_BUTTON_DPAD_RIGHT))     mask &= ~0x0020u;
    if (BTN(SDL_CONTROLLER_BUTTON_DPAD_DOWN))      mask &= ~0x0040u;
    if (BTN(SDL_CONTROLLER_BUTTON_DPAD_LEFT))      mask &= ~0x0080u;
    if (BTN(SDL_CONTROLLER_BUTTON_START))          mask &= ~0x0008u;
    if (BTN(SDL_CONTROLLER_BUTTON_BACK))           mask &= ~0x0001u; // Select
    if (BTN(SDL_CONTROLLER_BUTTON_A))              mask &= ~0x4000u; // Cross
    if (BTN(SDL_CONTROLLER_BUTTON_B))              mask &= ~0x2000u; // Circle
    if (BTN(SDL_CONTROLLER_BUTTON_Y))              mask &= ~0x1000u; // Triangle
    if (BTN(SDL_CONTROLLER_BUTTON_X))              mask &= ~0x8000u; // Square
    if (BTN(SDL_CONTROLLER_BUTTON_LEFTSHOULDER))   mask &= ~0x0400u; // L1
    if (BTN(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))  mask &= ~0x0800u; // R1
    if (BTN(SDL_CONTROLLER_BUTTON_LEFTSTICK))      mask &= ~0x0002u; // L3
    if (BTN(SDL_CONTROLLER_BUTTON_RIGHTSTICK))     mask &= ~0x0004u; // R3
    #undef BTN
    break; // first controller only
  }

  s_buttons = mask;
}
#endif // PSXPORT_SDL

// --- Native pad-read override -----------------------------------------------
// Wired into the recompiled core (kept out of the standalone unit test via
// PSXPORT_PAD_NO_OVERRIDES, mirroring memcard.c). Replaces the per-VBlank SIO
// pad read FUN_80003a4c(slot) so it writes our native packet straight into the
// slot's registered buffer instead of bit-banging SIO (which would spin on the
// _DAT_1f801044 status poll the no-IRQ runtime never satisfies).
//
// IMPORTANT LIMITATION (verified 2026-06-14): FUN_80003a4c lives at 0x80003A4C,
// which is BELOW MAIN.EXE's text range [0x80010000,0x800BE800). MAIN.EXE (the
// recompiler input) does NOT contain that SIO driver — it is part of the
// boot-stub/resident low-text image loaded at runtime. emit.py can only emit
// addresses inside MAIN.EXE, so 0x80003A4C is neither recompiled nor present in
// the dispatch/override table. rec_set_override(0x80003A4Cu, …) below therefore
// resolves to index -1 and is a SILENT NO-OP today; the override will only take
// effect once that driver is part of the recompiler input (or reached via the
// hybrid interpreter with override support). It is wired now so the
// infrastructure is in place and the contract is documented. The boot path does
// not call FUN_80003a4c (no [miss] 0x80003A4C at boot), so this is inert for the
// boot baseline either way. See report / docs/journal.md.
#ifndef PSXPORT_PAD_NO_OVERRIDES
#include "r3000.h"
#include <stdio.h>
#include <stdlib.h>

// Slot-buffer pointer table base in the SIO driver's low-RAM globals: the per-slot
// pad buffer pointer for slot `slot` is at 0x0000AEC8 + slot*4 (FUN_800040c4 stores
// them there; FUN_80003a4c reads them via &DAT_0000aec8 + slot*4).
// FUN_80003a4c(slot): a0 = slot index.
static void ov_pad_read(R3000* c) {
  uint32_t b = mem_r32(0x0000AEC8u + c->r[4] * 4u);   // registered slot buffer ptr
  if (b) {
    uint8_t pk[4];
    pad_fill_buffer(pk);
    for (int i = 0; i < 4; i++) mem_w8(b + i, pk[i]);
  }
  c->r[2] = 0;   // v0 = 0 (read complete / pad serviced)
}

void pad_overrides_init(void) {
  pad_init();
  rec_set_override(0x80003A4Cu, ov_pad_read);   // per-VBlank pad read (no-op until 0x80003A4C is recompilable)
}

// Per-frame native pad service. The real console fills the slot pad buffers from the per-VBlank
// SIO read (FUN_80003A4C, hooked into the VBlank IRQ by StartPAD). Our no-IRQ runtime never fires
// that, so FUN_80003A4C is dead and the buffers would stay at their init value (0xFF/no-pad),
// making FUN_800524b4 read "no controller". The native frame loop (native_boot.c) calls this once
// per frame, BEFORE the game reads input (FUN_800788ac -> FUN_800524b4), to do exactly what the
// VBlank read would: poll host input, then write the standard digital packet into every registered
// slot buffer. Slot buffer pointers live in the SIO driver's table at 0x0000AEC8 (+slot*4), set by
// FUN_800040c4 (via FUN_80088b00, called from the pad init FUN_800520e0 in the boot prefix).
//
// NOTE on the buffer address: FUN_800040c4 is the SIO driver's InitPAD, which lives in low text
// (0x80004xxx) BELOW MAIN.EXE and is never present/run in this port, so the runtime pointer table
// at 0x0000AEC8 stays NULL (verified: aec8==0 at boot). But the game reads its slot-0 packet from a
// FIXED, known address: FUN_800520e0 registers &DAT_800BF4F8 (slot0) / &DAT_800BF51A (slot1) via
// FUN_80088b00, and FUN_800524b4 reads DAT_800BF4F8 directly. So we write the packet to those fixed
// buffers (and, if the pointer table ever does get populated, to whatever it points at too).
#define PAD_SLOT0_BUF 0x800BF4F8u
#define PAD_SLOT1_BUF 0x800BF51Au

// REPL pad control (native-port -repl): a held active-low mask + a tap countdown. The REPL sets
// these; pad_service_frame applies them so the interactive driver can press/hold/tap buttons.
static uint16_t s_repl_hold = PAD_NONE;   // bits cleared = held down
static uint16_t s_repl_tap  = PAD_NONE;   // active-low mask pressed for s_repl_tap_n frames
static int      s_repl_tap_n = 0;
static int      s_repl_on = 0;
void pad_repl_hold(uint16_t active_low_mask)  { s_repl_on = 1; s_repl_hold = active_low_mask; }
void pad_repl_tap(uint16_t active_low_mask, int n) { s_repl_on = 1; s_repl_tap = active_low_mask; s_repl_tap_n = n; }
// Fully relinquish REPL pad control (s_repl_on=0) so neither a held mask nor a stale PAD_NONE keeps
// overriding host/FORCE input. Used by the state-gated auto-navigator to go hands-off in gameplay.
void pad_repl_release(void) { s_repl_on = 0; s_repl_hold = PAD_NONE; s_repl_tap = PAD_NONE; s_repl_tap_n = 0; }

void pad_service_frame(void) {
  static int s_have_window = -1;
  static int s_force_init = 0;
  static int s_force_on = 0;
  static uint16_t s_force_mask = PAD_NONE;
  static uint32_t s_fc = 0;       // internal frame counter for the pulse (== native frame index)
  static uint16_t s_hold_mask = PAD_NONE;  // headless test hook: a HELD (not pulsed) mask...
  static uint32_t s_hold_at = 0;           // ...applied from this native frame onward
  if (s_have_window < 0) s_have_window = getenv("PSXPORT_GPU_WINDOW") ? 1 : 0;
#ifdef PSXPORT_SDL
  if (s_have_window) pad_poll_sdl();             // host keyboard/gamepad -> s_buttons
#endif
  if (!s_force_init) {                           // headless test hook: pulse an active-low mask
    const char* force = getenv("PSXPORT_FORCE_BUTTONS");
    if (force) { s_force_on = 1; s_force_mask = (uint16_t)strtoul(force, 0, 16); }
    // Second phase: HOLD a mask continuously from PSXPORT_FORCE_HOLD_AT onward (overrides the
    // pulse). Lets a headless run reach a state via pulsed Start, then hold a direction in-level
    // (a held direction is what the game reads for movement) — for interactivity testing.
    const char* hold = getenv("PSXPORT_FORCE_HOLD");
    if (hold) { s_force_on = 1; s_hold_mask = (uint16_t)strtoul(hold, 0, 16);
                const char* at = getenv("PSXPORT_FORCE_HOLD_AT"); s_hold_at = at ? strtoul(at, 0, 0) : 0; }
    s_force_init = 1;
  }
  // Pulse the forced buttons (pressed 8 frames, released 24) so each press is a fresh EDGE the
  // game's current&~prev input logic (FUN_800788ac) actually sees — a continuous hold would edge
  // only once. Lets a headless run drive menus deterministically without a host controller. Once
  // past FORCE_HOLD_AT, hold FORCE_HOLD continuously instead (movement input).
  // PSXPORT_FORCE_STOP_AT=N: cease ALL forced input at frame N (release everything). Lets a run
  // pulse Start to drive through attract/menu/intro to a target scene, then go fully hands-off so
  // the scene's own BGM/state isn't disturbed by phantom presses (Start in gameplay = pause menu,
  // which stops BGM — that artifact poisoned earlier BGM captures).
  static long s_stop_at = -2;
  if (s_stop_at == -2) { const char* e = getenv("PSXPORT_FORCE_STOP_AT"); s_stop_at = e ? atol(e) : -1; }
  if (s_force_on && !(s_stop_at >= 0 && (long)s_fc >= s_stop_at)) {
    if (s_hold_mask != PAD_NONE && s_fc >= s_hold_at) pad_set_buttons(s_hold_mask);
    else pad_set_buttons((s_fc % 32u) < 8u ? s_force_mask : PAD_NONE);
  }
  // REPL pad control: a tap (countdown) overrides the held mask while active.
  if (s_repl_on) {
    if (s_repl_tap_n > 0) { pad_set_buttons(s_repl_tap); s_repl_tap_n--; }
    else pad_set_buttons(s_repl_hold);
  }
  s_fc++;

  uint8_t pk[4];
  pad_fill_buffer(pk);
  uint32_t bufs[2] = { PAD_SLOT0_BUF, PAD_SLOT1_BUF };       // fixed game pad buffers
  for (int slot = 0; slot < 2; slot++) {
    uint32_t b = mem_r32(0x0000AEC8u + (uint32_t)slot * 4u); // registered ptr (NULL in this port)
    if (!b) b = bufs[slot];                                  // fall back to the fixed buffer
    for (int i = 0; i < 4; i++) mem_w8(b + i, pk[i]);
  }
  // Slot 1: report "no controller" so single-pad logic ignores it (status 0xFF).
  mem_w8(PAD_SLOT1_BUF, 0xFF);
}

#endif // PSXPORT_PAD_NO_OVERRIDES

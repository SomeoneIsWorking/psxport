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
#include "cfg.h"
#include "core.h"
#include "game.h"   // PadState lives on Game; reached via c->game->pad (de-globalization, 2026-06-19)
#include "c_subsys.h" // gpu_windowed()

// PSX digital button bits, active-low (0 = pressed). Default = nothing pressed.
#define PAD_NONE 0xFFFFu

// Host button state now lives on the instance: c->game->pad.buttons (was the file-scope s_buttons).

void pad_init(Core* c) {
  c->game->pad.buttons = PAD_NONE;
}

// Host/PM feeds button state in the PSX active-low layout (0 bit = pressed).
void pad_set_buttons(Core* c, uint16_t mask) {
  c->game->pad.buttons = mask;
}

uint16_t pad_buttons(Core* c) {
  return c->game->pad.buttons;
}

// Write the standard digital auto-pad packet into a buffer the game polls.
// `buf` must have room for at least 4 bytes. Layout matches FUN_80003a4c's output
// for a connected digital pad (id 0x41).
void pad_fill_buffer(Core* c, uint8_t* buf) {
  if (!buf) return;
  uint16_t b = c->game->pad.buttons;
  buf[0] = 0x00;                       // status: pad present / read ok
  buf[1] = 0x41;                       // id: digital controller (1 halfword of data)
  buf[2] = (uint8_t)(b & 0xFF);        // button mask low  (active-low)
  buf[3] = (uint8_t)((b >> 8) & 0xFF); // button mask high (active-low)
}

// --- Optional SDL host input ------------------------------------------------
// DEFAULT (no host input) works headlessly: s_buttons stays 0xFFFF (no presses)
// unless pad_set_buttons() / pad_poll_sdl() updates it. SDL is compiled in only
// under PSXPORT_SDL, matching gpu_native.c's optional-SDL style.
#ifdef PSXPORT_SDL
#include <SDL.h>
#include <stdio.h>   // diagnostic fprintf in pad_poll_sdl (controller-driving-directions notice)
#include <stdlib.h>  // atoi (PSXPORT_PAD_NOPAD parse)

// Provided by rmlui_overlay.cpp. Returns 1 ONLY while the user is actively typing into an RmlUi text
// widget (io.WantTextInput) — NOT merely because the overlay is open/focused. We use it to suppress the
// game's keyboard read in that narrow case so typed characters don't leak into gameplay (and WASD doesn't
// leak into the text box). When the overlay is simply visible, this is 0 and WASD drives the game normally.
// Declared here (extern "C") instead of including rmlui_overlay.h so this TU stays header-light/testable.
extern "C" int rmlui_overlay_wants_keyboard(void);

// --- Game controller (gamepad) state ----------------------------------------
// Up to this many simultaneously-open controllers; hotswap-aware (DEVICEADDED/REMOVED handled by a
// per-frame rescan in pad_rescan_controllers() so NO other file needs editing — see pad_poll_sdl).
#define PAD_MAX_GC 4
static SDL_GameController* s_gc[PAD_MAX_GC] = { nullptr, nullptr, nullptr, nullptr };
static int                s_gc_inst[PAD_MAX_GC] = { -1, -1, -1, -1 };  // SDL_JoystickID per slot (-1 = empty)
static int                s_gc_sub_init = 0;                            // lazily added the gamecontroller subsystem?

// Lazily ensure SDL's gamecontroller subsystem is up. SDL_Init(SDL_INIT_VIDEO) happens in gpu_vk.cpp
// (not owned here); the controller subsystem is independent, so we add it on first use. Idempotent.
static void pad_ensure_gc_subsystem(void) {
  if (s_gc_sub_init) return;
  if (SDL_WasInit(SDL_INIT_GAMECONTROLLER) == 0)
    SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER);
  s_gc_sub_init = 1;
}

// HOTSWAP: open any newly-connected controllers and drop any that vanished. Self-contained per-frame
// rescan so we don't depend on SDL_CONTROLLERDEVICEADDED/REMOVED events reaching us through an event
// pump we don't own (the pump lives in gpu_vk.cpp). Cheap: SDL_NumJoysticks is a count, and we only
// call SDL_GameControllerOpen for indices we haven't already opened.
static void pad_rescan_controllers(void) {
  // ESCAPE HATCH (Linux WASD-dead): PSXPORT_PAD_NOPAD=1 ignores ALL game controllers and uses the
  // keyboard only. Use this if a connected/phantom pad with a drifting analog stick ("analog mode")
  // is injecting a phantom direction and you can't unplug it. Close anything already open, then bail.
  static int nopad = -1;
  if (nopad < 0) { const char* v = cfg_str("PSXPORT_PAD_NOPAD"); nopad = (v && atoi(v) != 0) ? 1 : 0; }
  if (nopad) {
    for (int s = 0; s < PAD_MAX_GC; s++)
      if (s_gc[s]) { SDL_GameControllerClose(s_gc[s]); s_gc[s] = nullptr; s_gc_inst[s] = -1; }
    return;
  }
  pad_ensure_gc_subsystem();
  // 1) Drop slots whose controller was unplugged.
  for (int s = 0; s < PAD_MAX_GC; s++) {
    if (s_gc[s] && !SDL_GameControllerGetAttached(s_gc[s])) {
      SDL_GameControllerClose(s_gc[s]);
      s_gc[s] = nullptr; s_gc_inst[s] = -1;
    }
  }
  // 2) Open any attached controller we don't already hold (dedup by instance id).
  //    Linux note: SDL enumerates a lot of /dev/input nodes; SDL_IsGameController already filters to
  //    devices with a real gamecontroller mapping, so we don't open accelerometers/phantom joysticks
  //    here and then read garbage axes from them (which used to inject a permanent phantom direction).
  int n = SDL_NumJoysticks();
  for (int i = 0; i < n; i++) {
    if (!SDL_IsGameController(i)) continue;
    SDL_JoystickID inst = SDL_JoystickGetDeviceInstanceID(i);
    int already = 0;
    for (int s = 0; s < PAD_MAX_GC; s++) if (s_gc_inst[s] == inst) { already = 1; break; }
    if (already) continue;
    for (int s = 0; s < PAD_MAX_GC; s++) {
      if (!s_gc[s]) {
        SDL_GameController* gc = SDL_GameControllerOpen(i);
        if (gc) { s_gc[s] = gc; s_gc_inst[s] = SDL_JoystickInstanceID(SDL_GameControllerGetJoystick(gc)); }
        break;
      }
    }
  }
}

// OR one controller's buttons + analog sticks into the active-low PSX mask.
// Button mapping (Sony layout — natural for a PSX title):
//   A -> Cross, B -> Circle, X -> Square, Y -> Triangle (SDL A/B/X/Y are positional SNES-style;
//   this gives the conventional Sony bottom=Cross, right=Circle, left=Square, top=Triangle).
//   LeftShoulder/RightShoulder -> L1/R1; LeftTrigger/RightTrigger (analog, thresholded) -> L2/R2.
//   Start -> Start, Back -> Select, LeftStick/RightStick click -> L3/R3.
//   D-pad AND left analog stick -> directions (stick past ~50% deflection counts as a press).
//
// DEADZONE / DRIFT ROBUSTNESS (Linux fix, 2026-06-21): the directional STICK threshold must be a LARGE
// fraction of full deflection, never a small "off-center" value. The previous code OR'd directions in
// additively across EVERY connected controller every frame with a 50%-ish threshold and NO lower guard,
// so on Linux a controller (or a phantom/virtual joystick SDL enumerates and the hotswap then opens)
// whose left stick rests slightly off-center — or whose axes read stale/extreme before the first
// joystick event is pumped — would HOLD a phantom direction continuously. A constantly-held analog
// direction makes the game look "stuck"/unresponsive to WASD (it fights or saturates movement), which
// is exactly the "WASD doesn't move the player / maybe analog mode" symptom on the Linux machine. Keep
// the threshold high (~70%) so only a deliberate stick push registers and resting drift never does.
static void pad_apply_controller(SDL_GameController* gc, uint16_t* mask) {
  #define BTN(b) SDL_GameControllerGetButton(gc, (b))
  const int STICK = 22000;   // ~67% of 32767 deflection -> treat as a directional press (drift-proof)
  Sint16 lx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
  Sint16 ly = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);
  if (BTN(SDL_CONTROLLER_BUTTON_DPAD_UP)    || ly < -STICK) *mask &= ~0x0010u; // Up
  if (BTN(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) || lx >  STICK) *mask &= ~0x0020u; // Right
  if (BTN(SDL_CONTROLLER_BUTTON_DPAD_DOWN)  || ly >  STICK) *mask &= ~0x0040u; // Down
  if (BTN(SDL_CONTROLLER_BUTTON_DPAD_LEFT)  || lx < -STICK) *mask &= ~0x0080u; // Left
  if (BTN(SDL_CONTROLLER_BUTTON_START))        *mask &= ~0x0008u; // Start
  if (BTN(SDL_CONTROLLER_BUTTON_BACK))         *mask &= ~0x0001u; // Select
  if (BTN(SDL_CONTROLLER_BUTTON_A))            *mask &= ~0x4000u; // Cross
  if (BTN(SDL_CONTROLLER_BUTTON_B))            *mask &= ~0x2000u; // Circle
  if (BTN(SDL_CONTROLLER_BUTTON_X))            *mask &= ~0x8000u; // Square
  if (BTN(SDL_CONTROLLER_BUTTON_Y))            *mask &= ~0x1000u; // Triangle
  if (BTN(SDL_CONTROLLER_BUTTON_LEFTSHOULDER)) *mask &= ~0x0400u; // L1
  if (BTN(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER))*mask &= ~0x0800u; // R1
  if (BTN(SDL_CONTROLLER_BUTTON_LEFTSTICK))    *mask &= ~0x0002u; // L3
  if (BTN(SDL_CONTROLLER_BUTTON_RIGHTSTICK))   *mask &= ~0x0004u; // R3
  if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT)  > STICK) *mask &= ~0x0100u; // L2
  if (SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > STICK) *mask &= ~0x0200u; // R2
  #undef BTN
}

// Map keyboard + first connected gamepad to the PSX button mask. Call once per
// frame (after SDL_PumpEvents elsewhere, or it pumps here). Builds an ACTIVE-LOW
// mask: a bit is cleared when its control is pressed.
void pad_poll_sdl(Core* c) {
  SDL_PumpEvents();
  uint16_t mask = PAD_NONE;

  // GitHub #18: keep SDL text input (IME) OFF during gameplay. SDL leaves text input ON by default and
  // RmlUi's SDL backend re-enables it via its IME handler whenever the overlay window is focused, so this
  // MUST run every frame (one-shot disabling is not enough). With IME on, KDE/Wayland compositors pop an
  // "alternative character"/compose widget that swallows WASD before SDL_GetKeyboardState() sees it ->
  // the player won't move. Only stop it when no UI text field actually wants the keyboard, so focused
  // RmlUi fields can still type.
  if (!rmlui_overlay_wants_keyboard() && SDL_IsTextInputActive()) {
    SDL_StopTextInput();
    static int s_ime_noted = 0;
    if (!s_ime_noted) { s_ime_noted = 1; fprintf(stderr, "[pad] IME/text-input disabled during gameplay (GH#18)\n"); }
  }

  // Suppress the keyboard ONLY while the user is typing into an RmlUi text field (see the extern decl).
  // A merely-visible overlay does NOT block WASD — that was the bug. The debug pause/step keys below stay
  // outside this guard intentionally so P/'.' still work even with a field focused.
  const Uint8* ks = (rmlui_overlay_wants_keyboard() ? nullptr : SDL_GetKeyboardState(NULL));
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
    #undef KEYDOWN
  }

  // Debug pause / frame-step keys (edge-detected so one keypress = one action). P toggles pause/play;
  // '.' (period) freezes and advances exactly one frame. Read SDL's keyboard snapshot directly (not the
  // `ks` above) so these still work even when RmlUi suppressed gameplay keys. Handled here because
  // pad_poll_sdl runs every frame in BOTH the running loop and the paused wait. (dbg_server.c)
  { void dbg_toggle_pause(void); void dbg_add_step(int);
    const Uint8* dks = SDL_GetKeyboardState(NULL);
    static int prev_p = 0, prev_step = 0;
    int p  = dks && dks[SDL_SCANCODE_P]      != 0;
    int st = dks && dks[SDL_SCANCODE_PERIOD] != 0;
    if (p && !prev_p)   dbg_toggle_pause();
    if (st && !prev_step) dbg_add_step(1);
    prev_p = p; prev_step = st; }

  // HOTSWAP-aware controllers: open/close as devices come and go, then OR every connected pad into the
  // mask (additive with the keyboard, so both work simultaneously). Self-contained — no dependency on
  // SDL_CONTROLLERDEVICE* events from the (unowned) event pump.
  //
  // SDL_GameControllerUpdate() FIRST: the joystick/controller event pump lives in another TU (gpu_vk's
  // poll_quit) and our SDL_PumpEvents() above only refreshes controller state if the GC subsystem was
  // already up at pump time. The subsystem is lazily initialized inside pad_rescan_controllers(), so on
  // the frames right after a controller is opened the button/axis reads could be stale (Linux: stale
  // axes read as a held direction). An explicit update guarantees fresh state before we read it.
  pad_rescan_controllers();
  SDL_GameControllerUpdate();
  uint16_t before_pads = mask;
  for (int s = 0; s < PAD_MAX_GC; s++)
    if (s_gc[s]) pad_apply_controller(s_gc[s], &mask);

  // DIAGNOSTIC (Linux WASD-dead hunt): if a controller is injecting directions while the keyboard
  // pressed nothing, say so once — this tells the user a connected/phantom pad (e.g. a drifting analog
  // stick "analog mode") is the input source, not their keyboard. Active-low: bits CLEARED == pressed.
  // Only fires on a transition so it doesn't spam. Set PSXPORT_PAD_NOPAD=1 to ignore controllers
  // entirely (keyboard only) if a phantom device is the culprit and you can't unplug it.
  {
    const uint16_t DIRS = 0x00F0u;  // Up/Right/Down/Left
    static int warned = 0;
    int pad_dirs   = (before_pads & DIRS) != (mask & DIRS);          // a pad changed a direction bit
    int kbd_no_dir = (before_pads & DIRS) == DIRS;                   // keyboard pressed no direction
    if (pad_dirs && kbd_no_dir && !warned) {
      fprintf(stderr, "[pad] a game controller is driving DIRECTIONS (host pad mask=0x%04x). If WASD "
                      "seems dead, an analog stick / phantom pad is the input. Set PSXPORT_PAD_NOPAD=1 "
                      "to use the keyboard only.\n", (unsigned)mask);
      warned = 1;
    }
  }

  c->game->pad.buttons = mask;
}

// Close any open controllers (call on shutdown if desired). Safe to call multiple times.
void pad_close_controllers(void) {
  for (int s = 0; s < PAD_MAX_GC; s++)
    if (s_gc[s]) { SDL_GameControllerClose(s_gc[s]); s_gc[s] = nullptr; s_gc_inst[s] = -1; }
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
#include <stdio.h>
#include <stdlib.h>

// Slot-buffer pointer table base in the SIO driver's low-RAM globals: the per-slot
// pad buffer pointer for slot `slot` is at 0x0000AEC8 + slot*4 (FUN_800040c4 stores
// them there; FUN_80003a4c reads them via &DAT_0000aec8 + slot*4).
// FUN_80003a4c(slot): a0 = slot index.
static void ov_pad_read(Core* c) {
  uint32_t b = c->mem_r32(0x0000AEC8u + c->r[4] * 4u);   // registered slot buffer ptr
  if (b) {
    uint8_t pk[4];
    pad_fill_buffer(c, pk);
    for (int i = 0; i < 4; i++) c->mem_w8(b + i, pk[i]);
  }
  c->r[2] = 0;   // v0 = 0 (read complete / pad serviced)
}

void pad_overrides_init(Core* c) {
  pad_init(c);
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

// REPL pad control (native-port -repl): a held active-low mask + a tap countdown, now on the
// instance (c->game->pad.repl_*). The REPL sets these; pad_service_frame applies them so the
// interactive driver can press/hold/tap buttons.
void pad_repl_hold(Core* c, uint16_t active_low_mask)  { c->game->pad.repl_on = 1; c->game->pad.repl_hold = active_low_mask; }
void pad_repl_tap(Core* c, uint16_t active_low_mask, int n) { c->game->pad.repl_on = 1; c->game->pad.repl_tap = active_low_mask; c->game->pad.repl_tap_n = n; }
// Fully relinquish REPL pad control (repl_on=0) so neither a held mask nor a stale PAD_NONE keeps
// overriding host/FORCE input. Used by the state-gated auto-navigator to go hands-off in gameplay.
void pad_repl_release(Core* c) { c->game->pad.repl_on = 0; c->game->pad.repl_hold = PAD_NONE; c->game->pad.repl_tap = PAD_NONE; c->game->pad.repl_tap_n = 0; }

void pad_service_frame(Core* c) {
  static int s_have_window = -1;
  static int s_force_init = 0;
  static int s_force_on = 0;
  static uint16_t s_force_mask = PAD_NONE;
  static uint32_t s_fc = 0;       // internal frame counter for the pulse (== native frame index)
  static uint16_t s_hold_mask = PAD_NONE;  // headless test hook: a HELD (not pulsed) mask...
  static uint32_t s_hold_at = 0;           // ...applied from this native frame onward
  s_have_window = gpu_windowed();                // a live on-screen window is up (gpu_vk.cpp)
#ifdef PSXPORT_SDL
  if (s_have_window) pad_poll_sdl(c);            // host keyboard/gamepad -> c->game->pad.buttons
#endif
  if (!s_force_init) {                           // headless test hook: pulse an active-low mask
    const char* force = cfg_str("PSXPORT_FORCE_BUTTONS");
    if (force) { s_force_on = 1; s_force_mask = (uint16_t)strtoul(force, 0, 16); }
    // Second phase: HOLD a mask continuously from PSXPORT_FORCE_HOLD_AT onward (overrides the
    // pulse). Lets a headless run reach a state via pulsed Start, then hold a direction in-level
    // (a held direction is what the game reads for movement) — for interactivity testing.
    const char* hold = cfg_str("PSXPORT_FORCE_HOLD");
    if (hold) { s_force_on = 1; s_hold_mask = (uint16_t)strtoul(hold, 0, 16);
                const char* at = cfg_str("PSXPORT_FORCE_HOLD_AT"); s_hold_at = at ? strtoul(at, 0, 0) : 0; }
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
  if (s_stop_at == -2) { const char* e = cfg_str("PSXPORT_FORCE_STOP_AT"); s_stop_at = e ? atol(e) : -1; }
  if (s_force_on && !(s_stop_at >= 0 && (long)s_fc >= s_stop_at)) {
    if (s_hold_mask != PAD_NONE && s_fc >= s_hold_at) pad_set_buttons(c, s_hold_mask);
    else pad_set_buttons(c, (s_fc % 32u) < 8u ? s_force_mask : PAD_NONE);
  }
  // REPL pad control: a tap (countdown) overrides the held mask while active.
  PadState& pad = c->game->pad;
  if (pad.repl_on) {
    if (pad.repl_tap_n > 0) { pad_set_buttons(c, pad.repl_tap); pad.repl_tap_n--; }
    else pad_set_buttons(c, pad.repl_hold);
  }
  s_fc++;

  // ---- INPUT RECORD / REPLAY (deterministic pad capture) ---------------------------------------
  // The engine has no wall-clock/RNG (Date/random are banned, see CLAUDE.md), so the per-frame final
  // pad mask fully determines a run. PSXPORT_PAD_RECORD=<path> appends the finalized active-low mask
  // (uint16 LE) every frame from boot; PSXPORT_PAD_REPLAY=<path> forces that exact sequence back,
  // reproducing a hand-played session (e.g. a hut entry) exactly, every time, headless. Recording
  // captures the COMBINED result of auto-skip + host input; replay overrides the mask AFTER all other
  // input sources so it wins. A dedicated frame counter (rec_fc) matches record↔replay (both tick once
  // per pad_service_frame call from frame 0). Past the recording's end, replay stops overriding
  // (hands-off) so the game free-runs.
  {
    static int      rec_init = 0;
    static FILE*     rec_fp = nullptr;     // record sink
    static uint16_t* rep_buf = nullptr;    // replay source (loaded once)
    static size_t    rep_n = 0;
    static uint32_t  rec_fc = 0;           // shared record/replay frame index
    if (!rec_init) {
      rec_init = 1;
      // Recording is ALWAYS ON by default (so a hand-played session — e.g. on the user's macOS build —
      // is captured and can be sent over for deterministic replay/diagnosis). Default sink:
      // scratch/bin/pad_session.pad, overwritten each run. Override the path with PSXPORT_PAD_RECORD=<path>;
      // disable with PSXPORT_PAD_RECORD=0. Skipped while REPLAYING (no point re-capturing the replayed input).
      const char* rpath = cfg_str("PSXPORT_PAD_RECORD");
      if (rpath && !strcmp(rpath, "0")) rpath = nullptr;                 // explicit disable
      else if (!rpath && !cfg_str("PSXPORT_PAD_REPLAY")) rpath = "scratch/bin/pad_session.pad";  // default-on
      if (rpath) { rec_fp = fopen(rpath, "wb");
        fprintf(stderr, "[padrec] recording -> %s\n", rec_fp ? rpath : "(open FAILED)"); }
      const char* ppath = cfg_str("PSXPORT_PAD_REPLAY");
      if (ppath) {
        FILE* f = fopen(ppath, "rb");
        if (f) { fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
          rep_n = (size_t)(sz / 2); rep_buf = (uint16_t*)malloc(rep_n * 2);
          if (rep_buf && fread(rep_buf, 2, rep_n, f) != rep_n) { free(rep_buf); rep_buf = nullptr; rep_n = 0; }
          fclose(f);
          fprintf(stderr, "[padrec] replaying %zu frames <- %s\n", rep_n, ppath);
        } else fprintf(stderr, "[padrec] replay open FAILED: %s\n", ppath);
      }
    }
    if (rep_buf && rec_fc < rep_n) {
      pad.buttons = rep_buf[rec_fc];        // force the recorded mask (overrides host/repl/force)
    }
    if (rec_fp) { uint16_t m = pad.buttons; fwrite(&m, 2, 1, rec_fp); fflush(rec_fp); }
    // PSXPORT_PAD_SHOT_AT=f0,f1,... : during replay, screenshot at these EXACT replay (pad) frame
    // indices to scratch/screenshots/padshot_<frame>.ppm. The pad-frame axis (rec_fc) is the faithful
    // one (gpu_frame_no drifts because boot/FMV presents extra frames), so this captures a deterministic
    // visual timeline of a replayed session. Done here, after the mask is applied for THIS frame.
    static int      shot_init = 0;
    static uint32_t shot_at[64]; static int shot_n = 0;
    if (!shot_init) {
      shot_init = 1;
      const char* s = cfg_str("PSXPORT_PAD_SHOT_AT");
      if (s) { char buf[512]; snprintf(buf, sizeof buf, "%s", s);
        for (char* t = strtok(buf, ","); t && shot_n < 64; t = strtok(nullptr, ","))
          shot_at[shot_n++] = (uint32_t)strtoul(t, 0, 0); }
    }
    for (int i = 0; i < shot_n; i++) if (shot_at[i] == rec_fc) {
      void gpu_vk_shot(Core*, const char*); char p[96];
      snprintf(p, sizeof p, "scratch/screenshots/padshot_%u.ppm", rec_fc);
      gpu_vk_shot(c, p);
      fprintf(stderr, "[padrec] shot at replay-frame %u -> %s\n", rec_fc, p);
    }
    // PSXPORT_PAD_DUMP_AT=f0,f1,... : dump 2MB guest RAM (+.spad) at these REPLAY frames to
    // scratch/bin/padram_<f>.bin — for A/B diffing scene state (e.g. village vs hut interior).
    static int dump_init = 0; static uint32_t dump_at[32]; static int dump_n = 0;
    if (!dump_init) { dump_init = 1; const char* s = cfg_str("PSXPORT_PAD_DUMP_AT");
      if (s) { char buf[256]; snprintf(buf, sizeof buf, "%s", s);
        for (char* t = strtok(buf, ","); t && dump_n < 32; t = strtok(nullptr, ","))
          dump_at[dump_n++] = (uint32_t)strtoul(t, 0, 0); } }
    for (int i = 0; i < dump_n; i++) if (dump_at[i] == rec_fc) {
      char p[96]; snprintf(p, sizeof p, "scratch/bin/padram_%u.bin", rec_fc);
      FILE* fp = fopen(p, "wb"); if (fp) { fwrite(c->ram, 1, 0x200000, fp); fclose(fp); }
      char sp[112]; snprintf(sp, sizeof sp, "%s.spad", p);
      FILE* spf = fopen(sp, "wb"); if (spf) { fwrite(c->scratch, 1, sizeof c->scratch, spf); fclose(spf); }
      fprintf(stderr, "[padrec] dumpram at replay-frame %u -> %s\n", rec_fc, p);
    }
    // PSXPORT_PAD_TRACE=lo-hi : log the transition/scene markers EVERY replay frame in [lo,hi] (pad-frame
    // indexed — the faithful axis). Finds which field moves when the player walks into the hut (the
    // seamless sub-scene transition). All fixed-address globals; sm = *0x1f800138.
    static int trace_init = 0; static uint32_t trace_lo = 1, trace_hi = 0;
    if (!trace_init) { trace_init = 1; const char* s = cfg_str("PSXPORT_PAD_TRACE");
      if (s) { unsigned a=0,b=0; if (sscanf(s,"%u-%u",&a,&b)==2){trace_lo=a;trace_hi=b;} else if (sscanf(s,"%u",&a)==1){trace_lo=0;trace_hi=a;} } }
    if (rec_fc >= trace_lo && rec_fc <= trace_hi) {
      uint32_t sm = c->mem_r32(0x1f800138u);
      fprintf(stderr, "[padtr] f%-4u bf839=%02X bf80f=%02X i236=%02X sm4a=%u sm4c=%u sm4e=%u scene=%u bf870=%02X bf809=%02X bf89c=%02X e7e68=%08X tX=%d tZ=%d\n",
        rec_fc, c->mem_r8(0x800bf839u), c->mem_r8(0x800bf80fu), c->mem_r8(0x1f800236u),
        c->mem_r16(sm+0x4a), c->mem_r16(sm+0x4c), c->mem_r16(sm+0x4e),
        c->mem_r32(0x800be258u), c->mem_r8(0x800bf870u), c->mem_r8(0x800bf809u), c->mem_r8(0x800bf89cu),
        c->mem_r32(0x800e7e68u), (int16_t)c->mem_r16(0x800fe916u), (int16_t)c->mem_r16(0x800fe91eu));
    }
    rec_fc++;
  }

  uint8_t pk[4];
  pad_fill_buffer(c, pk);
  uint32_t bufs[2] = { PAD_SLOT0_BUF, PAD_SLOT1_BUF };       // fixed game pad buffers
  for (int slot = 0; slot < 2; slot++) {
    uint32_t b = c->mem_r32(0x0000AEC8u + (uint32_t)slot * 4u); // registered ptr (NULL in this port)
    if (!b) b = bufs[slot];                                  // fall back to the fixed buffer
    for (int i = 0; i < 4; i++) c->mem_w8(b + i, pk[i]);
  }
  // Slot 1: report "no controller" so single-pad logic ignores it (status 0xFF).
  c->mem_w8(PAD_SLOT1_BUF, 0xFF);
}

#endif // PSXPORT_PAD_NO_OVERRIDES

// game_config.cpp — the Tomba!2-specific GameConfig instance (game_iface.h seam).
//
// Every value here is a MAIN.EXE guest-address literal that USED to be baked directly into the
// framework substrate (runtime/recomp/*). The framework now reads `c->cfg->field` in place of the
// literal; this file is where the game supplies them. Pure value-preservation: each number matches
// the framework literal it replaced EXACTLY (a wrong address silently breaks boot / diverges SBS).
//
// Installed once at the very top of main() (boot.cpp) via tomba_install_game_config(), before any
// Game/Core is constructed, so Core's ctor snapshots a non-null c->cfg.
#include "game_iface.h"
#include "game_ctx.h"

static const GameConfig g_tomba_config = {
  // --- crt0 / boot (native_boot.cpp crt0_setup, game_init) ---
  /* bssZeroLo      */ 0x800be0d8u,
  /* bssZeroHi      */ 0x80106228u,
  /* stackTopBase   */ 0x800a3f88u,
  /* stackTopBase2  */ 0x800a3f8cu,
  /* heapBase       */ 0x80106228u,
  /* heapSizePtr    */ 0x800abef8u,
  /* heapBasePtr    */ 0x800abef4u,
  /* gp             */ 0x800be0d4u,
  /* libcInit       */ 0x80089860u,
  /* gameMain       */ 0x80050b08u,   // FUN_80050b08 (native-overridden game-main; comment-only literal)
  /* crt0           */ 0x800896e0u,   // FUN_800896E0 (native crt0; comment-only literal)

  // --- per-frame OT / packet-pool dance (native_boot.cpp native_step_frame) ---
  /* otRegionBase     */ 0x800e80a8u,
  /* otRegionStride   */ 0x00002070u,
  /* packetPoolBase   */ 0x800bfe68u,
  /* packetPoolStride */ 0x00014000u,
  /* otBasePtr        */ 0x800ed8c8u,
  /* dwellCounter     */ 0x800e809cu,
  /* poolPtrCur       */ 0x800bf544u,
  /* poolPtrLast      */ 0x800bf4f4u,
  /* clearOtagR       */ 0x80081458u,
  /* putDrawEnv       */ 0x800815d0u,
  /* drawSync         */ 0x80080f6cu,
  /* irqEventClasses  */ { 0xF2000003u, 0xF0000001u, 0xF0000009u },
  /* dualviewRenderOrch */ 0x8003f9a8u,
  /* dualviewSubmit     */ 0x8010810cu,

  // --- scheduler task layout (scheduler.cpp, native_boot probes) ---
  /* taskTableBase  */ 0x801fe000u,
  /* taskSlotStride */ 0x00000070u,
  /* taskCount      */ 3u,             // up to 3 cooperative tasks (loop lives game-side, no framework literal)
  /* curTaskPtr     */ 0x1f800138u,
  /* stageStart     */ 0x8010649cu,
  /* stageDemo      */ 0x801062e4u,
  /* stageGame      */ 0x8010637cu,

  // --- overlay router slots (overlay_router.cpp slot_index) ---
  /* overlaySlots */ {
    { 0x80106228u, "STAGE" },   // START/DEMO/GAME
    { 0x80108f9cu, "MODE"  },   // SOP / A0* field area code
    { 0x8018a000u, "AREA"  },   // OPN; also raw area DATA
  },

  // --- CD chokepoints (cd_override.cpp) ---
  /* cdInit            */ 0x8008b2d8u,   // CdInit handshake (registered by PlatformHle::initBuiltins, not cd_override)
  /* cdCommand         */ 0x8008ac34u,
  /* cdSync            */ 0x8008a6ecu,
  /* cdReadPrim        */ 0x8008c1ecu,
  /* cdFileLoad        */ 0x8001db8cu,
  /* cdAsyncRead       */ 0x8001d940u,
  /* voicePlay         */ 0x8001d2a8u,
  /* voiceStop         */ 0x8001cf2cu,
  /* lastSectorTracker */ 0x800be0e0u,
  /* cdInlineLoad      */ 0x8001dc40u,
  /* cdCmdStream       */ 0x8001ce90u,
  /* cdCallbackTable   */ { 0x800abfbcu, 0x800abfc0u, 0x800abf24u, 0x800abf28u },
  /* cdCallbackFn      */ { 0x8009996cu, 0x80089994u, 0x800899bcu, 0x00000000u },

  // --- pad driver (pad_input.cpp) ---
  /* padSlot0Buf     */ 0x800bf4f8u,
  /* padSlot1Buf     */ 0x800bf51au,
  /* padDriverFn     */ 0x80003a4cu,   // FUN_80003A4C SIO pad read (inert: driver not in MAIN.EXE; no live register)
  /* padSlotPtrTable */ 0x0000aec8u,
};

// The game's callback vtable — defined in game_hooks.cpp (thin impls reaching eng(c).*).
extern const GameHooks g_tomba_hooks;

void tomba_install_game_config() { psxport_install_game(&g_tomba_config, &g_tomba_hooks); }

// legacy_game_config.h — deprecated guest-address/configuration bag for migrating consumers.
#pragma once
#include <stdint.h>

#ifdef __cplusplus

#include "platform_hle.h"

// SchedBody — which game stage body the framework scheduler (PcScheduler, now framework) is asking the
// game to run. PcScheduler owns the framework-side task/coro/yield machinery; the actual stage bodies are
// game code (Engine::*), reached through the single schedStageBody hook so the framework names no Engine
// method. Values are the game's dispatch cases (see game/core/game_hooks.cpp tomba_schedStageBody).
enum SchedBody {
  SCHED_DEMO_STAGEMAIN = 0,     // eng(c).demo.stageMain()          (fresh DEMO prologue)
  SCHED_DEMO_FRAME,             // eng(c).demo.frame()
  SCHED_GAME_PROLOGUE,          // eng(c).stagePrologue()           (fresh GAME prologue)
  SCHED_GAME_FRAME,             // eng(c).frame()                   → returns handled (0/1)
  SCHED_SOP_AREALOAD,           // eng(c).sop.areaLoad()
  SCHED_CORO_TEXGROUP,          // eng(c).asset.loadTexgroup()
  SCHED_CORO_PRELOAD1,          // eng(c).asset.preloadStage1AsTask()
  SCHED_CORO_AREADATA,          // eng(c).asset.areaDataLoadAsTask()
  SCHED_CORO_AREALOAD_FAITHFUL, // eng(c).sop.areaLoadFaithful()
  SCHED_FIBER_STARTBIN,         // eng(c).startBinStageFaithful()
  SCHED_FIBER_DEMO_BODY,        // eng(c).demo.stageBodyFaithful()
  SCHED_FIBER_STAGE_BODY,       // eng(c).stageBodyFaithful()
};

// GameConfig — deprecated compatibility storage for game-specific guest facts not yet migrated to
// typed runtime owners. A game fills one static instance; living legacy consumers read `c->cfg`.
// The first group below is adapter input only and must disappear after every consumer supplies its
// own GuestProgramImage. Do not add fields here.
struct GameConfig {
  // --- adapter-only GuestProgramImage projection; generic crt0/routing code no longer reads these ---
  uint32_t bssZeroLo, bssZeroHi;        // .bss clear range
  uint32_t stackTopBase, stackTopBase2; // guest stack top globals
  uint32_t heapBase;                    // heap start
  uint32_t heapSizePtr, heapBasePtr;    // heap globals written by crt0
  uint32_t gp;                          // global pointer
  uint32_t libcInit;                    // libc init entry
  uint32_t gameMain, crt0;              // game-main / crt0 entries

  // Recompiled MAIN .text range, masked to physical addresses. Adapter input only: the live overlay
  // router consumes GuestProgramImage::residentText.
  uint32_t recMainLo, recMainHi;

  // Per-game name of the environment variable / .env key that points at this game's disc image
  // (e.g. "PSXPORT_TOMBA2_DISC", "PSXPORT_SPIDERMAN_DISC"). The resolver in disc.c used to hardcode
  // the FIRST consumer's key, so a second port set its own variable, the framework never read it,
  // and the CD model ran with NO DISC MOUNTED while every log line still looked ordinary. Leave it
  // null to rely on the generic PSXPORT_DISC / drop-in *.chd paths, which are always tried too.
  const char *discEnvVar;

  // Boot intro movies, in play order, NULL-terminated. The framework's native .STR player
  // (native_fmv.cpp) resolves each path on the disc itself and needs nothing game-specific to
  // decode one — but native_boot_run used to play a HARDCODED path, the first consumer's file. A
  // second port then failed to open it on every boot, and the only way to silence that was an
  // env-var workaround that disabled FMV wholesale.
  //
  // Leave every slot null when a game's boot plays no movie natively. That is a real state, not a
  // gap: a port whose boot runs on the recompiled substrate has its movies played by the GUEST, and
  // the framework must not invent an intro it was never asked for.
  const char *bootFmv[4];

  // --- measured per-frame OT / packet-pool facts consumed by an adapter title's FrameDriver ---
  uint32_t otRegionBase, otRegionStride;     // per-parity OT region
  uint32_t packetPoolBase, packetPoolStride; // per-parity packet pool
  uint32_t otBasePtr;                        // OT-base pointer global
  uint32_t dwellCounter;
  uint32_t poolPtrCur, poolPtrLast;
  uint32_t clearOtagR, putDrawEnv, drawSync;
  uint32_t irqEventClasses[3];
  uint32_t dualviewRenderOrch, dualviewSubmit;

  // --- scheduler task layout (scheduler.cpp, native_boot probes) ---
  uint32_t taskTableBase, taskSlotStride, taskCount;
  uint32_t curTaskPtr;
  uint32_t stageStart, stageDemo, stageGame; // fresh-entry stage PCs

  // --- overlay router slots (overlay_router.cpp slot_index) ---
  struct OverlaySlot {
    uint32_t base;
    const char *name;
  };
  OverlaySlot overlaySlots[3];

  // --- CD chokepoints (cd_override.cpp) ---
  uint32_t cdInit, cdCommand, cdSync, cdReadPrim, cdFileLoad, cdAsyncRead, voicePlay, voiceStop, lastSectorTracker;
  uint32_t cdInlineLoad;       // (added P1.x) FUN_8001DC40 inline (non-spawning) sync loader
  uint32_t cdCmdStream;        // (added P1.x) FUN_8001CE90 streaming CD-cmd wrapper (GetlocL)
  uint32_t cdCallbackTable[4]; // the 4 guest-RAM slots hleInit writes the CD-event callbacks into
  uint32_t cdCallbackFn[4];    // (added P1.x) the 4 callback fn-ptr VALUES written into those slots

  // STOCK Sony libcd CdGetSector(dest, words) — the sector-transfer routine. Appended at the END of
  // this group on purpose: GameConfig is initialised POSITIONALLY by every consumer, so inserting a
  // field mid-struct silently shifts every value after it.
  //
  // Distinct from cdReadPrim because the CONTRACT differs: this one carries no LBA. Stock libcd
  // positions the head with CdlSetloc and reads from wherever it was left, so the handler consumes
  // Cd::setloc_lba rather than an argument. Zero for a game that uses an engine loader instead.
  uint32_t cdGetSector;

  // Guest global holding stock libcd's READY callback pointer (what CdReadyCallback() writes). The
  // PC drives the read through it: a stock-libcd read is a per-sector callback loop, so invoking
  // this once per sector completes the transfer with no interrupt in the picture at all. Zero for a
  // game that does not use stock libcd. See cd_override.cpp cd_command's ReadN case.
  uint32_t cdReadyCbPtr;

  // Guest buffer holding stock libcd's LAST REQUESTED POSITION (what CdLastPos() returns), and the
  // last Setmode byte immediately after it: [+0..3] = the Setloc parameter, [+4] = mode.
  //
  // This is bookkeeping the REPLACED routine used to do. Overriding a library entry point means
  // inheriting the state it maintained: stock libcd records the Setloc parameter inside its
  // command-send routine, and its read path later seeds the expected-sector counter from it
  // (CdPosToInt(CdLastPos())). Skip the record and the counter is seeded from stale bytes, the
  // drive-position check fails, and every read is rejected — with nothing pointing at the override
  // as the cause. Zero for a game that does not use stock libcd.
  uint32_t cdLastPosBuf;

  // STOCK Sony libcd CdRead(sectors, buf, mode) and CdReadSync(mode, result). Overriding at THIS
  // level makes the PC perform the whole read: the guest's per-sector callback loop, drive-position
  // check, vblank timeout and retry path never run. Zero for a game that does not use stock libcd.
  uint32_t cdReadStock;
  uint32_t cdReadSync;

  // STOCK Sony libcd CdSearchFile(CdlFILE*, name) — resolved natively from the disc image's ISO9660
  // tree, so the guest's filesystem walk issues no drive activity at all.
  uint32_t cdSearchFile;

  // BASE of the guest's per-channel DMA-COMPLETION callback table — the table `DMACallback(ch, fn)`
  // writes, indexed by DMA channel (channel ch's entry is base + 4*ch). The port dispatches these
  // itself, standing in for the BIOS DMA interrupt handler, gated on the channel's DICR enable.
  //
  // EVERY channel, not just the CD's: a streaming reader relies on the channel-3 callback to promote
  // a ring slot from "DMA in flight" to "ready", and an FMV player relies on the channel-1 (MDEC-out)
  // callback to upload the decoded strip to VRAM. Announcing only one of them looks like a working
  // port right up until the other subsystem is used.
  //
  // Zero for a game whose table has not been reverse-engineered — then NO callback is dispatched,
  // which is the same behaviour as a guest that registered none, never a wrong one.
  uint32_t dmaCallbackTable;

  // --- pad driver (pad_input.cpp) ---
  uint32_t padSlot0Buf, padSlot1Buf, padDriverFn;
  uint32_t padSlotPtrTable; // (added P1.x) SIO driver per-slot buf-ptr table base (+slot*padSlotPtrStride)
  // Byte distance between consecutive slots' buffer pointers. A driver that keeps a flat pointer
  // array uses 4 (Tomba!2); one that stores the pointer INSIDE a per-port context record uses that
  // record's size (Spyro: libpad's 240-byte per-port context). 0 is read as 4 so a config that
  // predates this field keeps its old meaning rather than silently reading slot 1 from slot 0.
  uint32_t padSlotPtrStride;

  // --- platform HLE: the PSX hardware-sync primitives (sync_overrides.cpp) ---
  // These are the SCEI library entry points whose real bodies busy-spin on a hardware IRQ our no-IRQ
  // runtime never raises (libmdec/libcd/libgpu/libetc sync + the kernel task-switch funnel).
  // PlatformHle::initBuiltins() installs a native handler at each.
  //
  // They were hardcoded in sync_overrides.cpp until 2026-07-28 — the SAME defect the seed set had,
  // and wrong in the same way: the addresses are facts about ONE executable. For a different game
  // they miss every primitive it actually uses AND install handlers over unrelated functions that
  // happen to sit at those addresses, which is a wrong abort waiting to fire rather than a silent
  // no-op. (Found standing up a second consumer; Spider-Man has real code at Tomba!2's VSync
  // address.) Same remedy as recMainLo/recMainHi: the value travels with the game.
  //
  // ZERO MEANS "this game has no such primitive, or it has not been RE'd yet" — initBuiltins skips
  // it. A game that leaves one zero and needs it will hang in the real spin loop, which is the
  // honest signal that its RE is outstanding.
  struct PlatformHleCfg {
    // The address windows register_() will accept. Everything outside them is refused, which is what
    // keeps GAME/engine logic out of this table (game logic is owned top-down through the override
    // registry instead). Each slot is one exact half-open range; an unused one is left {0,0}. If NO
    // window is configured, register_() refuses everything and says so — a game must state its own
    // memory map. Direct and adapter storage share the one framework capacity constant.
    uint32_t windowLo[kPlatformHleWindowCapacity], windowHi[kPlatformHleWindowCapacity];

    // Adapter input only for GuestProgramImage::backtraceText. Physical range; zero means the typed
    // image falls back to residentText.
    uint32_t codeScanLo, codeScanHi;

    uint32_t decDctInSync, decDctOutSync;    // libmdec DecDCTinSync / DecDCToutSync
    uint32_t cdReadSync, cdDataSync;         // libcd CdReadSync / CdDataSync
    uint32_t cdInitHandshake;                // libcd low-level CdInit controller-ready handshake
    uint32_t gpuTimeoutArm, gpuTimeoutCheck; // libgpu GPU-DMA-completion timeout (arm / check)
    uint32_t gpuTimeoutDeadlineVar;          // guest global the arm writes its far-future deadline to
    uint32_t gpuTimeoutFlagVar;              // guest global the arm clears
    uint32_t changeThread;                   // kernel cooperative task-switch / yield funnel

    // libgte SetGeomOffset / SetGeomScreen — the two leaves through which the game STATES its camera
    // projection (screen centre OFX/OFY, projection-plane distance H). Owned natively so the port
    // RECORDS the projection where the game sets it instead of reading CR24/25/26 back out of the GTE
    // at draw time. See proj_params.h for the implementation and why it is oracle-safe.
    //
    // Zero for a game whose libgte entry points have not been located — then nothing is registered and
    // the recompiled bodies run, which leaves ProjParams unset and makes the native camera's
    // requireGeom abort rather than draw with an invented projection. That is the intended behaviour
    // for an un-RE'd port, not a gap to paper over.
    uint32_t setGeomOffset, setGeomScreen;

    // Measured libetc VSync entry. Product boot requires this address and binds it to the framework's
    // all-mode fatal trap: every port's frame loop is native-owned, so neither guest waits nor guest
    // queries are valid shipping paths. The legacy field name is retained only to preserve aggregate
    // layout while adapter consumers migrate to PlatformHlePlan::vsyncAddress.
    uint32_t vsyncTrap;
  } hle;

  // --- adapter-only rendering policy --------------------------------------------------------------
  // Does the guest's UPLOADED VRAM stay visible under the submitted primitives?
  //
  // The renderer clears the colour target to black before drawing, on the principle that "the PC
  // renderer shows ONLY what a native producer submitted". That is right for a port whose native
  // renderer owns the frame — anything left over from the guest's own drawing would be stale.
  //
  // It is WRONG for a port still running the guest's drawing code, because on real hardware an upload
  // into the display area IS visible. A game whose logo screens, FMV stills or menus are uploads with
  // no primitives renders them BLACK: the upload lands in VRAM and the clear discards it before
  // anything is drawn on top. That is exactly what Spyro's SCE/Universal screens do.
  //
  // ZERO KEEPS THE EXISTING BEHAVIOUR (clear to black), so a consumer that does not set it is
  // unaffected — and this field is APPENDED at the end of the struct because GameConfig is
  // initialised positionally by some consumers. Set to 1 while the guest still owns drawing.
  // LegacyGameRuntimeAdapter projects this into GameRuntime::guestVramIsPicture(). The renderer no
  // longer reads the bag. Migrated runtimes override that policy directly and may vary it per frame.
  uint32_t preserveVramBackdrop;

  // --- memory card (memcard.cpp) ----------------------------------------------------------------
  // The consuming game's memory-card env key and default backing-file path. Same lesson as
  // discEnvVar: the resolver used to hardcode the FIRST consumer's key (PSXPORT_TOMBA2_CARD) and
  // filename (scratch/saves/tomba2.mcr), so a second consumer's card silently landed in the
  // reference game's file, or nowhere. NULL keeps the old behaviour for a consumer that has not set
  // them. Appended at the end because GameConfig is initialised positionally.
  const char *cardEnvVar;
  const char *cardDefaultPath;

  // --- frame pacing (frame_pacer.cpp) -----------------------------------------------------------
  // The pacer sleeps a whole-frame interval per call so a live run plays at the game's intended
  // speed instead of spinning. `quota` is the number of DISPLAY FIELDS one pacing call represents,
  // and the interval is that many fields at the game's real field rate (gpu_field_rate_millihz,
  // decoded from GP1(0x08) bit 3) divided by `parts`.
  //
  // IT IS NOT "WINDOWED PACING". It used to be — the pacer early-returned when there was no window,
  // which made every headless timing number a statement about a program the user never runs.
  // Headless and windowed are one program (headless = no window surface, no audio device, nothing
  // else); `PSXPORT_NOPACE` is the ONE switch for "run as fast as the host can".
  //
  // ZERO IS LOUD, ONCE, AND PACES AT ONE FIELD. It used to fall through to reading the scratchpad
  // byte 0x1F800235 — the FIRST consumer's (Tomba!2) engine field, "vblanks per displayed frame,
  // =2 => 30fps" — which is ordinary working memory in any other game (Spyro's geometry renderer
  // writes vertex data over it), so a second consumer slept on garbage and its run crawled. That
  // fallback is DELETED: a port that has not derived its cadence must be visibly unconfigured, not
  // quietly mistimed. A new game MUST set this field.
  //
  // Semantics of the value are by CALLING CADENCE, not the game's display rate: a native-loop port
  // that calls gpu_pace_frame once per logic frame sets the game's fields-per-frame (2 => 30fps on a
  // 60-field display). A port that still runs the guest's own frame loop and paces once per field
  // sets 1. The LENGTH of a field is not this field's business — that comes from the game's video
  // standard (gpu_field_rate_millihz). Appended at the end because GameConfig is initialised
  // positionally.
  uint32_t paceQuota;

  // Window title. The framework is game-agnostic and must not name a game, but gpu_vk.cpp hardcoded
  // "Tomba! 2 (SDL_GPU)" — the FIRST consumer's — so every port added since presented itself as
  // Tomba!2 in the title bar and the taskbar, on all three games at once. Left null, the window is
  // titled "psxport (untitled game)": OBVIOUSLY wrong rather than plausibly wrong, because a title
  // naming some OTHER game reads as correct and therefore never gets reported.
  // Also appended at the end — spider1 initialises GameConfig POSITIONALLY, so inserting a field
  // mid-struct silently shifts every field after it there.
  const char *windowTitle;

  // --- crt0 stack-top bias (crt0_boot.h) --------------------------------------------------------
  // The guest crt0's OWN adjustment of the stack-top word before it becomes sp:
  //
  //     v0 = mem[stackTopBase] + stackBias.value ;  sp = fp = v0 | 0x80000000
  //
  // and, because the biased word also feeds the heap-size subtraction, `heapsz = (v0 - mem[
  // stackTopBase2]) - (heapBase & 0x1FFFFFFF)` moves with it too.
  //
  // WHY THIS IS A DECLARATION AND NOT JUST A NUMBER. The framework used to hardcode `- 8`, which is
  // the stock PSY-Q crt0's `addi v0,v0,-8`. MEASURED over all six of this workspace's executables with
  // `tools/crt0_extract` (which calls the same `crt0_scan` the boot-time audit uses, so the extractor
  // and the gate cannot drift): FOUR bias by -8 (Tomba!2, Spyro, Spider-Man, Vagrant Story) and TWO do
  // not bias at all — Mega Man X4 AND Toy Story 2, whose measured value is 0. So zero cannot mean
  // "unset" for this field the way it does for every address in GameConfig — 0 is a real answer —
  // and a plain `int32_t` defaulting to 0 would silently move sp 8 bytes on the four ports that DO
  // bias, while defaulting to -8 would silently mis-boot every future port that does not. Both
  // silent. Hence: `declared` must be set to 1 by the consumer, and `crt0_plan` REFUSES a boot where
  // it is 0, naming the field. Loud is recoverable; either default is not.
  //
  // Appended at the END of the struct, like paceQuota/windowTitle above, because GameConfig is
  // initialised POSITIONALLY by some consumers (spider1) — a field inserted into the crt0 group at
  // the top would shift every value after it there, silently.
  struct Crt0StackBias {
    uint32_t declared; // 1 = this game has stated its bias (even if the bias is 0). 0 = crt0 refuses.
    int32_t value;     // the measured adjustment, e.g. -8 for the stock PSY-Q crt0, 0 for X4.
  } stackBias;

  // ── the scheduler's guest task ENTRY PCs, declared by the game ─────────────────────────────────
  // P1.7c moved PcScheduler into the framework with the game supplying task BODIES through
  // `hooks->schedStageBody(kind)`. The mapping from a guest task's entry PC to which body it is did NOT
  // move with it: `pc_scheduler.cpp` tested eight Tomba!2 literals directly
  // (`entry_pc == 0x801062E4` and friends). That made the framework schedule ONE game specially, left
  // those branches dead for the other five ports, and gave a new port no way to say "this entry is my
  // area-load callback". This table is the missing half of that seam — the noun to go with the verb.
  //
  // A game that declares nothing gets `schedEntryCount == 0`, which is exactly what the other five
  // ports experience today (the literals never matched), so adding this changes no port's behaviour.
  //
  // `nativeHandler` and `fiberBody` are separate because the two call sites ask different questions:
  // `hasNativeHandlerForEntry` asks "do the native per-frame stanzas own this entry at all", while the
  // fresh-fiber stanza asks "which coro body do I start for it". An entry can answer yes to the first
  // and have no fiber body (the DEMO/GAME dispatchers), or the reverse (the preload bodies), so one
  // flag could not express both without a sentinel that reads as a real value.
  struct SchedEntry {
    uint32_t pc;            // the guest task entry PC, from `mem_r32(taskbase + 0xc)`
    uint32_t nativeHandler; // 1 = the native per-frame stanzas handle this entry
    uint32_t hasFiberBody;  // 1 = a fresh task at this entry starts a coro body
    SchedBody fiberBody;    // which body — meaningful ONLY when hasFiberBody is 1
  };
  const SchedEntry *schedEntries; // may be null when the count is 0
  uint32_t schedEntryCount;

  // Appended for positional consumers. Zero means SynchronousTaskWait is unmeasured and refuses.
  uint32_t syncWaitDoneFlag, syncWaitParam2, syncWaitParam3;
  uint32_t syncWaitTaskGp, syncWaitForceCloseRa, syncWaitSpawnRa, syncWaitFinishRa;

  // --- THE GUEST'S OWN DISPLAY HEIGHT, in scanlines (0 = not declared) --------------------------
  // How many lines the game really scans out. Normally the GPU knows: GP1(0x07) programs the vertical
  // display range and gpu_gp1 decodes it. But a port that HLEs libgpu's display setup may leave that
  // register never written — measured on Tomba!2, 2026-08-19: 16,061 GP1 writes in a session and not
  // one of them is 05, 07 or 08 — and then `s_disp_h` is the framework's 240-line DEFAULT, which is a
  // guess wearing the costume of a measurement.
  //
  // The cost of that guess is visible: Tomba!2 is a 320x224 game (its cutscene letterbox rects sit at
  // rows 0..11 and 212..223, flush to the bottom of a 224-line screen — see the RE note in the
  // consumer's game/render/cine_bars.cpp), so presenting 240 lines put 16 rows of framebuffer BELOW the
  // bottom bar on screen, rows a console never scans out. The USER reported it as "a bogus segment
  // below the cutscene black bars".
  //
  // Applies to the GUEST-SOURCED render paths only (gte / psx), which are the ones claiming to show
  // what the console showed. A native renderer owns its own frame and may present more (USER,
  // 2026-08-19, on exactly this: "PC is fine, oracle isn't"). Unset leaves every path on the decoded
  // GP1 value, which is the correct behaviour for a game whose GPU registers are really programmed.
  // Appended at the end: GameConfig is initialised POSITIONALLY by some consumers.
  uint16_t guestDisplayHeight;
};

// Look up a task entry PC in the game's declared table. Returns null when the game declared none or the
// PC is not one of them — the caller must treat null as "not mine", which is the same fall-through the
// hardcoded comparisons produced. Kept inline in the seam header so both the framework and the game read
// one definition of what a declared entry means.
static inline const GameConfig::SchedEntry *sched_entry_for(const GameConfig *cfg, uint32_t entry_pc) {
  if (!cfg || !cfg->schedEntries) {
    return nullptr;
  }
  for (uint32_t i = 0; i < cfg->schedEntryCount; i++) {
    if (cfg->schedEntries[i].pc == entry_pc) {
      return &cfg->schedEntries[i];
    }
  }
  return nullptr;
}

#endif // __cplusplus
